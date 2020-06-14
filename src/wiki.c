#include "stdlib.h"
#include "stdio.h"

#include <sys/stat.h>

#include "vector.h"
#include "hashtable.h"

#include "filemap.h"

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

void skipline(char** str) {
  while (**str != '\n') (*str)++;
  (*str)++;
}

size_t skipline_i(char* str) {
  size_t i=0;

  while (*str != '\n' && *str) {
    i++;
    str++;
  }

  if (*str == '\n') i++;
  
  return i;
}

//returns beginning of matching line
char* search_line(char* search, char* line) {
  size_t len;
  while (*search && (len = skipline_i(search))) {
    if (strncmp(search, line, len)==0) return search;
    search += len;
  }

  return NULL;
}

int parse_wiki_path(char* path, vector_t* vec) {
	char* segment = path;
	int alphabetical = 0;

	while (*path) {
		switch (*path) {
			case ' ': {
				if (!alphabetical) return 0;

				alphabetical = 0;
				break;
			}
			case 'a'...'z':
			case 'A'...'Z': {
				alphabetical = 1;
				break;
			}
			default: return 0;
		}

		path++;

		if (*path == '/' || !*path) {
			if (!alphabetical) return 0; //spaces cannot be at the end of a segment

			int len = path - segment;
			char* s = heapcpy(len+1, segment);
      vector_pushcpy(vec, &s);

      s[len] = 0;

      if (*path) path++;
			segment = path;
		}
	}

	if (vec->length == 0) return 0;

	return 1;
}

//may keep references to "to"
diff_t find_changes(char* from, char* to) {
  diff_t d;
  d.additions = vector_new(sizeof(add_t));
  d.deletions = vector_new(sizeof(del_t));

  if (!from) {
    vector_pushcpy(&d.additions, &(add_t){.pos=0, .txt=to});
  }

  char* from_start = from;

  char* insertion=NULL, *deletion=NULL;

  while (*to && *from) {
    size_t skip_len = skipline_i(from);

    if (strncmp(from, to, skip_len)!=0) {
      insertion = to;
      
      while (1) {
        deletion = search_line(from, to);

        if (deletion || !*to) { //EOF or skip
          if (!*to) {
            deletion = from + strlen(from); //delete to EOF, if EOF
          }

          if (to > insertion) {
            add_t* add = vector_pushcpy(&d.additions, &(add_t){.pos=from-from_start, .txt=heap(to-insertion+1)});
            strncpy(add->txt, insertion, to-insertion);
            add->txt[to-insertion] = 0;
          }

          if (deletion > from) {
            del_t* del = vector_pushcpy(&d.deletions, &(del_t){.pos=from-from_start, .txt=heap(deletion-from+1)});
            strncpy(del->txt, from, deletion-from);
						del->txt[deletion-from] = 0;
						from = deletion;
          }
          
          break;
        }

        skipline(&to);
      }
    } else {
      from += skip_len;
      to += skip_len;
    }
  }

  return d;
}

void group_new(char* dirname) {
  char* datapath = heapstr("%s%s", DATA_PATH, dirname);
  mkdir(datapath, 0775);
  drop(datapath);
}

text_t txt_new(char* filename) {
  text_t txt;

  char* datapath = heapstr("%s%s", DATA_PATH, filename);

  txt.file = fopen(datapath, "rb+");
  if (!txt.file) {
    txt.file = fopen(datapath, "wb+");
  }

  drop(datapath);

  txt.diffs = vector_new(sizeof(diff_t));
  return txt;
}

void add_diff(text_t* txt, diff_t* d, char* current_str) {
  uint64_t current = 8;
  fseek(txt->file, 0, SEEK_SET);
  if (fread(&current, 8, 1, txt->file) < 1) {
    fwrite(&current, 8, 1, txt->file); //pad beginning, we will rewrite later
  }
  
  uint64_t len, prev=0;
  
  fseek(txt->file, current, SEEK_SET);
  
  if (fread(&prev, 8, 1, txt->file) < 1) {
    fwrite(&prev, 8, 1, txt->file);
  }

  //write diff after prev
  len = (uint64_t)d->additions.length;
  fwrite(&len, 8, 1, txt->file);

  vector_iterator add_iter = vector_iterate(&d->additions);
  while (vector_next(&add_iter)) {
    add_t* add = add_iter.x;
    fwrite(&add->pos, 8, 1, txt->file);

    len = strlen(add->txt);
    fwrite(&len, 8, 1, txt->file);
    fwrite(add->txt, len, 1, txt->file);
  }

  len = (uint64_t)d->deletions.length;
  fwrite(&len, 8, 1, txt->file);

  vector_iterator del_iter = vector_iterate(&d->deletions);
  while (vector_next(&del_iter)) {
    del_t* del = del_iter.x;
    fwrite(&del->pos, 8, 1, txt->file);

		len = strlen(del->txt);
		fwrite(&len, 8, 1, txt->file);
		fwrite(del->txt, len, 1, txt->file);
  }

  current = ftell(txt->file);

  fwrite(&current, 8, 1, txt->file);

  len = strlen(current_str);
  fwrite(&len, 8, 1, txt->file);
  fwrite(current_str, len, 1, txt->file);

  fseek(txt->file, 0, SEEK_SET);
  fwrite(&current, 8, 1, txt->file);

  fclose(txt->file);
}

void read_txt(text_t* txt, uint64_t max) {
  if (!txt->file) {
    txt->current = NULL;
    return;
  }

  fseek(txt->file, 0, SEEK_SET);
  
  uint64_t current, length, prev;
  fread(&current, 8, 1, txt->file);
  fseek(txt->file, current, SEEK_SET);

  //prev diff | length | data
  fread(&prev, 8, 1, txt->file);
  fread(&length, 8, 1, txt->file);
  
  txt->current = heap(length+1);
  txt->current[length] = 0;

  fread(txt->current, length, 1, txt->file);

  for (uint64_t i=0; prev && i<max; i++) {
    fseek(txt->file, prev, SEEK_SET);
    
    fread(&prev, 8, 1, txt->file);
    fread(&length, 8, 1, txt->file); //length of additions

    diff_t* d = vector_push(&txt->diffs);
    d->additions = vector_new(sizeof(add_t));
    d->deletions = vector_new(sizeof(del_t));

    uint64_t length2;

    for (uint64_t i=0; i<length; i++) {
      add_t* add = vector_push(&d->additions);
      
      fread(&add->pos, 8, 1, txt->file);
      fread(&length2, 8, 1, txt->file);
      
      add->txt = heap(length2+1);
      add->txt[length2] = 0;

      fread(add->txt, length2, 1, txt->file);
    }

    fread(&length, 8, 1, txt->file); //length of deletions

    //same fokin thing
    for (uint64_t i=0; i<length; i++) {
      del_t* del = vector_push(&d->deletions);
      
      fread(&del->pos, 8, 1, txt->file);
			fread(&length2, 8, 1, txt->file);

			del->txt = heap(length2+1);
			del->txt[length2] = 0;

			fread(del->txt, length2, 1, txt->file);
    }
  }
}

void txt_free(text_t* txt) {
  vector_iterator diff_iter = vector_iterate(&txt->diffs);
  while (vector_next(&diff_iter)) {
    diff_t* d = diff_iter.x;

    vector_iterator add_iter = vector_iterate(&d->additions);
    while (vector_next(&add_iter)) {
      drop(((add_t*)add_iter.x)->txt);
    }

		vector_iterator del_iter = vector_iterate(&d->deletions);
		while (vector_next(&del_iter)) {
			drop(((del_t*)del_iter.x)->txt);
		}
	}

	drop(txt->current);
  fclose(txt->file);
}
