#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "store.h"
#include "persist.h"

static void respond(int fd, const char *msg) {
    if (fd == -1) return;   // replay mode - don't send responses
    write(fd, msg, strlen(msg));
}

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
        char *key = strtok_r(NULL, " ", &saveptr);

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
        char *key = strtok_r(NULL, " ", &saveptr);

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