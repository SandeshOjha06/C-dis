#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_KEYS 50

struct kv_pair {
    char key[64];
    char val[256];
    int in_use;
};

static struct kv_pair database[MAX_KEYS];

void parse_commands(char *buf, int client_fd) {
     // strip trailing \r\n first
    int len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
        buf[--len] = '\0';

    char *saveptr;
    char *cmd = strtok_r(buf, " ", &saveptr);  // split on space
    if (cmd == NULL) return;

    if (strcmp(cmd, "SET") == 0) {
        char *key = strtok_r(NULL, " \r\n", &saveptr);

        char *val = strtok_r(NULL, " \r\n", &saveptr);

        if (!key || !val) {
              write(client_fd, "-ERR SET requires key and value\n",
            strlen("-ERR SET requires key and value\n"));
            return;
        }

        // scan to find an empty slot or update existing key
        for (int i = 0; i < MAX_KEYS; i++) {
            if(!database[i].in_use || strcmp(database[i].key, key) == 0){
                strncpy(database[i].key, key, sizeof(database[i].key) - 1 );
                database[i].key[sizeof(database[i].key) - 1] = '\0';

                strncpy(database[i].val, val, sizeof(database[i].val) - 1);
                database[i].val[sizeof(database[i].val) - 1] = '\0';
                
                database[i].in_use = 1;
                write(client_fd, "+OK\n", 4);
                return;
            }
        }
        write(client_fd, "-ERR database full\n",
              strlen("-ERR database full\n"));
    }

    else if (strcmp(cmd, "GET") == 0) {
        char *key = strtok_r(NULL, " ", &saveptr);

        if (!key) {
            write(client_fd, "-ERR GET requires key\n",
                  strlen("-ERR GET requires key\n"));
            return;
        }

        for (int i = 0; i < MAX_KEYS; i++) {
            if (database[i].in_use &&
                strcmp(database[i].key, key) == 0) {
                char resp[512];
                int rlen = snprintf(resp, sizeof(resp),
                                    "$%s\n", database[i].val);
                write(client_fd, resp, rlen);
                return;
            }
        }
        write(client_fd, "-ERR key not found\n",
              strlen("-ERR key not found\n"));
    }

   else if (strcmp(cmd, "DEL") == 0) {
        char *key = strtok_r(NULL, " ", &saveptr);

        if (!key) {
            write(client_fd, "-ERR DEL requires key\n",
                  strlen("-ERR DEL requires key\n"));
            return;
        }

        for (int i = 0; i < MAX_KEYS; i++) {
            if (database[i].in_use &&
                strcmp(database[i].key, key) == 0) {
                database[i].in_use = 0;
                write(client_fd, ":1\n", 3);
                return;
            }
        }
        write(client_fd, ":0\n", 3);  /* not found - return 0 not error */
    }

        else {
        write(client_fd, "-ERR unknown command\n",
              strlen("-ERR unknown command\n"));
    }

}