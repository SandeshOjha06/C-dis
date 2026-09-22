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

#include "store.h"

#define PEER_PORT "6379"

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

        // Add socket to epoll instance.
        // EPOLLOUT notifies when connection handshakes complete; EPOLLIN detects data/disconnections.
        struct epoll_event ev;
        ev.events  = EPOLLIN | EPOLLOUT | EPOLLET;
        ev.data.fd = peer_sock;

        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, peer_sock, &ev) == -1) {
            perror("[cluster] epoll_ctl failed for peer socket");
            close(peer_sock);
        } else {
            printf("[cluster] Registered peer socket fd=%d for host %s\n", peer_sock, peer_dns);
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
                int client_fd = events[i].data.fd;
                char buf[4096];

                int bytes = read_line(client_fd, buf, sizeof(buf));

                if (bytes <= 0) {
                    printf("Socket closed/disconnected: fd=%d\n", client_fd);
                    epoll_ctl(epfd, EPOLL_CTL_DEL, client_fd, NULL);
                    close(client_fd);
                } else {
                    parse_commands(buf, client_fd, ht);
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