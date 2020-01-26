#include "stdlib.h"
#include "stdio.h"

#include "vector.h"
#include "hashtable.h"

#include "filemap.h"

#define DIFF_LIMIT 10 //limit which diffs are compressesd and a new base is created
#define SEARCH_AREA 7

typedef struct {
  uint64_t pos;
  char* txt;
} add_t;

typedef struct {
  uint64_t pos;
  uint64_t len;
} del_t;

typedef struct {
  vector_t additions;
  vector_t deletions;
} diff;

typedef struct {
  FILE* file;

  char* current;
  vector_t diffs;
} text;

void skipline(char** str) {
  while (**str != '\n') (*str)++;
  (*str)++;
}

size_t skipline_i(char* str) {
  size_t i=1;

  while (*str != '\n') {
    i++;
    str++;
  }
  
  str++;

  return i;
}

//returns beginning of matching line
char* search_line(char* search, char* in) {
  size_t len;
  while (*search && (len = skipline_i(search))) {
    if (strncmp(search, in, len)==0) return search;
    search += len;
  }

  return NULL;
}

//may keep references to "to"
diff find_changes(char* from, char* to) {
  diff d;
  d.additions = vector_new(sizeof(add_t));
  d.deletions = vector_new(sizeof(del_t));

  if (!from) {
    vector_pushcpy(&d.additions, &(add_t){.pos=0, .txt=to});
  }

  char* from_start = from;

  char* insertion=NULL, *deletion;

  while (*to && *from) {
    size_t skip_len = skipline_i(from);

    if (strncmp(from, to, skip_len)!=0) {
      insertion = to;
      
      while (1) {
        deletion = search_line(to, from);

        if (deletion || !*to) { //EOF or skip
          if (!*to) {
            deletion = from + strlen(from); //delete to EOF, if EOF
          }

          if (to-insertion>0) {
            add_t* a = vector_pushcpy(&d.additions, &(add_t){.pos=from-from_start, .txt=malloc(to-insertion+1)});
            strncpy(a->txt, insertion, to-insertion);
          }

          if (deletion-from>0) {
            vector_pushcpy(&d.deletions, &(del_t){.pos=from-from_start, .len=(deletion-from)});
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

text txt_new(char* filename) {
  text txt;
  
  txt.file = fopen(filename, "rb+");
  if (!txt.file) {
    txt.file = fopen(filename, "wb+");
  }

  txt.diffs = vector_new(sizeof(diff));
  return txt;
}

void add_diff(text* txt, diff* d, char* current_str) {
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

  prev = ftell(txt->file) - 8;
  
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
    fwrite(&del->len, 8, 1, txt->file);
  }

  current = ftell(txt->file);

  fwrite(&prev, 8, 1, txt->file);

  len = strlen(current_str);
  fwrite(&len, 8, 1, txt->file);
  fwrite(current_str, len, 1, txt->file);

  fseek(txt->file, 0, SEEK_SET);
  fwrite(&current, 8, 1, txt->file);

  fclose(txt->file);
}

void read_txt(text* txt, uint64_t max) {  
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
  
  txt->current = malloc(length+1);
  txt->current[length] = 0;

  fread(txt->current, length, 1, txt->file);

  while (prev && max) {
    max--;

    fseek(txt->file, prev, SEEK_SET);
    
    fread(&prev, 8, 1, txt->file);
    fread(&length, 8, 1, txt->file); //length of additions

    diff* d = vector_push(&txt->diffs);
    d->additions = vector_new(sizeof(add_t));
    d->deletions = vector_new(sizeof(del_t));

    uint64_t length2;

    for (uint64_t i=0; i<length; i++) {
      add_t* add = vector_push(&d->additions);
      
      fread(&add->pos, 8, 1, txt->file);
      fread(&length2, 8, 1, txt->file);
      
      add->txt = malloc(length2+1);
      add->txt[length2] = 0;

      fread(add->txt, length2, 1, txt->file);
    }

    fread(&length, 8, 1, txt->file); //length of deletions

    for (uint64_t i=0; i<length; i++) {
      del_t* del = vector_push(&d->deletions);
      
      fread(&del->pos, 8, 1, txt->file);
      fread(&del->len, 8, 1, txt->file);
    }
  }
}

void txt_free(text* txt) {
  vector_iterator diff_iter = vector_iterate(&txt->diffs);
  while (vector_next(&diff_iter)) {

    vector_iterator add_iter = vector_iterate(&((diff*)diff_iter.x)->additions);
    while (vector_next(&add_iter)) {
      free(((add_t*)add_iter.x)->txt);
    }
  }

  free(txt->current);
  fclose(txt->file);
}