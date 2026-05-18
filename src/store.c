#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

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

// hash() - private function
static unsigned int hash(const char *key) {
    unsigned long int hashval = 5381;
    int c;

    while((c = *key++)) {
        hashval = ((hashval << 5) + hashval) + c;
    }

    return hashval & (HASH_SIZE - 1);  // faster than % HASH_SIZE

}
// takes a string, returns index 0..TABLE_SIZE-1

// ht_create() - allocate and return a new HashTable
HashTable *ht_create(void) {
    // allocate with calloc so all bucket pointers start as NULL
    HashTable *ht = calloc(1, sizeof(HashTable));
    // check for failure
        if (ht == NULL) {
        return NULL;
    }
    // return it
    return ht;
}

// ht_set() - insert or update a key
int ht_set(HashTable *ht, const char *key, const char *val) {
    unsigned int index = hash(key);
    // search bucket chain for existing key first
    Entry *curr = ht->bucket[index];
    while (curr != NULL) {
        if (strcmp(curr->key, key) == 0){
            // if found: update value
            strncpy(curr->val, val, sizeof(curr->val) - 1);
            curr->val[sizeof(curr->val) - 1] = '\0';
            return 1;
            
        }
        curr = curr->next;
        // if not found: create new Entry, prepend to chain
        
    }
}
    
// ht_get() - find key, return value pointer or NULL

// ht_del() - remove key from chain
// need prev pointer to relink

// ht_exists() - return 1 or 0

// ht_destroy() - free everything

int main(void) {
    printf("hash(name)     = %u\n", hash("name"));
    printf("hash(city)     = %u\n", hash("city"));
    printf("hash(name)     = %u\n", hash("name")); // same as first?
    printf("hash(aaaa)     = %u\n", hash("aaaa"));
    printf("hash(bbbb)     = %u\n", hash("bbbb"));
    return 0;
}