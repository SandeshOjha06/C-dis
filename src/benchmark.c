#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>

#define PORT 6379
#define NUM_PAIRS 20000  // 20k SETs and 20k GETs = 40k total operations

int main() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return 1;
    }

    struct sockaddr_in serv_addr = {0};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection to kvstore failed. Is the server running?");
        return 1;
    }

    char send_buf[256];
    char recv_buf[4096];
    struct timespec start, end;

    printf(" Starting Benchmark: Firing %d command pairs...\n", NUM_PAIRS);

    // Grab the absolute start time before execution begins
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < NUM_PAIRS; i++) {
        // 1. Pipeline a SET command
        int set_len = snprintf(send_buf, sizeof(send_buf), "SET bench_key_%d val_%d\n", i, i);
        send(sock, send_buf, set_len, 0);
        
        // Synchronously read the server response to keep it sequential
        int n1 = recv(sock, recv_buf, sizeof(recv_buf) - 1, 0);
        if (n1 <= 0) break;

        // 2. Pipeline a GET command
        int get_len = snprintf(send_buf, sizeof(send_buf), "GET bench_key_%d\n", i);
        send(sock, send_buf, get_len, 0);
        
        int n2 = recv(sock, recv_buf, sizeof(recv_buf) - 1, 0);
        if (n2 <= 0) break;
    }

    // Grab the absolute end time immediately after the loop stops
    clock_gettime(CLOCK_MONOTONIC, &end);

    // Calculate total elapsed time in seconds
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1000000000.0;
    int total_cmds = NUM_PAIRS * 2;
    double rps = total_cmds / elapsed;

    printf("\n --- BENCHMARK RESULTS ---\n");
    printf("Total Time Elapsed:  %.4f seconds\n", elapsed);
    printf("Total Commands Run:  %d\n", total_cmds);
    printf("Throughput Metrics:  \033[1;32m%.2f requests/second\033[0m\n", rps);

    close(sock);
    return 0;
}