#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "store.h"

void parse_commands(char *buf, int client_fd, HashTable *ht);
int  read_line(int fd, char *buf, int size);

void server_run_epoll(int server_fd, HashTable *ht, int log_fd) {

    // epoll_create1 create epoll instance
    int epfd = epoll_create1(0);
    if (epfd == -1) {
        perror("epoll_create1");
        return;
    }

    // epoll_ctl ADD server_fd - watch for new connections
    struct epoll_event ev;
    ev.events  = EPOLLIN;      // ready to read
    ev.data.fd = server_fd;

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev) == -1) {
        perror("epoll_ctl server_fd");
        return;
    }

    // buffer for ready events
    struct epoll_event events[64];

    // while(1):
    while (1) {

        //   epoll_wait() - block until something ready
        int n = epoll_wait(epfd, events, 64, -1);
        if (n == -1) {
            if (errno == EINTR) continue;  // signal interrupted - retry
            perror("epoll_wait");
            return;
        }

        printf("[epoll] %d fd(s) ready\n", n);

        //   for each ready event:
        for (int i = 0; i < n; i++) {

            //     if event.data.fd == server_fd:
            if (events[i].data.fd == server_fd) {

                //       accept new client
                struct sockaddr_in addr = {0};
                socklen_t len = sizeof(addr);
                int client_fd = accept(server_fd,
                                       (struct sockaddr *)&addr, &len);
                if (client_fd == -1) {
                    perror("accept");
                    continue;
                }

                //       epoll_ctl ADD client_fd
                ev.events  = EPOLLIN;
                ev.data.fd = client_fd;
                if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
                    perror("epoll_ctl add client");
                    close(client_fd);
                    continue;
                }

                printf("client connected: fd=%d\n", client_fd);

            } else {

                //     else (existing client has data):
                int client_fd = events[i].data.fd;
                char buf[4096];

                //       read_line from client_fd
                int bytes = read_line(client_fd, buf, sizeof(buf));

                if (bytes <= 0) {

                    //       if n == 0: client disconnected
                    //                  epoll_ctl DEL client_fd
                    //                  close(client_fd)
                    printf("client disconnected: fd=%d\n", client_fd);
                    epoll_ctl(epfd, EPOLL_CTL_DEL, client_fd, NULL);
                    close(client_fd);

                } else {

                    //       if n > 0:  parse_commands()
                    parse_commands(buf, client_fd, ht);
                }
            }
        }
    }
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
        }
        else if (n == 0) {
            buf[total] = '\0';
            return 0;        // EOF - client disconnected
        }
        else {
            if (errno == EINTR) continue;
            return -1;       // real error
        }
    }

    buf[total] = '\0';
    return total;
}