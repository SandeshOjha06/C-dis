#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

#include "store.h"
#include "persist.h"

extern int my_node_id;
extern int cluster_size;

// Forward declarations from server_epoll.c
extern int peer_fd_for_node(int node_id);
extern int get_connection_role(int fd);
extern int get_peer_node_id(int fd);
extern bool forward_to_peer_node(int target_node, const void *data, size_t len);

typedef enum { ROLE_UNKNOWN = 0, ROLE_CLIENT, ROLE_PEER } conn_role_t;

static void respond(int fd, const char *msg) {
    if (fd == -1) return;   // replay mode - don't send responses
    write(fd, msg, strlen(msg));
}

static unsigned int get_target_node(const char *key) {
    unsigned long int hashval = 5381;
    int c;
    while ((c = *key++)) {
        hashval = ((hashval << 5) + hashval) + c;
    }
    return hashval % cluster_size;
}

void parse_commands(char *buf, int client_fd, HashTable *ht) {
    // Build the forward copy. The read path now strips the line terminator,
    // so append '\n' explicitly — the peer's line-based parser requires it.
    char raw_cmd[5120];
    int raw_len = snprintf(raw_cmd, sizeof(raw_cmd), "%s\n", buf);
    if (raw_len < 0 || raw_len >= (int)sizeof(raw_cmd)) return;

    char *saveptr;
    char *cmd = strtok_r(buf, " ", &saveptr);  // split on space
    if (cmd == NULL) return;

    char *key = strtok_r(NULL, " ", &saveptr);
    if (key != NULL) {
        int target_node = get_target_node(key);

        if (target_node != my_node_id) {
            printf("Routing key '%s' to Node %d\n", key, target_node);

            conn_role_t role = (conn_role_t)get_connection_role(client_fd);

            if (role == ROLE_CLIENT) {
                // Forward once to the target peer (non-blocking via egress buffer)
                if (!forward_to_peer_node(target_node, raw_cmd, (size_t)raw_len)) {
                    respond(client_fd, "-ERR peer unreachable or buffer full\n");
                }
                return;
            } else if (role == ROLE_PEER) {
                // Forwarding loop detected - peer trying to forward to another peer
                respond(client_fd, "-ERR forwarding loop detected\n");
                return;
            }
            // ROLE_UNKNOWN shouldn't happen for commands, but treat as client
        }
    }

    if (strcmp(cmd, "SET") == 0) {
        char *val = strtok_r(NULL, " ", &saveptr);
        if (!key || !val) {
            respond(client_fd, "-ERR SET requires key and value\n");
            return;
        }
        if (g_log_fd != -1) {
            char logline[512];
            snprintf(logline, sizeof(logline), "SET %s %s\n", key, val);
            wal_write(g_log_fd, logline);
        }

        ht_set(ht, key, val);
        respond(client_fd, "+OK\n");
    }
    else if (strcmp(cmd, "GET") == 0) {
        if (!key) {
            respond(client_fd, "-ERR GET requires key\n");
            return;
        }

        char *found = ht_get(ht, key);
        if (found == NULL) {
            respond(client_fd, "-ERR key not found\n");
        } else {
            char resp[512];
            snprintf(resp, sizeof(resp), "$%s\n", found);
            respond(client_fd, resp);
        }
    }
    else if (strcmp(cmd, "DEL") == 0) {
        if (!key) {
            respond(client_fd, "-ERR DEL requires key\n");
            return;
        }

        if (g_log_fd != -1) {
            char logline[256];
            snprintf(logline, sizeof(logline), "DEL %s\n", key);
            wal_write(g_log_fd, logline);
        }

        if (ht_del(ht, key)) {
            respond(client_fd, ":1\n");
        } else {
            respond(client_fd, ":0\n");
        }
    }
    else {
        respond(client_fd, "-ERR unknown command\n");
    }
}