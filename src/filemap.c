#include <stdio.h>
#include <stdint.h> //ints for compatibility, since we are writing to files
#include <string.h>

#include "util.h"
#include "vector.h"
#include "rwlock.h"

#include "threads.h"
#include "siphash.h"

#define LOAD_FACTOR 0.5
#define RESIZE_SLOTS 3
#define MUTEXES 10

// probe hashmap with an index and data
typedef struct {
  FILE* index; //stores indexes which to search in data
  FILE* data; //stores key value pairs

  uint16_t hash_seed[4];

  uint64_t free;

  uint64_t length;
  uint64_t slots;

  rwlock_t resize; //lock for resizes
  mtx_t mutexes[MUTEXES]; //list of mutexes, indexed by the item hash. no need for probing or identification, this is a lossy map for temporary synchronization
} filemap_t;

typedef struct {
  uint64_t index;
  uint64_t data_pos;
  uint64_t data_size;

  char exists;
} filemap_result;

typedef struct {
  char exists;

  uint64_t length;
  char* data;
} filemap_mem_result;

// write slots to index and data
// assumes index is at correct point
void write_slots(filemap_t* filemap, unsigned int slots) {    
  for (unsigned i=0; i<slots; i++) {
    uint64_t pos = 0; //NULL pos, uninitialized
    fwrite(&pos, 8, 1, filemap->index);
  }
}

//seed and lengths, before indexes
const size_t PREAMBLE = 2*4 + (8*2);

int filemap_new(filemap_t* filemap, char* index, char* data) {
  filemap->index = fopen(index, "rb+");
  filemap->data = fopen(data, "rb+");

  if (!filemap->index && !filemap->data) {
    filemap->index = fopen(index, "wb");
    filemap->data = fopen(data, "wb");

    //copied from hashtable
    for (char i = 0; i < 4; i++) {
			filemap->hash_seed[i] = rand(); //seed 16 bytes at a time
		}

    //go to beginning and write seed
    fseek(filemap->index, 0, SEEK_SET);
    fwrite(filemap->hash_seed, 2, 4, filemap->index);
    
    filemap->slots = 2;
    filemap->length = 0;

    filemap->free = 0;

    //uint64s -> 8 bytes
    fwrite(&filemap->slots, 8, 1, filemap->index);
    fwrite(&filemap->length, 8, 1, filemap->index);

    fwrite(&filemap->free, 8, 1, filemap->data);

    write_slots(filemap, 2); //write slots afterwards

    freopen(index, "rb+", filemap->index);
    freopen(data, "rb+", filemap->data);

    return 1;
  }

  //read the seed (2 bytes x 4)
  fread(filemap->hash_seed, 2, 4, filemap->index);
  fread(&filemap->slots, sizeof(uint64_t), 1, filemap->index);
  fread(&filemap->length, sizeof(uint64_t), 1, filemap->index);

  for (int i=0; i<MUTEXES; i++) {
    mtx_init(&filemap->mutexes[i], mtx_plain);
  }

  filemap->resize = rwlock_new();

  return 1;
}

uint64_t do_hash(filemap_t* filemap, char* key, uint64_t key_size) {
  return siphash24((uint8_t*)key, key_size, (char*)filemap->hash_seed);
}

//same as above, but does not check for equality / only returns uninitialized indices
static uint64_t filemap_nondup_find(filemap_t* filemap, uint64_t hash) {
  long probes = 0;
  uint64_t pos = 1;
  uint64_t index;

  while (pos != 0) {
    uint64_t slot = (uint64_t)(hash + 0.5*probes + 0.5*probes*probes) % filemap->slots;
    fseek(filemap->index, PREAMBLE + (slot*8), SEEK_SET);

    index = ftell(filemap->index);
    fread(&pos, 8, 1, filemap->index);

    probes++;
  }

  return index;
}

//incremental resize
//add another bucket(s), check first few buckets to see if it lies in this bucket
void filemap_resize(filemap_t* filemap) {
  if ((double)filemap->length/(double)filemap->slots > LOAD_FACTOR) {
    rwlock_write(&filemap->resize); //anticipate resize, other threads may grab it first
    
    //so we check again...
    while ((double)filemap->length/(double)filemap->slots > LOAD_FACTOR) {
      fseek(filemap->index, 0, SEEK_END);

      write_slots(filemap, RESIZE_SLOTS);
      filemap->slots += RESIZE_SLOTS;
      
      fseek(filemap->index, PREAMBLE, SEEK_SET);

      for (int i=0; i < RESIZE_SLOTS; i++) {
        uint64_t slot_pos = ftell(filemap->index);

        uint64_t pos, key_size, val_size;
        fread(&pos, 8, 1, filemap->index);

        if (!pos) continue;

        fseek(filemap->data, pos, SEEK_SET);
        fread(&key_size, 8, 1, filemap->data);
        fread(&val_size, 8, 1, filemap->data);

        char key[key_size];
        fread(key, key_size, 1, filemap->data);

        uint64_t hash = do_hash(filemap, key, key_size);
        
        if (hash % filemap->slots != i) {
          uint64_t current = ftell(filemap->index);
          
          //seek back to pos, set to zero (since we are moving item)
          fseek(filemap->index, slot_pos, SEEK_SET);

          uint64_t set_pos = 0;
          fwrite(&set_pos, 8, 1, filemap->index);
          
          uint64_t new_index = filemap_nondup_find(filemap, hash);
          //write old position to new index
          fseek(filemap->index, new_index, SEEK_SET);
          fwrite(&pos, 8, 1, filemap->index);
          
          //seek back to current
          fseek(filemap->index, current, SEEK_SET);
        }
      }
    }

    rwlock_unwrite(&filemap->resize);
  }
}

static filemap_result filemap_find(filemap_t* filemap, uint64_t hash, char* key, uint64_t key_size) {
  filemap_result res;
  res.exists = 0;

  long probes = 0;

  do {
    uint64_t slot = (uint64_t)(hash + 0.5*probes + 0.5*probes*probes) % filemap->slots;
    fseek(filemap->index, PREAMBLE + (slot*8), SEEK_SET);

    res.index = ftell(filemap->index);
    fread(&res.data_pos, 8, 1, filemap->index); //read pos from current slot

    if (res.data_pos != 0) {
      fseek(filemap->data, res.data_pos, SEEK_SET);

      uint64_t cmp_key_size=0;
    
      fread(&cmp_key_size, 8, 1, filemap->data);
      fread(&res.data_size, 8, 1, filemap->data);

      if (cmp_key_size == key_size) {
        char cmp_key[key_size];
        fread(&cmp_key, key_size, 1, filemap->data);
        
        if (memcmp(key, cmp_key, key_size)==0) {
          res.exists = 1;
          return res;
        }
      }
    }

    probes++;

  } while (res.data_pos != 0);

  return res;
}

//returns 1 if list needs to be rearranged so that block is at the head
int freelist_insert(filemap_t* filemap, uint64_t freelist, uint64_t block, uint64_t size) {
  uint64_t freelist_prev = 0;
  uint64_t cmp_size = UINT64_MAX;

  // keep iterating from the front until we find an element that we are greater then
  while (freelist) {
    fseek(filemap->data, freelist, SEEK_SET);
    fread(&cmp_size, 8, 1, filemap->data); //read size

    if (size > cmp_size) {
      break;
    } else {
      freelist_prev = freelist;
    }

    fread(&freelist, 8, 1, filemap->data); //read next
  }

  //insert between freelist_prev and freelist
  
  //write size and next (freelist)
  fseek(filemap->data, block, SEEK_SET);
  fwrite(&size, 8, 1, filemap->data);
  fwrite(&freelist, 8, 1, filemap->data);

  if (freelist_prev) {
    fseek(filemap->data, freelist + 8, SEEK_SET); //go to next 
    fwrite(&block, 8, 1, filemap->data);

    return 0;
  } else {
    return block;
  }
}

//get a free block from freelist or end of data file
//data_size does not include key or data lengths
uint64_t get_free(filemap_t* filemap, uint64_t data_size) {
  if (filemap->free) {
    fseek(filemap->data, filemap->free, SEEK_SET);
    uint64_t size, next;
    
    fread(&size, 8, 1, filemap->data);

    if (size == data_size) {
      fread(&next, 8, 1, filemap->data);

      fseek(filemap->data, 0, SEEK_SET);
      fwrite(&next, 8, 1, filemap->data);

      return filemap->free;
    
    } else if (size > data_size && size - data_size >= 8*2) {
      //insert residue as free block
      uint64_t block = filemap->free + data_size;
      uint64_t freed = filemap->free;

      //set next or block as head
      if (freelist_insert(filemap, next, block, size - data_size)) {
        filemap->free = block;
      } else {
        filemap->free = next;
      }

      return freed;
    }
  }

  fseek(filemap->data, 0, SEEK_END);
  return ftell(filemap->data);
}

void filemap_lock(filemap_t* filemap, uint64_t hash) {
  rwlock_read(&filemap->resize);
  mtx_lock(&filemap->mutexes[hash % MUTEXES]);
}

void filemap_unlock(filemap_t* filemap, uint64_t hash) {
  rwlock_unread(&filemap->resize);
  mtx_unlock(&filemap->mutexes[hash % MUTEXES]);
}

// returns zero if already exists
int filemap_insert(filemap_t* filemap, char* key, char* value, uint64_t key_size, uint64_t val_size) {
  filemap_resize(filemap);

  uint64_t hash = do_hash(filemap, key, key_size);
  filemap_lock(filemap, hash);
  
  filemap_result res = filemap_find(filemap, hash, key, key_size);

  if (!res.exists) {
    res.data_pos = get_free(filemap, key_size + val_size);
    
    //store both sizes in block
    //these are used to store the length and next of freelist
    fwrite(&key_size, 8, 1, filemap->data);
    fwrite(&val_size, 8, 1, filemap->data);
    
    fwrite(key, key_size, 1, filemap->data); //write key if not exist
    
    fseek(filemap->index, res.index, SEEK_SET);
    fwrite(&res.data_pos, 8, 1, filemap->index); //write index
    
    fwrite(value, val_size, 1, filemap->data); //write value
  } else {
    fseek(filemap->data, res.data_pos + 8, SEEK_SET); //skip key size

    fwrite(&val_size, 8, 1, filemap->data); //write value size
    
    fseek(filemap->data, key_size, SEEK_CUR); //skip key

    fwrite(value, val_size, 1, filemap->data); //write value
  }

  filemap_unlock(filemap, hash);

  if (!res.exists) {
    rwlock_write(&filemap->resize);
    
    filemap->length++;

    rwlock_unwrite(&filemap->resize);
  }

  return res.exists;
}

filemap_mem_result filemap_findcpy(filemap_t* filemap, char* key, uint64_t key_size) {  
  uint64_t hash = do_hash(filemap, key, key_size);
  filemap_lock(filemap, hash);

  filemap_result res = filemap_find(filemap, hash, key, key_size);

  if (res.exists) {
    char* data = malloc(res.data_size);
    fread(data, res.data_size, 1, filemap->data);
    return (filemap_mem_result){.exists=1, .length=res.data_size, .data=data};
  } else {
    return (filemap_mem_result){.exists=0};
  }

  filemap_unlock(filemap, hash);
}

int filemap_remove(filemap_t* filemap, char* key, uint64_t key_size) {
  uint64_t hash = do_hash(filemap, key, key_size);
  filemap_lock(filemap, hash);
  
  filemap_result res = filemap_find(filemap, hash, key, key_size);

  if (res.exists) {
    if (freelist_insert(filemap, filemap->free, res.data_pos, res.data_size + key_size)) {
      filemap->free = res.data_pos;
    }

    //set original index to zero to indicate slot is empty
    fseek(filemap->index, res.index, SEEK_SET);
    uint64_t setptr = 0;
    fwrite(&setptr, 8, 1, filemap->index);

    filemap_unlock(filemap, hash);

    rwlock_write(&filemap->resize);
    
    filemap->length--;

    rwlock_unwrite(&filemap->resize);
  } else {

    filemap_unlock(filemap, hash);
    return 0;
  }
}

void filemap_free(filemap_t* filemap) {
  fseek(filemap->index, 2*4, SEEK_SET); //skip seed
  fwrite(&filemap->slots, 8, 1, filemap->index);
  fwrite(&filemap->length, 8, 1, filemap->index);

  fseek(filemap->data, 0, SEEK_SET);
  fwrite(&filemap->free, 8, 1, filemap->data);

  fclose(filemap->index);
  fclose(filemap->data);
}