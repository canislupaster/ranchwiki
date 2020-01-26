// Automatically generated header.

#pragma once
#include "hashtable.h"
#include "stdlib.h"
#include "stdio.h"
#include "vector.h"
typedef struct {
  vector_t additions;
  vector_t deletions;
} diff;
typedef struct {
  FILE* file;

  char* current;
  vector_t diffs;
} text;
diff find_changes(char* from, char* to);
text txt_new(char* filename);
void add_diff(text* txt, diff* d, char* current_str);
void read_txt(text* txt, uint64_t max);
void txt_free(text* txt);
