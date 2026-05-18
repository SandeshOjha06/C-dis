#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

void parse_commands(char *buf, int client_fd);
int read_line(int fd, char *buf, int size);

int main() {
    // create socket fd
    int server_fd = socket(AF_INET,SOCK_STREAM,0);
    if (server_fd < 0) {
        perror("socket error");
        return 1;
    }

    // allow port reuse
    int val = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) == -1) {
    perror("setsockopt");
    return 1;
}
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; // sets the address fakily to IPv4
    addr.sin_port = htons(6379); // setting port to 6379
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // tells the socket to bind to all available network interfaces


    // bind socket to address
    if(bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0){
        perror("bind error");
        return 1;
    };

    // start listeing, queue upto 10 conns
    if (listen(server_fd, 10) < 0) {
        perror("listen failed");
        return 1;
    }

    printf("KV store waiting for connections..\n");

    // accept one connection
    struct sockaddr_in client_addr = {0};
    socklen_t client_len = sizeof(client_addr);

    // This blocks until someone connects
    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
        perror("accept failed");
        return 1;
    }

    printf("Client Connected\n");

    char buf[4096];
    int  n;
    while (1) {
    n = read_line(client_fd, buf, sizeof(buf));
    if (n <= 0) break;
    printf("[server] received: '%s'\n", buf);
    parse_commands(buf, client_fd);
}

    printf("Client disconnected");
    // close client fd
    close(client_fd);

    // close server fd
    close(server_fd);

    return 0;

}

int read_line(int fd, char *buf, int size) {
    int total = 0;
    char c;
    int  n;

    while (total < size - 1) {
        n = read(fd, &c, 1);

        if (n == 1) {
            buf[total++] = c;
            if (c == '\n') {
                buf[total] = '\0';   // null terminate
                return total;        // return byte count
            }
        }
        else if (n == 0) {
            buf[total] = '\0';
            return total;            // EOF
        }
        else {
            if (errno == EINTR) continue;
            return -1;               // real error
        }
    }

    buf[total] = '\0';
    return total;
}