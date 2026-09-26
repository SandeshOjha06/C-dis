#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/resource.h>

#include "store.h"

#define PEER_PORT "6379"
#define MAX_NODES 16

static int peer_sockets[MAX_NODES];
static int *peer_by_fd = NULL;
static int *pending_peer_node = NULL;
static int max_fds = 0;

static void registry_init(void) {
    for (int i = 0; i < MAX_NODES; i++) peer_sockets[i] = -1;

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur > 0) {
        max_fds = (int)rl.rlim_cur;
    } else {
        max_fds = 1024; // fallback
    }
    if (max_fds > 65536) max_fds = 65536; // sanity cap

    peer_by_fd = calloc(max_fds, sizeof(int));
    pending_peer_node = calloc(max_fds, sizeof(int));
    if (!peer_by_fd || !pending_peer_node) {
        perror("calloc registry");
        exit(1);
    }
    for (int i = 0; i < max_fds; i++) {
        peer_by_fd[i] = -1;
        pending_peer_node[i] = -1;
    }
    printf("[registry] Initialized with max_fds=%d\n", max_fds);
}

static void registry_register(int node_id, int fd) {
    if (node_id < 0 || node_id >= MAX_NODES) return;
    if (fd < 0 || fd >= max_fds) return;

    // If a stale socket exists for this node, clean it up first
    int old_fd = peer_sockets[node_id];
    if (old_fd != -1 && old_fd != fd) {
        if (old_fd < max_fds) peer_by_fd[old_fd] = -1;
        // Note: we don't close old_fd here; caller handles epoll/close
    }

    peer_sockets[node_id] = fd;
    peer_by_fd[fd] = node_id;
    pending_peer_node[fd] = -1;
    printf("[registry] Registered node %d -> fd %d\n", node_id, fd);
}

static void registry_unregister(int fd) {
    if (fd < 0 || fd >= max_fds) return;
    int node = peer_by_fd[fd];
    if (node != -1) {
        if (peer_sockets[node] == fd) peer_sockets[node] = -1;
        peer_by_fd[fd] = -1;
        printf("[registry] Unregistered node %d (fd %d)\n", node, fd);
    }
}

static int peer_fd_for_node(int node_id) {
    if (node_id < 0 || node_id >= MAX_NODES) return -1;
    return peer_sockets[node_id];
}

void parse_commands(char *buf, int client_fd, HashTable *ht);
int  read_line(int fd, char *buf, int size);

// Helper function to configure sockets for asynchronous non-blocking I/O
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL O_NONBLOCK");
    }
}

// Inspects PEER_NODES topology injected by the Operator and connects to peer nodes
void bootstrap_cluster_peers(int epoll_fd) {
    char my_hostname[256];
    if (gethostname(my_hostname, sizeof(my_hostname)) == -1) {
        perror("gethostname");
        return;
    }

    char *env_peers = getenv("PEER_NODES");
    if (env_peers == NULL) {
        printf("[cluster] PEER_NODES not found. Starting in standalone mode.\n");
        return;
    }

    printf("[cluster] Local hostname: %s\n", my_hostname);
    printf("[cluster] Injected topology: %s\n", env_peers);

    // strtok_r modifies the buffer in-place, so duplicate the environment string
    char *peers_copy = strdup(env_peers);
    if (!peers_copy) {
        perror("strdup");
        return;
    }

    char *saveptr;
    char *peer_dns = strtok_r(peers_copy, ",", &saveptr);

    while (peer_dns != NULL) {
        // Skip self-referential addresses
        if (strstr(peer_dns, my_hostname) != NULL) {
            peer_dns = strtok_r(NULL, ",", &saveptr);
            continue;
        }

        printf("[cluster] Resolving peer address: %s\n", peer_dns);

        struct addrinfo hints, *res;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(peer_dns, PEER_PORT, &hints, &res) != 0) {
            fprintf(stderr, "[cluster] DNS resolution failed for %s (peer may still be starting)\n", peer_dns);
            peer_dns = strtok_r(NULL, ",", &saveptr);
            continue;
        }

        int peer_sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (peer_sock == -1) {
            perror("[cluster] socket creation failed");
            freeaddrinfo(res);
            peer_dns = strtok_r(NULL, ",", &saveptr);
            continue;
        }

        set_nonblocking(peer_sock);

        // Initiate connection. For non-blocking sockets, connect returns -1 with EINPROGRESS.
        int conn_res = connect(peer_sock, res->ai_addr, res->ai_addrlen);
        if (conn_res == -1 && errno != EINPROGRESS) {
            perror("[cluster] connect failed");
            close(peer_sock);
            freeaddrinfo(res);
            peer_dns = strtok_r(NULL, ",", &saveptr);
            continue;
        }

        // Extract target node_id from DNS name (format: kv-store-N.kv-store-network)
        int target_node = -1;
        sscanf(peer_dns, "kv-store-%d.kv-store-network", &target_node);
        if (target_node >= 0 && target_node < MAX_NODES) {
            if (peer_sock < max_fds) pending_peer_node[peer_sock] = target_node;
        } else {
            fprintf(stderr, "[cluster] Could not parse node_id from %s\n", peer_dns);
        }

        // Add socket to epoll instance.
        // EPOLLOUT notifies when connection handshakes complete; EPOLLIN detects data/disconnections.
        struct epoll_event ev;
        ev.events  = EPOLLIN | EPOLLOUT | EPOLLET;
        ev.data.fd = peer_sock;

        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, peer_sock, &ev) == -1) {
            perror("[cluster] epoll_ctl failed for peer socket");
            close(peer_sock);
        } else {
            printf("[cluster] Registered peer socket fd=%d for host %s (target node %d)\n", peer_sock, peer_dns, target_node);
        }

        freeaddrinfo(res);
        peer_dns = strtok_r(NULL, ",", &saveptr);
    }

    free(peers_copy);
}

void server_run_epoll(int server_fd, HashTable *ht) {

    // Create epoll instance
    int epfd = epoll_create1(0);
    if (epfd == -1) {
        perror("epoll_create1");
        return;
    }

    // Register listening server socket
    struct epoll_event ev;
    ev.events  = EPOLLIN;
    ev.data.fd = server_fd;

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev) == -1) {
        perror("epoll_ctl server_fd");
        close(epfd);
        return;
    }

    // Initialize peer registry
    registry_init();

    // Dial out to sibling cluster pods using the injected topology
    bootstrap_cluster_peers(epfd);

    struct epoll_event events[64];

    while (1) {
        int n = epoll_wait(epfd, events, 64, -1);
        if (n == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == server_fd) {
                // Incoming connection from client or peer
                struct sockaddr_in addr = {0};
                socklen_t len = sizeof(addr);
                int client_fd = accept(server_fd, (struct sockaddr *)&addr, &len);
                if (client_fd == -1) {
                    perror("accept");
                    continue;
                }

                set_nonblocking(client_fd);

                ev.events  = EPOLLIN | EPOLLET;
                ev.data.fd = client_fd;
                if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
                    perror("epoll_ctl add client");
                    close(client_fd);
                    continue;
                }

                printf("Connection established: fd=%d\n", client_fd);

            } else {
                int fd = events[i].data.fd;
                uint32_t evts = events[i].events;

                // Handle connection completion for outbound peer connections
                if ((evts & EPOLLOUT) && fd < max_fds && pending_peer_node[fd] != -1) {
                    int node = pending_peer_node[fd];
                    // Verify connection succeeded
                    int err = 0;
                    socklen_t len = sizeof(err);
                    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                        registry_register(node, fd);
                        // Switch to EPOLLIN only; we don't need EPOLLOUT anymore
                        struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.fd = fd };
                        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
                    } else {
                        fprintf(stderr, "[cluster] Peer connection failed for node %d (fd %d): %s\n",
                                node, fd, strerror(err));
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                        pending_peer_node[fd] = -1;
                    }
                    continue;
                }

                char buf[4096];
                int bytes = read_line(fd, buf, sizeof(buf));

                if (bytes <= 0) {
                    printf("Socket closed/disconnected: fd=%d\n", fd);
                    registry_unregister(fd);
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                } else {
                    parse_commands(buf, fd, ht);
                }
            }
        }
    }

    close(epfd);
}

int read_line(int fd, char *buf, int size) {
    int  total = 0;
    char c;
    int  n;

    while (total < size - 1) {
        n = read(fd, &c, 1);

        if (n == 1) {
            buf[total++] = c;
            if (c == '\n') {
                buf[total] = '\0';
                return total;
            }
        } else if (n == 0) {
            buf[total] = '\0';
            return 0; // EOF
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Non-blocking socket drained for now
                break;
            }
            return -1;
        }
    }

    buf[total] = '\0';
    return total;
}
