// Automatically generated header.

#pragma once
#include "stdlib.h"
#include "stdio.h"
#include <sys/stat.h>
#include "vector.h"
#include "hashtable.h"
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
	uint64_t author;
	uint64_t time;
	
	uint64_t prev; //only returned, otherwise garbage
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
void read_txt(text_t* txt, uint64_t start, uint64_t max);
typedef struct {
	char* str;
	unsigned long len;

	enum {dseg_add, dseg_del, dseg_current} ty;
	unsigned long diff;
} dseg;
vector_t display_diffs(text_t* txt);
void diff_free(diff_t* d);
void txt_free(text_t* txt);
