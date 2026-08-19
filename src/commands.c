#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>      
#include <sys/types.h>
#include <sys/socket.h>

#include "store.h"
#include "persist.h"

extern int my_node_id;
extern int cluster_size;

static void forward_to_peer(int target_node, const char *raw_cmd, int client_fd);

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
    // save copy for proxying
    char raw_cmd[4096];
    strncpy(raw_cmd, buf, sizeof(raw_cmd) - 1);
    raw_cmd[sizeof(raw_cmd) - 1] = '\0';

     // strip trailing \r\n first
    int len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
        buf[--len] = '\0';

    char *saveptr;
    char *cmd = strtok_r(buf, " ", &saveptr);  // split on space
    if (cmd == NULL) return;

    char *key = strtok_r(NULL, " ", &saveptr);
        if (key != NULL) {
            int target_node = get_target_node(key);

            if (target_node != my_node_id) {
                printf("Routing key '%s' to Node %d\n", key, target_node);
                forward_to_peer(target_node, raw_cmd, client_fd);
                return;
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

static void forward_to_peer(int target_node, const char *raw_cmd, int client_fd) {
    char peer_dns[256];
    //build the Kubernetes Headless Service DNS string
    snprintf(peer_dns, sizeof(peer_dns), "kv-store-%d.kv-store-network", target_node);

    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    // ask the Kubernetes CoreDNS to translate the name to an IP
    if (getaddrinfo(peer_dns, "6379", &hints, &res) != 0) {
        respond(client_fd, "-ERR Proxy DNS resolution failed\n");
        return;
    }

    // create a socket and connect to the peer
    int proxy_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (proxy_fd < 0) {
        respond(client_fd, "-ERR Proxy socket creation failed\n");
        freeaddrinfo(res);
        return;
    }

    if (connect(proxy_fd, res->ai_addr, res->ai_addrlen) < 0) {
        respond(client_fd, "-ERR Proxy connection failed\n");
        close(proxy_fd);
        freeaddrinfo(res);
        return;
    }
    
    //free the memory allocated by getaddrinfo
    freeaddrinfo(res); 

    // forward the command over the network
    write(proxy_fd, raw_cmd, strlen(raw_cmd));
    
    // The target peer's read_line() function expects a \n to know when to stop reading!
    write(proxy_fd, "\n", 1); 

    // wait for the peer to process it and read their response
    char resp_buf[512];
    int n = read(proxy_fd, resp_buf, sizeof(resp_buf) - 1);
    
    if (n > 0) {
        resp_buf[n] = '\0';
        //relay the peer's answer directly back to our original client
        respond(client_fd, resp_buf);
    } else {
        respond(client_fd, "-ERR Proxy read failed\n");
    }

    // hang up the phone
    close(proxy_fd);
}