// Automatically generated header.

#pragma once
#include <stdint.h> //ints for compatibility, since we are writing to files
#include <stdio.h>
#include <string.h>
#include "util.h"
#include "siphash.h"
typedef struct {
  FILE* index; //stores indexes which to search in data
  FILE* data; //stores key value pairs

  uint16_t hash_seed[4];

  uint64_t free;

  uint64_t length;
  uint64_t slots;
} filemap_t;
typedef struct {
  char exists;

  uint64_t length;
  char* data;
} filemap_mem_result;
int filemap_new(filemap_t* filemap, char* index, char* data);
int filemap_insert(filemap_t* filemap, char* key, char* value, uint64_t key_size, uint64_t val_size);
filemap_mem_result filemap_findcpy(filemap_t* filemap, char* key, uint64_t key_size);
int filemap_remove(filemap_t* filemap, char* key, uint64_t key_size);
void filemap_free(filemap_t* filemap);
