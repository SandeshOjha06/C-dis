#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "store.h"

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
    Entry *curr = ht->buckets[index];
    while (curr != NULL) {
        if (strcmp(curr->key, key) == 0){
            // if found: update value
            strncpy(curr->val, val, sizeof(curr->val) - 1);
            curr->val[sizeof(curr->val) - 1] = '\0';
            return 1;
            
        }
        curr = curr->next;
        
    }
    // if not found: create new Entry, prepend to chain
    Entry *new_entry = malloc(sizeof(Entry));
    if (new_entry == NULL) return 0;

    strncpy(new_entry->key,key,sizeof(new_entry->key) - 1);
    new_entry->key[sizeof(new_entry->key) -1] = '\0'; 
    strncpy(new_entry->val,val,sizeof(new_entry->val) - 1);
    new_entry->val[sizeof(new_entry->val) - 1] = '\0';

    // prepend to chain
    new_entry->next = ht->buckets[index];
    ht->buckets[index] = new_entry;
    ht->count++;

    return 1;
}
    
// ht_get() - find key, return value pointer or NULL
char *ht_get(HashTable *ht, const char *key) {
    unsigned int index = hash(key);

    //start at first node of the bucket
    Entry *curr = ht->buckets[index];

    while (curr != NULL) {
        if (strcmp(curr->key, key) == 0) {
            return curr->val;
        }

        curr = curr->next;
    }

    return NULL;

}

// ht_del() - remove key from chain
int ht_del(HashTable *ht, const char *key) {
    unsigned int index = hash(key);

    Entry *curr = ht->buckets[index];
    Entry *prev = NULL;

    while (curr != NULL) {
        if(strcmp(curr->key, key) == 0) {
            // if it is the forst node in the bucket
            if (prev == NULL) {
                ht->buckets[index] = curr->next;
            }

            //if it is in the middle or end
            else {
                prev->next = curr->next; //bridge the gap
            }

            //memory cleanup
            free(curr);
            ht->count--;

            return 1;
        }

        prev = curr;
        curr = curr->next;
    }

    return 0;
}


// ht_exists() - return 1 or 0
int ht_exists(HashTable *ht, const char *key) {
    if (ht_get(ht, key) != NULL) {
        return 1;
    }
    return 0;
}

// ht_destroy() - free everything
void ht_destroy(HashTable *ht) {
    for (int i = 0; i < HASH_SIZE; i++) {
        Entry *curr = ht->buckets[i];
        while (curr != NULL) {
            Entry *next_node = curr->next;
            free(curr);
            curr = next_node;
        }
    }
    free(ht); //destroy the main table
}

