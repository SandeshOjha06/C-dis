
extern int g_log_fd;
int  wal_open(const char *path);
int  wal_write(int log_fd, const char *line);
void wal_replay(const char *path, HashTable *ht);