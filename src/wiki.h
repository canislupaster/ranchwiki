// Automatically generated header.

#pragma once
#include "stdio.h"
#include <sys/stat.h>
#include "vector.h"
#include "stdlib.h"
#include "hashtable.h"
typedef struct {
  uint64_t pos;
  char* txt;
} add_t;
typedef struct {
  uint64_t pos;
	char* txt;
} del_t;
typedef struct {
  vector_t additions;
  vector_t deletions;
} diff_t;
typedef struct {
  FILE* file;

  char* current;
  vector_t diffs;
} text_t;
int parse_wiki_path(char* path, vector_t* vec);
void group_new(char* dirname);
text_t txt_new(char* filename);
void add_diff(text_t* txt, diff_t* d, char* current_str);
void read_txt(text_t* txt, uint64_t max);
void txt_free(text_t* txt);
