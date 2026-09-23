#include <stdio.h>
#include "store.h"

void parse_commands(char *buf, int client_fd, HashTable *ht) {
    if (client_fd != -1) {
        printf("[parser] Received data on fd %d: %s\n", client_fd, buf);
    } else {
        printf("[parser] Replaying WAL: %s\n", buf);
    }
}
