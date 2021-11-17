#include <unistd.h>
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include "tasks.h"
#include "utils.h"

#define FILE_PATH_SIZE 100

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int world_size, rank, i;
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    // Get command-line params
    char *input_files_dir = argv[1];
    int num_files = atoi(argv[2]);
    int num_map_workers = atoi(argv[3]);
    int num_reduce_workers = atoi(argv[4]);
    char *output_file_name = argv[5];
    int map_reduce_task_num = atoi(argv[6]);

#if DEBUG
    // Initialise the wall clock time variables
    double mpi_start = MPI_Wtime();
#endif

    // Identify the specific map function to use
    MapTaskOutput* (*map) (char*);
    switch(map_reduce_task_num){
        case 1:
            map = &map1;
            break;
        case 2:
            map = &map2;
            break;
        case 3:
            map = &map3;
            break;
    }

    // Create two distinct groups of worker processes
    MPI_Group map_group, reduce_group, world_group;
    MPI_Comm map_comm, reduce_comm;

    // Create an array distinguishing the processes
    MPI_Comm_group(MPI_COMM_WORLD, &world_group);
    
    // Initialisation of different values
    int total_buffer_size, individual_workload;
    char * total_workload;
    printf("Process %d, rank %d\n", getpid(), rank);
    // Distinguish the different ranks
    if (rank == 0) {
        printf("Rank (%d): This is the master process\n", rank);
        if (map_comm == MPI_COMM_NULL) 
          printf("Error!\n");
        
        // Initialising phase gather all the files
        total_buffer_size = 0;

        for (i = 0; i < num_files; i++) 
        {
            // Open the files based on the index
            char file_str[FILE_PATH_SIZE];
            sprintf(file_str, "./%s/%d.txt", input_files_dir, i);
            printf("File: %s\n", file_str);
            FILE * fp = fopen(file_str, "r");

            // Get file size
            fseek(fp, 0L, SEEK_END);
            total_buffer_size += ftell(fp);
            fseek(fp, 0L, SEEK_SET);
        }
        
        // malloc a whole buffer
        total_workload = malloc(total_buffer_size + 1);
        memset(total_workload, 0, total_buffer_size + 1);
        total_buffer_size++;
        char * p = total_workload;

        for (i = 0; i < num_files; i++) 
        {
            // Open the files based on the index
            char file_str[FILE_PATH_SIZE];
            sprintf(file_str, "./%s/%d.txt", input_files_dir, i);
            FILE * fp = fopen(file_str, "r");

            // Determine the size of the send buffer based on the file size
            fseek(fp, 0L, SEEK_END);
            int fileSize = ftell(fp);
            fseek(fp, 0L, SEEK_SET);

            // Open a file path object
            fread(p, fileSize, 1, fp);
            p += fileSize;
            fclose(fp);
        }
        total_workload[total_buffer_size - 1] = '\0';

        // Create broadcast before scatter
        MPI_Bcast(&total_buffer_size, 1, MPI_INT, 0, MPI_COMM_WORLD);
        
        // Not entirely possible to create a scatter mechanism (collective communication)
        // as it'll imply the master process which is not involved in mapping
        individual_workload = (total_buffer_size + num_map_workers - 1) / num_map_workers;
        int current_balance_workload = total_buffer_size;
        int global_i = 0;
        int slave_id = 1;
        while (current_balance_workload > 0) {
            int old_balance = current_balance_workload;
            current_balance_workload -= individual_workload;
            int workload = current_balance_workload < 0 ? old_balance : individual_workload;
            char * send_buffer = malloc(workload);
            memset(send_buffer, 0, workload);
            for (int j = 0; j < individual_workload; j++) {
                send_buffer[j] = total_workload[global_i];
                global_i++;
            }
     		MPI_Send(send_buffer, workload, MPI_CHAR, slave_id, slave_id, MPI_COMM_WORLD);
            printf("Rank (%d): Outgoing buffer size %d\n", rank, workload);
            slave_id++;
        }

        int temp, send_rank; 
        MPI_Status status;
        for (int reduce_id = num_map_workers + 1; reduce_id < num_reduce_workers + num_map_workers + 1; reduce_id++) {
            // Wait for the next free slave
            send_rank = num_map_workers;
            while (send_rank <= num_map_workers) {
                MPI_Recv(&temp, 1, MPI_INT, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
                send_rank = status.MPI_SOURCE;
            }
        }
        printf("Total execution timing: %f\n", MPI_Wtime() - mpi_start);


    } else if ((rank >= 1) && (rank <= num_map_workers)) {
        // Initialise random variables
        MPI_Status status;

        // Create communicator
        MPI_Bcast(&total_buffer_size, 1, MPI_INT, 0, MPI_COMM_WORLD);
        printf("Rank (%d): Received broadcast: %d\n", rank, total_buffer_size);
        
        // Receiving work
        individual_workload = (total_buffer_size + num_map_workers - 1) / num_map_workers;
        if (rank == num_map_workers) 
            individual_workload = total_buffer_size - ((num_map_workers - 1) * individual_workload);
        char * received_map_workload = malloc(individual_workload);
         
        printf("Rank (%d): Incoming buffer size %d\n", rank, individual_workload);
        MPI_Recv(received_map_workload, individual_workload, MPI_CHAR, 0, rank, MPI_COMM_WORLD, &status); 
        
        // Begin mapping
        MapTaskOutput * output = map(received_map_workload);
        Partitions * partitioned_output = partition_map_task_output(output, num_reduce_workers);       
        for (int i = 0; i < num_reduce_workers; i++) {
            // In this loop, i represents the reduce worker ID
#ifdef DEBUG
            output_map_task_output(partitioned_output->arr[i], rank);
#endif
            MapTaskOutput * temp = partitioned_output->arr[i];
            printf("Rank (%d): temp->len is %d\n", rank, temp->len);
            KeyValue * kvs_arr = temp->kvs;
            int num_kvs = temp->len;
            int stream_size = num_kvs * CHAR_STREAM_LEN;
            unsigned char * send_stream = malloc(stream_size);
            memset(send_stream, 0, stream_size);
            for (int j = 0; j < num_kvs; j++) {
                unsigned char * current_stream = send_stream + j * CHAR_STREAM_LEN;
                keyvalue_to_char_stream(&kvs_arr[j], current_stream);
            }        
            printf("Rank (%d): Sending stream size %d over\n", rank, stream_size);
            MPI_Send(&stream_size, 1, MPI_INT, i + num_map_workers + 1, i, MPI_COMM_WORLD);
            printf("Rank (%d): Sending stream over to rank %d\n", rank, i + num_map_workers + 1);
            MPI_Send(send_stream, stream_size, MPI_UNSIGNED_CHAR, i + num_map_workers + 1, i, MPI_COMM_WORLD);
            free(send_stream);
        }

        free_map_task_output(output);
        free(received_map_workload);
    } else {
        printf("Rank (%d): Reduce worker is starting.\n", rank);

        int stream_size; 
        MPI_Status status;
        unsigned char * received_stream;
       
        // Initialise an array that keeps track of val
        CharCollection * keys = malloc(sizeof(CharCollection)); 
        keys->next = NULL;
        
        // The first index represents the pointer to the current index of the current list 
        IntCollection * val = malloc(sizeof(IntCollection));
        val->len = 0; 
        val->next = NULL;
        keys->val = val;
        strcpy(keys->key, "\0");

        // Initialise an array to keep track of map workers sending information to the reduce worker
        char * map_sizes = malloc(num_map_workers);
        memset(map_sizes, 0, num_map_workers);

        // Create communicator
        MPI_Bcast(&total_buffer_size, 1, MPI_INT, 0, MPI_COMM_WORLD);
        printf("Rank (%d): Receive buffer size %d\n", rank, total_buffer_size);
        
        // Receiving stream
        while (sum_char_array(map_sizes, num_map_workers) != num_map_workers) {
            printf("Rank (%d): sum_char_array: %d, num_map_workers: %d\n", rank, sum_char_array(map_sizes, num_map_workers), num_map_workers);
            MPI_Recv(&stream_size, 1, MPI_INT, MPI_ANY_SOURCE, rank - num_map_workers - 1, MPI_COMM_WORLD, &status);
            map_sizes[status.MPI_SOURCE - 1] = 1;
            received_stream = malloc(stream_size);
            printf("Rank (%d): Receive stream size %d from rank %d\n", rank, stream_size, status.MPI_SOURCE);
            MPI_Recv(received_stream, stream_size, MPI_UNSIGNED_CHAR, status.MPI_SOURCE, rank - num_map_workers - 1, MPI_COMM_WORLD, &status);
            int num_key_value_pairs = stream_size / CHAR_STREAM_LEN;
            printf("Rank (%d): Receive stream of KVP %d from rank %d\n", rank, num_key_value_pairs, status.MPI_SOURCE);
            for (int i = 0; i < num_key_value_pairs; i++) {
                unsigned char key[KEY_LEN];
                memset(key, 0, KEY_LEN);
                strncpy(key, received_stream + i * CHAR_STREAM_LEN, KEY_LEN);
                int value = byte_array_to_int(received_stream + i * CHAR_STREAM_LEN + KEY_LEN);
                CharCollection * iterator_key = keys;
                while (iterator_key != NULL) {
                    // Help to check if the list is currently empty
                    if (!strcmp(iterator_key->key, "\0")) {
                        strncpy(iterator_key->key, key, KEY_LEN); 
                        iterator_key->val->len++;
                        iterator_key->val->val = value;
                        break;
                    } else if (!strcmp(iterator_key->key, key)) {
                        // If there is a match, add it to the current value
                        IntCollection * temp = malloc(sizeof(IntCollection));
                        temp->len = 1;
                        temp->val = value;
                        temp->next = NULL;
                        IntCollection * last = iterator_key->val;
                        while (last->next != NULL) {
                            last = last->next;
                        }
                        last->next = temp;
                        iterator_key->val->len++;
                        break;
                    }
                    
                    // No found key. Create a new kvs 
                    if (iterator_key->next == NULL) {
                        CharCollection * new_key = malloc(sizeof(CharCollection));
                        IntCollection * temp = malloc(sizeof(IntCollection));
                        temp->len = 1;
                        temp->val = value;
                        temp->next = NULL;
                        new_key->val = temp;
                        new_key->next = NULL;
                        strncpy(new_key->key, key, KEY_LEN);
                        iterator_key->next = new_key;
                        break;
                    }
                    iterator_key = iterator_key->next;
                }
            }
        }

        // Completed receiving information from all map workers, now is possible to reduce
        CharCollection * iterator_key = keys;
        while (iterator_key != NULL) {
            IntCollection * val_iterator = iterator_key->val;
            int list_size = val_iterator->len;
            int * val = malloc(list_size * sizeof(int));
            int i = 0;
            while (val_iterator != NULL) {
                val[i] = val_iterator->val;
                val_iterator = val_iterator->next;
                i++;
            }
            free(val_iterator);

            KeyValue output = reduce(iterator_key->key, val, list_size);
            printf("Rank (%d): Key->%s, value->%d\n", rank, output.key, output.val);
            iterator_key = iterator_key->next;
        }
    
        MPI_Send(&stream_size, 1, MPI_INT, 0, rank, MPI_COMM_WORLD);
    }


    //Clean up
    MPI_Finalize();
    return 0;
}
