#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

typedef struct HashTable HashTable;
extern void parse_commands(char *buf, int client_fd, HashTable *ht);

// wal_open(path)
//   open log file for appending
//   create if doesn't exist
//   return fd
int g_log_fd = -1;

int wal_open(const char *path) {
    g_log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_log_fd == -1) perror("wal_open");
    return g_log_fd;
}

int wal_write(int log_fd, const char *line) {
    size_t len = strlen(line);
    size_t total_written = 0;

    while (total_written < len) {
        ssize_t bytes_written = write(log_fd, line + total_written, len - total_written);
        
        if (bytes_written == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("Failed to write to WAL");
            return -1;
        }
        
        total_written += bytes_written;
    } 

    return 0;
}


//   open log file for reading
//   read line by line
//   for each line: parse_commands() into ht
//   close file
//   if file doesn't exist: that's fine, return silently
void wal_replay(const char *path, HashTable *ht) {
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        
        if (errno == ENOENT) {
            return;
        }
        // If it failed for another reason, log it but still return
        perror("Error accessing existing WAL file");
        return;
    }

    char *line = NULL;
    size_t len = 0;
    ssize_t read;

    
    while ((read = getline(&line, &len, file)) != -1) {
        parse_commands(line, -1, ht);
    }

    // Free the buffer allocated by getline
    free(line);
    
    // close file
    fclose(file);
}