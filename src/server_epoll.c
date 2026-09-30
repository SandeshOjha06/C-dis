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
#include <stdbool.h>

extern int my_node_id;
extern int cluster_size;

#define PEER_PORT "6379"
#define MAX_NODES 16
#define EGRESS_BUF_INIT 4096
#define EGRESS_BUF_MAX  (64 * 1024)  // 64KB high-water mark

typedef enum { ROLE_UNKNOWN = 0, ROLE_CLIENT, ROLE_PEER } conn_role_t;

typedef struct {
    int fd;
    conn_role_t role;
    int peer_node_id;       // valid only if role == ROLE_PEER, else -1
    uint8_t *wbuf;          // egress buffer
    size_t wbuf_cap;        // allocated capacity
    size_t woff;            // write offset (bytes already sent)
    size_t wlen;            // valid bytes in buffer (woff..woff+wlen-1)
    bool write_armed;       // is EPOLLOUT registered?
} conn_t;

static int peer_sockets[MAX_NODES];
static int *peer_by_fd = NULL;
static int *pending_peer_node = NULL;
static conn_t *connections = NULL;
static int max_fds = 0;
static int g_epfd = -1;  // epoll fd for append_to_wbuf

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
    connections = calloc(max_fds, sizeof(conn_t));
    if (!peer_by_fd || !pending_peer_node || !connections) {
        perror("calloc registry");
        exit(1);
    }
    for (int i = 0; i < max_fds; i++) {
        peer_by_fd[i] = -1;
        pending_peer_node[i] = -1;
        connections[i].fd = i;
        connections[i].role = ROLE_UNKNOWN;
        connections[i].peer_node_id = -1;
        connections[i].wbuf = NULL;
        connections[i].wbuf_cap = 0;
        connections[i].woff = 0;
        connections[i].wlen = 0;
        connections[i].write_armed = false;
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

int peer_fd_for_node(int node_id) {
    if (node_id < 0 || node_id >= MAX_NODES) return -1;
    return peer_sockets[node_id];
}

int get_connection_role(int fd) {
    if (fd < 0 || fd >= max_fds) return ROLE_UNKNOWN;
    return connections[fd].role;
}

int get_peer_node_id(int fd) {
    if (fd < 0 || fd >= max_fds) return -1;
    return connections[fd].peer_node_id;
}

// Ensure egress buffer has at least 'need' bytes free
static bool ensure_wbuf_capacity(conn_t *c, size_t need) {
    size_t available = c->wbuf_cap - c->woff - c->wlen;
    if (available >= need) return true;

    size_t new_cap = c->wbuf_cap ? c->wbuf_cap * 2 : EGRESS_BUF_INIT;
    while (new_cap - c->woff - c->wlen < need) new_cap *= 2;
    if (new_cap > EGRESS_BUF_MAX) new_cap = EGRESS_BUF_MAX;
    if (new_cap - c->woff - c->wlen < need) return false;  // would exceed max

    uint8_t *nb = realloc(c->wbuf, new_cap);
    if (!nb) return false;
    c->wbuf = nb;
    c->wbuf_cap = new_cap;
    return true;
}

// Append data to egress buffer, arm EPOLLOUT if needed
static bool append_to_wbuf(int epfd, int fd, const void *data, size_t len) {
    if (fd < 0 || fd >= max_fds) return false;
    conn_t *c = &connections[fd];
    if (!ensure_wbuf_capacity(c, len)) {
        fprintf(stderr, "[egress] Buffer full for fd %d (len=%zu, cap=%zu)\n", fd, c->woff + c->wlen + len, c->wbuf_cap);
        return false;
    }
    memcpy(c->wbuf + c->woff + c->wlen, data, len);
    c->wlen += len;

    if (!c->write_armed) {
        struct epoll_event ev = { .events = EPOLLIN | EPOLLOUT | EPOLLET, .data.fd = fd };
        if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) == 0) {
            c->write_armed = true;
        } else {
            perror("epoll_ctl MOD EPOLLOUT");
            return false;
        }
    }
    return true;
}

// Drain egress buffer on EPOLLOUT readiness
static void drain_wbuf(int epfd, int fd) {
    if (fd < 0 || fd >= max_fds) return;
    conn_t *c = &connections[fd];
    if (c->wlen == 0) return;

    ssize_t n = write(fd, c->wbuf + c->woff, c->wlen);
    if (n > 0) {
        c->woff += n;
        if (c->woff == c->wlen) {
            c->woff = 0;
            c->wlen = 0;
        }
    } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;  // try again on next EPOLLOUT
    } else {
        // Error or EOF - let the EPOLLIN/EPOLLHUP path handle cleanup
        return;
    }

    if (c->wlen == 0 && c->write_armed) {
        struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.fd = fd };
        if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) == 0) {
            c->write_armed = false;
        }
    }
}

// Free egress buffer on connection close
static void free_wbuf(int fd) {
    if (fd < 0 || fd >= max_fds) return;
    conn_t *c = &connections[fd];
    free(c->wbuf);
    c->wbuf = NULL;
    c->wbuf_cap = 0;
    c->woff = 0;
    c->wlen = 0;
    c->write_armed = false;
}

bool forward_to_peer_node(int target_node, const void *data, size_t len) {
    int peer_fd = peer_fd_for_node(target_node);
    if (peer_fd == -1) return false;
    return append_to_wbuf(g_epfd, peer_fd, data, len);
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
    g_epfd = epfd;
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

                if (client_fd < max_fds) {
                    connections[client_fd].role = ROLE_UNKNOWN;
                    connections[client_fd].peer_node_id = -1;
                }

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
                        if (fd < max_fds) {
                            connections[fd].role = ROLE_PEER;
                            connections[fd].peer_node_id = node;
                        }
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

                // Handle egress buffer drain for established connections
                if ((evts & EPOLLOUT) && fd < max_fds && connections[fd].write_armed) {
                    drain_wbuf(epfd, fd);
                    // If there's still data to send, EPOLLOUT stays armed;
                    // drain_wbuf disarms it when buffer is empty.
                    // Continue to EPOLLIN processing in case both are set.
                }

                if (evts & EPOLLIN) {
                    char buf[4096];
                    int bytes = read_line(fd, buf, sizeof(buf));

                    if (bytes <= 0) {
                        printf("Socket closed/disconnected: fd=%d\n", fd);
                        registry_unregister(fd);
                        free_wbuf(fd);
                        if (fd < max_fds) {
                            connections[fd].role = ROLE_UNKNOWN;
                            connections[fd].peer_node_id = -1;
                        }
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                    } else {
                        // Handle HELLO handshake for inbound peer connections
                        if (fd < max_fds && connections[fd].role == ROLE_UNKNOWN) {
                            if (strncmp(buf, "HELLO ", 6) == 0) {
                                int node = atoi(buf + 6);
                                if (node >= 0 && node < MAX_NODES && node != my_node_id && peer_sockets[node] == -1) {
                                    connections[fd].role = ROLE_PEER;
                                    connections[fd].peer_node_id = node;
                                    registry_register(node, fd);
                                    printf("[cluster] Inbound peer handshake: node %d on fd %d\n", node, fd);
                                } else {
                                    fprintf(stderr, "[cluster] Invalid HELLO from fd %d: node=%d (my_id=%d, occupied=%d)\n",
                                            fd, node, my_node_id, peer_sockets[node] != -1);
                                    // Treat as client
                                    connections[fd].role = ROLE_CLIENT;
                                }
                                continue;  // Handshake consumed, wait for next line
                            } else {
                                // Not a peer handshake -> client
                                connections[fd].role = ROLE_CLIENT;
                            }
                        }
                        parse_commands(buf, fd, ht);
                    }
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
