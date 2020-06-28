// Automatically generated header.

#pragma once
#include "stdio.h"
#include "vector.h"
#include "hashtable.h"
#include "stdlib.h"
#include <sys/stat.h>
#define DATA_PATH "./data/"
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
diff_t find_changes(char* from, char* to);
vector_t make_path(vector_t* path);
text_t txt_new(char* filename);
void add_diff(text_t* txt, diff_t* d, char* current_str);
void read_txt(text_t* txt, uint64_t max);
void diff_free(diff_t* d);
void txt_free(text_t* txt);
