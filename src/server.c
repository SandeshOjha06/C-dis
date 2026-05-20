#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include "store.h"

void parse_commands(char *buf, int client_fd, HashTable *ht);
int read_line(int fd, char *buf, int size);

// initialize clients[] array - all slots to -1
void server_run(int server_fd, HashTable *ht) {
    // initialize client array - all empty
    int clients[64];
    for(int i = 0; i < 64; i++) {
        clients[i] = -1;
    }
    
    // set max_fd = server_fd
    int max_fd = server_fd;
    
    while (1) {
        // rebuild fd_set every iteration - select destroys it
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds); // add server_fd to the fd_set
        
        for(int i = 0; i < 64; i++) {
            if (clients[i] != -1) {
                FD_SET(clients[i], &readfds);
            }
        }
    
        // block until something is ready 
        int ready = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (ready == -1) {
            perror("SELECT");
            return;
        }

        // handle new connections
        if (FD_ISSET(server_fd, &readfds)) {
            struct sockaddr_in client_addr = {0};
            socklen_t client_len = sizeof(client_addr); 
            // accept new client
            int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

            if (client_fd == -1) {
                perror("ACCEPT");
            } else {
                // find empty slot in clients[]
                int added = 0;
                for (int i = 0; i < 64; i++) {
                    if (clients[i] == -1) {
                        clients[i] = client_fd;

                        if (client_fd > max_fd) max_fd = client_fd;
                        
                        printf("Client connected: fd=%d\n", client_fd);
                        added = 1;
                        break;
                    }
                }

                if (!added) {
                    // no room 
                    write(client_fd, "-ERR server full\n", 17); 
                    close(client_fd); 
                }
            }
        }
        
        // existing client data handler
        for(int i =0; i < 64; i++) {
            int client_fd = clients[i];

            //only checkong slots that actually have active client
            if (client_fd != -1 && FD_ISSET(client_fd, &readfds)) {
                char buf[4096];

                int n = read_line(client_fd, buf, sizeof(buf));

                if (n <= 0) {
                    printf("Client has disconnected fd=%d\n", client_fd);
                    close(client_fd);
                    clients[i] = -1;

                    max_fd = server_fd;
                    for (int j = 0; j < 64; j++) {
                        if (clients[j] > max_fd) {
                            max_fd = clients[j];
                        }
                    }
                } else {
                    parse_commands(buf, client_fd, ht);
                }
            }
        }


        
    } 
} 
