#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "store.h"

void parse_commands(char *buf, int client_fd, HashTable *ht) {
     // strip trailing \r\n first
    int len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
        buf[--len] = '\0';

    char *saveptr;
    char *cmd = strtok_r(buf, " ", &saveptr);  // split on space
    if (cmd == NULL) return;

  if (strcmp(cmd, "SET") == 0) {
    char *key = strtok_r(NULL, " ", &saveptr);
    char *val = strtok_r(NULL, " ", &saveptr);
    if (!key || !val) {
        write(client_fd, "-ERR SET requires key and value\n", 32);
        return;
    }
    ht_set(ht, key, val);
    write(client_fd, "+OK\n", 4);
}

    else if (strcmp(cmd, "GET") == 0) {
        char *key = strtok_r(NULL, " ", &saveptr);

        if (!key) {
            write(client_fd, "-ERR GET requires key\n",
                  strlen("-ERR GET requires key\n"));
            return;
        }

       char *found = ht_get(ht, key);
if (found == NULL) {
    write(client_fd, "-ERR key not found\n", 19);
} else {
    char resp[512];
    int rlen = snprintf(resp, sizeof(resp), "$%s\n", found);
    write(client_fd, resp, rlen);
}

    
    }

   else if (strcmp(cmd, "DEL") == 0) {
        char *key = strtok_r(NULL, " ", &saveptr);

        if (!key) {
            write(client_fd, "-ERR DEL requires key\n",
                  strlen("-ERR DEL requires key\n"));
            return;
        }

       if (ht_del(ht, key)) {
    write(client_fd, ":1\n", 3);
} else {
    write(client_fd, ":0\n", 3);
}
       
    }

        else {
        write(client_fd, "-ERR unknown command\n",
              strlen("-ERR unknown command\n"));
    }

}