#include "runtime.hpp"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <assert.h>
#include <iostream>
#include <fstream>

// # threads per process.
#ifndef THREADS_NUM
#define THREADS_NUM 4
#endif
#define MATRIX_DIM (128ull*1024)              // Matrix dimensions
#define FACTORS_DIM (4ull*1024)              // Number of latent factors
#define ITERATIONS 800                // Number of optimization iterations
#define LEARNING_RATE 0.01          // Learning rate for gradient descent
#define REGULARIZATION 0.01         // Regularization parameter

// Build variants (see scripts/eval.sh): -DSINGLE_PROCESS reads the
// per-thread-count input for the single-process baseline; -DSHARE_EVERYTHING
// routes every allocation into the coherent shared region (shared-heap baseline).

#ifdef SHARE_EVERYTHING
#define malloc malloc_shared
#endif


// For fixed-point arithmetic (convert float to int)
#define FIXED_POINT_MULTIPLIER 1000

std::string filename_prefix = "data/matrix-1024M-128K-";
std::string filename_postfix = "t.bin";
std::string filename;

// Original sparse matrix entry
struct SparseEntry {
    uint32_t row;
    uint32_t col;
    int value;  // Using int instead of float
};

// Thread argument structure
struct ThreadArg {
    int thread_id;
    int **row_factor_array;
};

// The partition of sparse matrix for the process
SparseEntry* sparse_entries[THREADS_NUM] = {NULL};
int fw_server_idx = 0;
int fw_server_size = 0;
int total_thread_num = 0;
unsigned long rows_per_process = 0;
unsigned long rows_per_thread = 0;
unsigned long entries_per_thread = 0;
int* local_row_factors = NULL;
int* local_col_factors = NULL;
// An array storing the pointer to row factors so it can be aggregated globally.
// set in stage 1
// used in stage 2
int **row_factor_array = NULL;

// Read input file into a buffer
// Returns number of elements read, or -1 on error
unsigned long read_matrix_file(const std::string& filename, SparseEntry** buffer) {
    // Open the binary file for reading
    std::ifstream infile(filename, std::ios::binary);
    if (!infile) {
        std::cerr << "Failed to open file: " << filename << std::endl;
        return -1;
    }

    // Read the first 4 bytes to get the number of elements
    unsigned long num_elements;
    infile.read(reinterpret_cast<char*>(&num_elements), sizeof(int32_t));
    
    if (infile.fail()) {
        std::cerr << "Failed to read element count from file: " << filename << std::endl;
        infile.close();
        return -1;
    }

    // Allocate memory for the buffer
    *buffer = (SparseEntry*)malloc(sizeof(SparseEntry)*num_elements);
    if (*buffer == NULL) {
        std::cerr << "Failed to allocate memory for " << num_elements << " elements" << std::endl;
        infile.close();
        return -1;
    }

    // Read the entire rest of the file into the buffer
    infile.read(reinterpret_cast<char*>(*buffer), num_elements * sizeof(SparseEntry));
    
    if (infile.fail()) {
        std::cerr << "Failed to read " << num_elements << " elements from file: " << filename << std::endl;
        delete[] *buffer;
        *buffer = nullptr;
        infile.close();
        return -1;
    }

    infile.close();
    std::cout << "Successfully read " << num_elements << " elements from " << filename << std::endl;
    return num_elements;
}

// Stage 1: Generate and process sparse matrix entries in thread-local storage
void* stage1_process_blocks(void* arg) {
    ThreadArg* thread_arg = (ThreadArg*)arg;
    int thread_id = thread_arg->thread_id;
    // With **round-robin** allocation
    // Assume 4 nodes
    // Node 0 get thread 0, 4, 8 12
    // Node 1 get thread 1, 5, 9, 13
    // So the local sparse_entries to use should be (thread_id / 4)
    uint32_t local_thread_id = thread_id / fw_server_size;
    SparseEntry* local_entries = sparse_entries[local_thread_id];

    if (local_thread_id == 0) {
        (thread_arg->row_factor_array)[fw_server_idx] = local_row_factors;
    }
    
    // Calculate row range for this thread
    uint32_t start_row = thread_id * rows_per_thread;
    uint32_t end_row = start_row + rows_per_thread;

    // Generate a thread-local row factors to be used later
    int* row_sums = (int*)malloc((end_row - start_row) * sizeof(int));
    int* row_counts = (int*)malloc((end_row - start_row) * sizeof(int));
    // First pass: collect statistics from sparse entries
    for (uint32_t i = 0; i < entries_per_thread; i++) {
        // The original data only contain row offset, update it to have to full row
        local_entries[i].row += start_row;
        uint32_t row = local_entries[i].row;
        int value = local_entries[i].value;
        
        // Only process rows that belong to this thread
        if (row >= start_row && row < end_row) {
            int local_row_idx = row - start_row;
            row_sums[local_row_idx] += value;
            row_counts[local_row_idx]++;
        }
    }

    // Second pass: initialize row factors based on collected statistics
    for (uint32_t r = 0; r < (end_row - start_row); r++) {
        
        // Calculate average value for this row (if there are any entries)
        int base_value = 0;
        if (row_counts[r] > 0) {
            base_value = row_sums[r] / row_counts[r];
        } else {
            // Default value if no entries for this row
            base_value = 500;
        }
        
        // Initialize row factors with some variation based on the average value
        for (uint32_t f = 0; f < FACTORS_DIM; f++) {
            // Use the base value with some variation based on factor index
            int factor_value = base_value * (0.8 + 0.4 * (f % 10) / 10.0);
            
            // Store the factor value
            local_row_factors[(local_thread_id * rows_per_thread + r) * FACTORS_DIM + f] = factor_value;
        }
    }
    
    // Wait for all threads to finish stage 1
    remote_threads_barrier_wait();
    return NULL;
}

// Stage 2: Process global model and prepare for optimization
void* stage2_merge_global(void* arg) {
    
    // Only one thread normalizes the global row factors to ensure consistency
    printf("Normalizing global row factors...\n");

    
    // Normalize global row factors
    for (int server_id = 0; server_id < fw_server_size; server_id++) {
        int *row_factor = row_factor_array[server_id];
        for (size_t row = 0; row < rows_per_process; row++) {
            // Calculate the L2 norm of this row
            long long sum_squares = 0;
            for (size_t f = 0; f < FACTORS_DIM; f++) {
                int factor = row_factor[row * FACTORS_DIM + f];
                sum_squares += (long long)factor * factor;
            }
            
            // Avoid division by zero
            if (sum_squares > 0) {
                // Calculate normalization factor (using fixed-point)
                int norm_factor = (int)(FIXED_POINT_MULTIPLIER * sqrt(FIXED_POINT_MULTIPLIER) / sqrt((double)sum_squares));
                
                // Normalize the row
                for (size_t f = 0; f < FACTORS_DIM; f++) {
                    row_factor[row * FACTORS_DIM + f] = 
                        (row_factor[row * FACTORS_DIM + f] * norm_factor) / FIXED_POINT_MULTIPLIER;
                }
            }
        }
    }
    
    return NULL;
}

// Stage 3: Each thread optimizes a partition of the global factor matrix
void* stage3_optimize_factors(void* arg) {
    ThreadArg* thread_arg = (ThreadArg*)arg;
    int thread_id = thread_arg->thread_id;
    int* gradients = (int*)malloc(rows_per_thread * FACTORS_DIM * sizeof(int));
    uint32_t local_thread_id = thread_id / fw_server_size;
    SparseEntry* local_entries = sparse_entries[local_thread_id];
    
    // Each thread is responsible for a partition of rows in the global matrix
    int start_row = thread_id * rows_per_thread;
    int end_row = start_row + rows_per_thread;

#ifdef FW_PHASE_HINT
    // Optional application-side hint at the phase boundary: ask the runtime
    // to run an unsharing pass now (firework_enable_unsharing). Off by
    // default; build with -DFW_PHASE_HINT to compare against the hinted
    // variant used in earlier measurements.
    syscall(492);
#endif

    // Memory-intensive optimization stage
    for (int iter = 0; iter < ITERATIONS; iter++) {
        // Reset gradients
        memset(gradients, 0, rows_per_thread * FACTORS_DIM * sizeof(int));
        
        // Process all sparse entries to compute gradients for row factors
        for (uint32_t i = 0; i < entries_per_thread; i++) {
            uint32_t row = local_entries[i].row;
            uint32_t col = local_entries[i].col;
            int value = local_entries[i].value;
            
            // We know all entries are already in this thread's row range
            int local_row = row - start_row;
            
            // Compute prediction error using fixed-point arithmetic
            int prediction = 0;
            for (size_t f = 0; f < FACTORS_DIM; f++) {
                // Use fixed-point arithmetic - divide by multiplier after multiplication
                prediction += (local_row_factors[(local_thread_id * rows_per_thread + local_row) * FACTORS_DIM + f] * local_col_factors[col * FACTORS_DIM + f]) / FIXED_POINT_MULTIPLIER;
            }
            int error = value - prediction;
            
            // Compute gradients for row factors
            for (size_t f = 0; f < FACTORS_DIM; f++) {
                int col_factor = local_col_factors[col * FACTORS_DIM + f];
                
                // Update gradient (includes regularization)
                gradients[local_row * FACTORS_DIM + f] += 
                    (error * col_factor) / FIXED_POINT_MULTIPLIER -
                    (REGULARIZATION * FIXED_POINT_MULTIPLIER * 
                    local_row_factors[(local_thread_id * rows_per_thread + local_row) * FACTORS_DIM + f]) / FIXED_POINT_MULTIPLIER;
            }
        }
        
        // Apply gradients to the row factors
        for (int row = start_row; row < end_row; row++) {
            int local_row = row - start_row;
            
            for (size_t f = 0; f < FACTORS_DIM; f++) {
                int update = (LEARNING_RATE * FIXED_POINT_MULTIPLIER * 
                             gradients[local_row * FACTORS_DIM + f]) / FIXED_POINT_MULTIPLIER;
                
                // Direct update - no synchronization needed
                local_row_factors[(local_thread_id * rows_per_thread + local_row) * FACTORS_DIM + f] += update;
            }
        }
        
        // Periodically perform intensive computation on assigned rows
        // This creates a memory-intensive workload on the assigned partition
        // Sync between iterations
        remote_threads_barrier_wait();
        
        if (thread_id == 0 && iter % 10 == 0) {
            printf("Completed iteration %d\n", iter);
        }
    }
    end_th();
    
    return NULL;
}

// Main benchmark function
void benchmark() {
    row_factor_array = (int**)malloc(fw_server_size * sizeof(int*));
    memset(row_factor_array, 0, fw_server_size * sizeof(int*));
    
    struct timespec start, end;
    struct timespec bench_start, bench_end;
    double time_spent;
    
    // Create thread barrier
    remote_threads_barrier_init(total_thread_num);
    
    // Create threads and their local models
    pthread_t *threads = (pthread_t*)malloc(sizeof(pthread_t) * total_thread_num);
    ThreadArg** thread_args = (ThreadArg**)malloc(sizeof(ThreadArg*) * total_thread_num);
    for (int i = 0; i < total_thread_num; i++) {
        thread_args[i] = (ThreadArg*)malloc(sizeof(ThreadArg));
    }
    
    // Initialize thread arguments and allocate local models
    for (int i = 0; i < total_thread_num; i++) {
        thread_args[i]->thread_id = i;
        thread_args[i]->row_factor_array = row_factor_array;
    }

    clock_gettime(CLOCK_MONOTONIC, &bench_start);

    // Stage 1: Generate sparse entries and initial row factors
    printf("Starting Stage 1: Local Processing and Initial Factor Generation\n");
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < total_thread_num; i++) {
        int target = i % fw_server_size;
        threads[i] = remote_pthread_create(target, (void*)stage1_process_blocks, thread_args[i]);
    }
    
    for (int i = 0; i < total_thread_num; i++) {
        int target = i % fw_server_size;
        remote_pthread_join(threads[i], target);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    time_spent = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    printf("Stage 1 completed in %.2f seconds\n", time_spent);
    
    // Stage 2: Normalize row factors
    printf("Starting Stage 2: Global Model Processing\n");
    clock_gettime(CLOCK_MONOTONIC, &start);
    stage2_merge_global(NULL);
    clock_gettime(CLOCK_MONOTONIC, &end);
    time_spent = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    printf("Stage 2 completed in %.2f seconds\n", time_spent);
    
    // Stage 3: Each thread optimizes its partition of the global row factors
    printf("Starting Stage 3: Partition Optimization\n");
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < total_thread_num; i++) {
        int target = i % fw_server_size;
        threads[i] = remote_pthread_create(target, (void*)stage3_optimize_factors, thread_args[i]);
    }
    
    for (int i = 0; i < total_thread_num; i++) {
        int target = i % fw_server_size;
        remote_pthread_join(threads[i], target);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    time_spent = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    printf("Stage 3 completed in %.2f seconds\n", time_spent);

    clock_gettime(CLOCK_MONOTONIC, &bench_end);
    time_spent = (bench_end.tv_sec - bench_start.tv_sec) + (bench_end.tv_nsec - bench_start.tv_nsec) / 1e9;
    printf("Bench completed in %.2f seconds\n", time_spent);
    
    printf("Benchmark completed\n");
    show_debug_fs();

    return;
}

int main() {
    // Initialize random seed
    srand(time(NULL));
    
    // Adapt to your firework runtime
    const char* fw_server_idx_ch = getenv("FW_SERVER_IDX");
    const char* fw_server_size_ch = getenv("FW_SERVER_SIZE");
    fw_server_idx = atoi(fw_server_idx_ch);
    fw_server_size = atoi(fw_server_size_ch);
    firework_init(fw_server_idx, fw_server_size, 1000, 1);

    rows_per_process = MATRIX_DIM / fw_server_size;
    rows_per_thread = rows_per_process / THREADS_NUM;
    total_thread_num = fw_server_size * THREADS_NUM;

    // Load the sparse matrix from file
    // The file store the non-zero elements for each thread
    // In the format of (row index, col, value)
    // where row index is a random value between [0, #rows_per_thread]
    // This should be duplicated for all threads within the same process
    // The row_start will be added to the sparse entries in stage 1
#ifdef SINGLE_PROCESS
    filename = filename_prefix + std::to_string(THREADS_NUM) + filename_postfix;
#else
    filename = filename_prefix + std::to_string(4 * fw_server_size) + filename_postfix;
#endif
    unsigned long entries_per_thread = read_matrix_file(filename, &(sparse_entries[0]));
    if (entries_per_thread == (unsigned long)-1) {
        printf("Failed to read matrix file\n");
        return -1;
    }
    for (int i = 1; i < THREADS_NUM; i++) {
        sparse_entries[i] = (SparseEntry*)malloc(sizeof(SparseEntry) * entries_per_thread);
        memcpy(sparse_entries[i], sparse_entries[0], sizeof(SparseEntry) * entries_per_thread);
    }

    local_row_factors = (int*)malloc(rows_per_process * FACTORS_DIM * sizeof(int));
    local_col_factors = (int*)malloc(MATRIX_DIM * FACTORS_DIM * sizeof(int));
    memset(local_row_factors, 1, rows_per_process * FACTORS_DIM * sizeof(int));
    // the col factors are prefilled
    memset(local_col_factors, 1, MATRIX_DIM * FACTORS_DIM * sizeof(int));
    
    if (fw_server_idx == fw_server_size - 1) {
        benchmark();
    } else {
        sleep(10000);
    }
    
    return 0;
}