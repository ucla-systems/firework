#include "runtime.hpp"
#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <chrono>
#include <string.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>

#define MAX_ITERATIONS 200
#ifndef THREADS_PER_PROCESS
#define THREADS_PER_PROCESS 4
#endif


#ifdef SHARE_EVERYTHING
#define malloc malloc_shared
#endif

int fw_server_size;
int total_thread_num = 0;

struct GraphData {
    int id;
    int num;
};

struct ArgData {
    int idx;
    int start;
    int end;
    double* pagerank;
#ifdef SHARE_EVERYTHING
    struct GraphData* graph_data;
    int* offsets;
#endif
};

struct GraphData* graph_data = NULL;
int* offsets = NULL;

int num_nodes;
int num_edges;


pthread_barrier_t barrier;

ssize_t full_read(int fd, void *buf, size_t count) {
    size_t bytes_read = 0;
    char *ptr = (char *)buf;

    while (bytes_read < count) {
        ssize_t ret = read(fd, ptr + bytes_read, count - bytes_read);
        if (ret < 0) return -1;       // Error
        if (ret == 0) break;          // EOF
        bytes_read += ret;
    }

    return bytes_read;
}

void* pagerank_th(void* arg) {
    struct ArgData* data = (struct ArgData*)arg;
    int idx = data->idx;
    int start = data->start;
    int end = data->end;
    int ops = end - start;
    double* pagerank = data->pagerank;
#ifdef SHARE_EVERYTHING
    if (graph_data == NULL)
        graph_data = data->graph_data;
    if (offsets == NULL)
        offsets = data->offsets;
#endif

    double *new_pagerank = (double*)malloc(sizeof(double) * ops);

    for (int iter=0; iter<MAX_ITERATIONS; iter++) {
        memset(new_pagerank, 0, sizeof(double) * ops);

        for (int node=start; node<end; node++) {
            int next_offset = (node+1 < num_nodes) ? offsets[node+1] : num_edges;
            
            for (int j = offsets[node]; j < next_offset; j++) {
                int incoming_node = graph_data[j].id;
                int outgoing_edges = graph_data[j].num;
                if (outgoing_edges > 0) {
                    new_pagerank[node-start] += pagerank[incoming_node] / outgoing_edges;
                }
            }
        }

        remote_threads_barrier_wait();

        for (int i=0; i<ops; i++) {
            pagerank[i+start] = new_pagerank[i];
        }
        remote_threads_barrier_wait();

        if (idx == 0 && iter%10 == 0) printf("Iteration %d\n", iter);
    }

    end_th();

    return NULL;
}

int partition(int start, int limit) {
    int end;

    for (end=start; end<num_nodes; end++) {
        if (offsets[end] >= limit) {
            if (end==start) return end+1;
            else return end;
        }
    }
    
    return end; // num_nodes
}


void calculatePageRank() {
    double *pagerank = (double*)malloc(sizeof(double) * num_nodes);
    for (int i = 0; i < num_nodes; i++) {
        pagerank[i] = 1.0;
    }
    remote_threads_barrier_init(total_thread_num);

    pthread_t *threads = (pthread_t*)malloc(sizeof(pthread_t) * total_thread_num);
    int start = 0;
    for (int i=0; i<total_thread_num; i++) {
        int end = partition(start, num_edges / total_thread_num * (i+1));
        struct ArgData* arg = (struct ArgData*)malloc(sizeof(struct ArgData));
        arg->idx = i;
        arg->start = start;
        arg->end = end;
        arg->pagerank = pagerank;
#ifdef SHARE_EVERYTHING
        arg->graph_data = graph_data;
        arg->offsets = offsets;
#endif
        int target = i % fw_server_size;
        threads[i] = remote_pthread_create(target, (void*)pagerank_th, arg);
        start = end;
    }

    for (int i=0; i<total_thread_num; i++) {
        int target = i % fw_server_size;
        remote_pthread_join(threads[i], target);
    }

}

void read_graph() {
    // Dataset location is configurable so the artifact is not tied to a
    // fixed absolute path. Set PAGERANK_DATA_DIR to the directory holding
    // incoming_en.txt / offsets_en.txt (default: ./data).
    const char* data_dir = getenv("PAGERANK_DATA_DIR");
    if (!data_dir) data_dir = "./data";
    static char incoming_path[4096], offsets_path[4096];
    snprintf(incoming_path, sizeof(incoming_path), "%s/incoming_en.txt", data_dir);
    snprintf(offsets_path, sizeof(offsets_path), "%s/offsets_en.txt", data_dir);
    const char* incoming_file = incoming_path;
    const char* offsets_file = offsets_path;
    ssize_t read_ret;

    int fd = open(incoming_file, O_RDONLY);
    int fd2 = open(offsets_file, O_RDONLY);

    if (fd < 0 || fd2 < 0) {
        std::cout << "Error opening file" << std::endl;
        return;
    }

    read_ret = full_read(fd, &num_edges, sizeof(int));
    if (read_ret != sizeof(int)) {
        printf("%s: expected %lu bytes, got %ld bytes\n", incoming_file, sizeof(int), read_ret);
        goto exit;
    }
    read_ret = full_read(fd2, &num_nodes, sizeof(int));
    if (read_ret != sizeof(int)) {
        printf("%s: expected %lu bytes, got %ld bytes\n", offsets_file, sizeof(int), read_ret);
        goto exit;
    }


    graph_data = (struct GraphData*)malloc(sizeof(struct GraphData) * num_edges);
    offsets = (int*)malloc(sizeof(int) * num_nodes);

    read_ret = full_read(fd, graph_data, sizeof(struct GraphData) * num_edges);
    if (read_ret != sizeof(struct GraphData) * num_edges) {
        printf("Error reading graph data: expected %lu bytes, got %ld bytes\n", sizeof(struct GraphData) * num_edges, read_ret);
        goto exit;
    }
    read_ret = full_read(fd2, offsets, sizeof(int) * num_nodes);
    if (read_ret != sizeof(int) * num_nodes) {
        printf("Error reading offsets: expected %lu bytes, got %ld bytes\n", sizeof(int) * num_nodes, read_ret);
        goto exit;
    }

    printf("num_nodes: %d\n", num_nodes);
    printf("num_edges: %d\n", num_edges);
exit:
    close(fd);
    close(fd2);
    return;
}

int main() {
    const char* fw_server_idx_ch = getenv("FW_SERVER_IDX");
    const char* fw_server_size_ch = getenv("FW_SERVER_SIZE");
    int fw_server_idx = atoi(fw_server_idx_ch);
    fw_server_size = atoi(fw_server_size_ch);
    total_thread_num = fw_server_size * THREADS_PER_PROCESS;
    firework_init(fw_server_idx, fw_server_size, 1000, 1);

#ifdef SHARE_EVERYTHING
    if (fw_server_idx == fw_server_size - 1) {
        read_graph();
    }
#else
    read_graph();
#endif

    if (fw_server_idx == fw_server_size - 1) {
        auto start = std::chrono::high_resolution_clock::now();
        calculatePageRank();
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> elapsed = end - start;
        std::cout << "Elapsed time: " << elapsed.count() / 1000 << "s\n";
    } else {
        sleep(10000);
    }

    show_debug_fs();

    return 0;
}