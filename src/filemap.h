// Automatically generated header.

#pragma once
#include <stdint.h> //ints for compatibility, since we are writing to files
#include <err.h>
#include <stdio.h>
#include <string.h>
#include "util.h"
#include "vector.h"
#include "threads.h"
#include "siphash.h"
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
filemap_t filemap_new(char* data, unsigned fields, int overwrite);
filemap_index_t filemap_index_new(char* index, unsigned field, int overwrite);
filemap_partial_object filemap_find(filemap_t* filemap, filemap_index_t* index, char* key, uint64_t key_size);
filemap_partial_object filemap_insert(filemap_t* filemap, filemap_index_t* index, filemap_object* obj);
filemap_object filemap_push(filemap_t* filemap, char** fields, uint64_t* lengths);
filemap_object filemap_findcpy(filemap_t* filemap, filemap_index_t* index, char* key, uint64_t key_size);
void filemap_index_free(filemap_index_t* index);
void filemap_free(filemap_t* filemap);
