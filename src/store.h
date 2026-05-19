#ifndef STORE_H
#define STORE_H

#define HASH_SIZE 1024

typedef struct Entry {
    char         key[64];
    char         val[256];
    struct Entry *next;
} Entry;

typedef struct {
    Entry *buckets[HASH_SIZE];
    int    count;
} HashTable;

HashTable *ht_create(void);
void       ht_destroy(HashTable *ht);
int        ht_set(HashTable *ht, const char *key, const char *val);
char      *ht_get(HashTable *ht, const char *key);
int        ht_del(HashTable *ht, const char *key);
int        ht_exists(HashTable *ht, const char *key);

#endif
