#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "store.h"
#include "persist.h"

void server_run_epoll(int server_fd, HashTable *ht);
int wal_open(const char *path);
void wal_replay(const char *path, HashTable *ht);

int my_node_id = 0;
int cluster_size = 1;

int main(void) {
 //Disable buffering immediately
  setbuf(stdout, NULL);
  printf("[boot] Starting C-dis Initialization...\n");

  char *env_size = getenv("CLUSTER_SIZE");
  if (env_size != NULL) {
    cluster_size = atoi(env_size);
    if (cluster_size <= 0) {
      fprintf(stderr, "Invalid cluster size, defaulting to 1\n");
      cluster_size = 1;
    }
  }

  char *hostname = getenv("HOSTNAME");
  if (hostname != NULL) {
    sscanf(hostname, "my-database-sts-%d", &my_node_id);
    printf("[boot] Starting node %d of %d\n", my_node_id, cluster_size);
  }

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    perror("socket");
    return 1;
  }

  int val = 1;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) == -1) {
    perror("setsockopt");
    return 1;
  }

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(6379);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    return 1;
  }

  if (listen(server_fd, 10) < 0) {
    perror("listen");
    return 1;
  }

  printf("[boot] KV store listening on port 6379\n");

  HashTable *ht = ht_create();
  if (!ht) {
    fprintf(stderr, "ht_create failed\n");
    return 1;
  }

// Write directly to the Kubernetes Persistent Volume
  const char* wal_path = "/app/data/kvstore.log";
  g_log_fd = -1;                      
  wal_replay(wal_path, ht);      
  g_log_fd = wal_open(wal_path); 

  // Start the network mesh
  server_run_epoll(server_fd, ht);

  ht_destroy(ht);
  close(server_fd);
  return 0;
}