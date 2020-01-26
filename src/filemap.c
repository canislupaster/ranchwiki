#include <stdio.h>
#include <stdint.h> //ints for compatibility, since we are writing to files
#include <string.h>

#include <err.h>

#include "util.h"
#include "vector.h"

#include "threads.h"
#include "siphash.h"

#define LOAD_FACTOR 0.5
#define RESIZE_SLOTS 3
#define MUTEXES 10

typedef struct {
  mtx_t lock;
  FILE* file;

  uint16_t hash_seed[4];

  uint64_t length;

  uint64_t slots;
  //slots being resized out of all slots
  //theoretically this could be packed into slots
  //and slots will be rounded down to the nearest power of two
  //so bits switched beforehand would be the resize_slots
  uint64_t resize_slots;

  unsigned field;
} filemap_index_t;

// probe hashmap with an index and data
typedef struct {
  FILE* data;

  mtx_t lock; //lock for file io

  uint64_t free;

  unsigned fields;
} filemap_t;

typedef struct {
  uint64_t index;
  uint64_t data_pos;

  char exists;
} filemap_partial_object;

typedef struct {
  uint64_t data_pos;
  uint64_t data_size;

  char** fields;
  uint64_t* lengths;

  char exists;
} filemap_object;

// write slots to index and data
// assumes index is at correct point
void write_slots(filemap_index_t* index, unsigned int slots) {    
  for (unsigned i=0; i<slots; i++) {
    uint64_t pos = 0; //NULL pos, uninitialized
    fwrite(&pos, 8, 1, index->file);
  }
}

//seed and lengths, before indexes
const size_t PREAMBLE = 2*4 + (8*2);

filemap_t filemap_new(char* data, unsigned fields, int overwrite) {
  filemap_t filemap;

  filemap.data = fopen(data, "rb+");
  filemap.fields = fields;

  if (!filemap.data || overwrite) {
    filemap.data = fopen(data, "wb");
    filemap.free = 0;

    //initialize freelist
    fwrite(&filemap.free, 8, 1, filemap.data);
    filemap.data = freopen(data, "rb+", filemap.data);
  } else {
    fseek(filemap.data, 0, SEEK_SET);
    fread(&filemap.free, 8, 1, filemap.data);
  }

  mtx_init(&filemap.lock, mtx_plain);

  return filemap;
}

filemap_index_t filemap_index_new(char* index, unsigned field, int overwrite) {
  filemap_index_t new_index;

  new_index.file = fopen(index, "rb+");
  new_index.field = field;

  if (!new_index.file || overwrite) {
    new_index.file = fopen(index, "wb");

    //copied from hashtable
    for (unsigned char i = 0; i < 4; i++) {
			new_index.hash_seed[i] = rand(); //seed 16 bytes at a time
		}

    //go to beginning and write seed
    fseek(new_index.file, 0, SEEK_SET);
    fwrite(new_index.hash_seed, 2, 4, new_index.file);
    
    new_index.slots = 2;
    new_index.resize_slots = 0;
    new_index.length = 0;

    //uint64s -> 8 bytes
    fwrite(&new_index.slots, 8, 1, new_index.file);
    fwrite(&new_index.resize_slots, 8, 1, new_index.file);
    fwrite(&new_index.length, 8, 1, new_index.file);

    write_slots(&new_index, 2); //write slots afterwards

    freopen(index, "rb+", new_index.file);
  } else {

    //read the seed (2 bytes x 4)
    fread(new_index.hash_seed, 2, 4, new_index.file);

    fread(&new_index.slots, 8, 1, new_index.file);
    fread(&new_index.resize_slots, 8, 1, new_index.file);
    fread(&new_index.length, 8, 1, new_index.file);
  }

  mtx_init(&new_index.lock, mtx_plain);

  return new_index;
}

uint64_t do_hash(filemap_index_t* index, char* key, uint64_t key_size) {
  return siphash24((uint8_t*)key, key_size, (char*)index->hash_seed);
}

//does not check for equality / only returns uninitialized indices
//expects index already locked
static uint64_t filemap_nondup_find(filemap_index_t* index, uint64_t hash) {
  long probes = 0;
  uint64_t pos = 1;
  uint64_t index_pos;

  while (pos != 0) {
    uint64_t slot = (hash + (uint64_t)(0.5*(double)probes + 0.5*(double)(probes*probes))) % index->slots;
    fseek(index->file, PREAMBLE + (slot*8), SEEK_SET);

    index_pos = ftell(index->file);
    fread(&pos, 8, 1, index->file);

    probes++;
  }

  return index_pos;
}

//fields are in the format
//skip to one, skip to two, one, two
//so you can index the sizes without having to read them
//returns the size of the field (skipfield-skip(field-1))
//expects index already locked
uint64_t skip_fields(filemap_t* filemap, filemap_index_t* index) {
  uint64_t skip_field;
  uint64_t skip_field_before = 0;
  
  if (index->field > 0) {
    fseek(filemap->data, (index->field-1) * 8, SEEK_CUR);
    fread(&skip_field_before, 8, 1, filemap->data);
  }
  
  fread(&skip_field, 8, 1, filemap->data);

  //1 less field because we have read one
  fseek(filemap->data, ((filemap->fields - index->field-1) * 8) + skip_field_before, SEEK_CUR);
  return skip_field - skip_field_before;
}

//linear hashing
//add another bucket(s)
void filemap_resize(filemap_t* filemap, filemap_index_t* index) {
  mtx_lock(&index->lock);

  while ((double)index->length/(double)(index->slots + index->resize_slots) > LOAD_FACTOR) {
    fseek(index->file, 0, SEEK_END);

    write_slots(index, RESIZE_SLOTS);
    index->resize_slots += RESIZE_SLOTS;
    
    fseek(index->file, PREAMBLE, SEEK_SET);

    for (uint64_t i=0; i < RESIZE_SLOTS; i++) {
      uint64_t slot_pos = ftell(index->file);

      uint64_t pos=0;
      fread(&pos, 8, 1, index->file);

      if (!pos) continue;
      
      mtx_lock(&filemap->lock);

      fseek(filemap->data, pos, SEEK_SET);
      
      uint64_t f_size = skip_fields(filemap, index);

      char field[f_size];
      if (fread(field, (size_t)f_size, 1, filemap->data) < 1) {
        err(1, "corrupted database, field %i does not exist", index->field);
      }

      mtx_unlock(&filemap->lock);

      uint64_t hash = do_hash(index, field, f_size);
      
      if (hash % (index->slots*2) != i) { //will either be i or slots+i (next modular multiple? of i)
        uint64_t current = ftell(index->file);
        
        //seek back to pos, set to zero (since we are moving item)
        fseek(index->file, slot_pos, SEEK_SET);

        uint64_t set_pos = 0;
        fwrite(&set_pos, 8, 1, index->file);
        
        uint64_t new_index = filemap_nondup_find(index, hash);
        //write old position to new index
        fseek(index->file, new_index, SEEK_SET);
        fwrite(&pos, 8, 1, index->file);
        
        //seek back to current
        fseek(index->file, current, SEEK_SET);
      }
    }

    //resize complete
    if (index->resize_slots > index->slots) {
      index->resize_slots -= index->slots;
      index->slots *= 2;
    }
  }

  mtx_unlock(&index->lock);
}

filemap_partial_object filemap_find(filemap_t* filemap, filemap_index_t* index, char* key, uint64_t key_size) {
  uint64_t hash = do_hash(index, key, key_size);
  
  mtx_lock(&index->lock);
  
  filemap_partial_object obj;
  obj.exists = 0;

  long probes = 0;

  do {
    uint64_t slot = (hash + (uint64_t)(0.5*(double)probes + 0.5*(double)(probes*probes))) % index->slots;
    fseek(index->file, PREAMBLE + (slot*8), SEEK_SET);

    obj.index = ftell(index->file);
    fread(&obj.data_pos, 8, 1, index->file); //read pos from current slot

    if (obj.data_pos != 0) {
      mtx_lock(&filemap->lock);

      fseek(filemap->data, obj.data_pos, SEEK_SET);
      uint64_t cmp_key_size = skip_fields(filemap, index);

      if (cmp_key_size == key_size) {
        char cmp_key[key_size];
        fread(cmp_key, (size_t)cmp_key_size, 1, filemap->data);
        
        if (memcmp(key, cmp_key, key_size)==0) {
          mtx_unlock(&filemap->lock);
          mtx_unlock(&index->lock);
          
          obj.exists = 1;
          return obj;
        }
      }

      mtx_unlock(&filemap->lock);
    }

    probes++;

  } while (obj.data_pos != 0);

  mtx_unlock(&index->lock);

  return obj;
}

//returns 1 if list needs to be rearranged so that block is at the head
//locks data
int freelist_insert(filemap_t* filemap, uint64_t freelist, uint64_t block, uint64_t size) {
  uint64_t freelist_prev = 0;
  uint64_t cmp_size = UINT64_MAX;

  mtx_lock(&filemap->lock);

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

    mtx_unlock(&filemap->lock);
    return 0;
  } else {
    mtx_unlock(&filemap->lock);
    return block;
  }
}

//get a free block from freelist or end of data file
//data_size does not include key or data lengths
//locks data
uint64_t get_free(filemap_t* filemap, uint64_t data_size) {
  mtx_lock(&filemap->lock);

  if (filemap->free) {
    fseek(filemap->data, filemap->free, SEEK_SET);
    
    uint64_t size, next;
    
    fread(&size, 8, 1, filemap->data);
    fread(&next, 8, 1, filemap->data);

    if (size == data_size) {

      uint64_t free = filemap->free;
      filemap->free = next;

      mtx_unlock(&filemap->lock);

      return free;
    
    } else if (size > data_size && size - data_size >= 8*2) {
      //insert residue as free block
      uint64_t block = filemap->free + data_size;
      uint64_t freed = filemap->free;

      //mutex hell
      mtx_unlock(&filemap->lock);
      
      //set next or block as head
      int res = freelist_insert(filemap, next, block, size - data_size);
      
      mtx_lock(&filemap->lock);
      
      if (res) {
        filemap->free = block;
      } else {
        filemap->free = next;
      }

      mtx_unlock(&filemap->lock);

      return freed;
    }
  }

  fseek(filemap->data, 0, SEEK_END);
  uint64_t new_block = ftell(filemap->data);

  mtx_unlock(&filemap->lock);
  
  return new_block;
}

//adds obj to index
filemap_partial_object filemap_insert(filemap_t* filemap, filemap_index_t* index, filemap_object* obj) {
  char* key = obj->fields[index->field];
  uint64_t key_size = obj->lengths[index->field];

  filemap_resize(filemap, index);
  
  filemap_partial_object res = filemap_find(filemap, index, key, key_size);
  
  mtx_lock(&index->lock);
  
  fseek(index->file, res.index, SEEK_SET);
  fwrite(&obj->data_pos, 8, 1, index->file); //write index

  if (!res.exists) index->length++;

  mtx_unlock(&index->lock);
  
  return res;
}

void filemap_remove(filemap_t* filemap, filemap_index_t* index, filemap_partial_object* obj) {    
  mtx_lock(&index->lock);

  //set original index to zero to indicate slot is empty
  fseek(index->file, obj->index, SEEK_SET);
  uint64_t setptr = 0;
  fwrite(&setptr, 8, 1, index->file);

  index->length--;
  
  mtx_unlock(&index->lock);
}

filemap_object filemap_push(filemap_t* filemap, char** fields, uint64_t* lengths) {
  filemap_object obj;

  obj.data_size = 8*filemap->fields; //one for each skip
  for (unsigned i=0; i<filemap->fields; i++) {
    obj.data_size+=lengths[i];
  }

  if (obj.data_size < 8*2) {
    obj.exists = 0;
    return obj;
  }

  obj.fields = fields;
  obj.lengths = lengths;
  
  obj.data_pos = get_free(filemap, obj.data_size);
  
  mtx_lock(&filemap->lock);
  
  fseek(filemap->data, obj.data_pos, SEEK_SET);
  
  //write skips -- sum of all previous lengths
  uint64_t skip = 0;
  for (unsigned i=0; i<filemap->fields; i++) {
    skip += lengths[i];
    fwrite(&skip, 8, 1, filemap->data);
  }

  //write each field
  for (unsigned i=0; i<filemap->fields; i++) {
    fwrite(fields[i], lengths[i], 1, filemap->data);
  }

  mtx_unlock(&filemap->lock);

  obj.exists = 1;
  return obj;
}

filemap_object filemap_findcpy(filemap_t* filemap, filemap_index_t* index, char* key, uint64_t key_size) {  
  filemap_partial_object res = filemap_find(filemap, index, key, key_size);
  
  mtx_lock(&index->lock);

  filemap_object obj;

  if (res.exists) {
    obj.lengths = heap(8*filemap->fields);
    obj.fields = heap(sizeof(char*)*filemap->fields);

    mtx_lock(&filemap->lock);

    fseek(filemap->data, res.data_pos, SEEK_SET);

    //read lengths
    uint64_t sum=0; //use sum to decode skips

    for (unsigned i=0; i<filemap->fields; i++) {
      if (i==index->field) {
        obj.lengths[i] = key_size;
        fseek(filemap->data, 8, SEEK_CUR);
      } else {
        fread(&obj.lengths[i], 8, 1, filemap->data);
        obj.lengths[i] -= sum;
      }

      sum += obj.lengths[i];
    }

    //read data
    for (unsigned i=0; i<filemap->fields; i++) {
      if (obj.lengths[i] > 0) {
        obj.fields[i] = heap(obj.lengths[i]);
        fread(obj.fields[i], obj.lengths[i], 1, filemap->data);
      } else {
        obj.fields[i] = NULL;
      }
    }
    
    mtx_unlock(&filemap->lock);
    mtx_unlock(&index->lock);
    
    obj.exists = 1;
    return obj;
  } else {
    mtx_unlock(&index->lock);
    
    obj.exists = 0;
    return obj;
  }
}

void filemap_delete(filemap_t* filemap, filemap_partial_object* obj) {
  mtx_lock(&filemap->lock);

  //get sum / last skip
  fseek(filemap->data, obj->data_pos + (filemap->fields * 8), SEEK_SET);
  
  uint64_t size;
  fread(&size, 8, 1, filemap->data);

  mtx_unlock(&filemap->lock);

  if (freelist_insert(filemap, filemap->free, obj->data_pos, size)) {
    filemap->free = obj->data_pos;
  }
}

#define COPY_CHUNK 1024
static void filecpy(FILE* f, uint64_t from, uint64_t size, uint64_t to) {
  while (size>0) {
    uint64_t chunk_size = COPY_CHUNK > size ? size : COPY_CHUNK;
    size -= chunk_size;

    char chunk[chunk_size];
    
    fseek(f, from, SEEK_SET);
    fread(chunk, chunk_size, 1, f);

    fseek(f, to, SEEK_SET);
    fwrite(chunk, chunk_size, 1, f);

    from += chunk_size;
    to += chunk_size;
  }
}

void filemap_clean(filemap_t* filemap) {
  mtx_lock(&filemap->lock);
  
  uint64_t next = filemap->free;
  uint64_t offset = 0; //extra empty space from frees
  uint64_t size, free;
  
  //get all free zones, shift em backwards
  while (next) {
    fseek(filemap->data, next, SEEK_SET); //skip size
    free = next;

    fread(&size, 8, 1, filemap->data);
    fread(&next, 8, 1, filemap->data);

    filecpy(filemap->data, free+size, next-(free+size), free-offset); //shift everything after size before next back
    offset += size;
  }

  mtx_unlock(&filemap->lock);
}

void filemap_index_free(filemap_index_t* index) {
  fseek(index->file, 2*4, SEEK_SET); //skip seed
  fwrite(&index->slots, 8, 1, index->file);
  fwrite(&index->resize_slots, 8, 1, index->file);
  fwrite(&index->length, 8, 1, index->file);

  fclose(index->file);

  mtx_destroy(&index->lock);
}

void filemap_free(filemap_t* filemap) {
  fseek(filemap->data, 0, SEEK_SET);
  fwrite(&filemap->free, 8, 1, filemap->data);

  fclose(filemap->data);

  mtx_destroy(&filemap->lock);
}
