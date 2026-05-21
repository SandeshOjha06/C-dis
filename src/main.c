#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "store.h"

void server_run(int server_fd, HashTable *ht);

int main(void) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int val = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &val, sizeof(val)) == -1) {
        perror("setsockopt"); return 1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(6379);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen"); return 1;
    }

    printf("KV store listening on port 6379\n");

    HashTable *ht = ht_create();
    if (!ht) { fprintf(stderr, "ht_create failed\n"); return 1; }

    server_run(server_fd, ht);

    ht_destroy(ht);
    close(server_fd);
    return 0;
}