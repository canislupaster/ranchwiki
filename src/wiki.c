#include "stdlib.h"
#include "stdio.h"

#include <sys/stat.h>

#include "vector.h"
#include "hashtable.h"

#include "filemap.h"
#include "context.h"

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

int skipline(char** str) {
	while (**str != '\n' && **str != '\r' && **str) (*str)++;
	if (!**str) return 0;
	
	(*str)++;
	if (**str=='\n') (*str)++;
	
	return 1;
}

size_t skipline_i(char* str) {
	size_t i=0;

	while (*str != '\r' && *str != '\n' && *str) {
		i++;
		str++;
	}
	
	return i;
}

//returns beginning of matching line
char* search_line(char* search, char* line, size_t linelen) {
	size_t len;
	while (*search && (len = skipline_i(search))) {
		if (len == linelen && strncmp(search, line, len)==0) return search;
		search += len;
		
		skipline(&search);
	}

	return NULL;
}

//copies (obv...)
int parse_wiki_path(char* path, vector_t* vec) {
	int alphabetical = 0;

	while (*path && *path=='/') path++;
	char* segment = path;

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
		vector_pushcpy(&d.additions, &(add_t){.pos=0, .txt=heapcpystr(to)});
		return d;
	}

	char* from_start = from;

	char* insertion=to, *deletion=from;

	while (*to || *from) {
		size_t from_skip_len = skipline_i(from);
		size_t to_skip_len = skipline_i(to);

		if (from_skip_len != to_skip_len || strncmp(from, to, from_skip_len)!=0) {
			while (1) {
				deletion = search_line(from, to, to_skip_len); //includes newline at start of line

				if (deletion || !*to) { //EOF or skip
					if (!*to) {
						deletion = from + strlen(from); //delete and insert to EOF, if EOF
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

				to += to_skip_len;
				skipline(&to);
				
				to_skip_len = skipline_i(to);
			}
		} else {
			from += from_skip_len;
			to += to_skip_len;
			
			//match newline inclusion
			if (!skipline(&from)) {
				insertion = to;
				skipline(&to);
			} else {
				skipline(&to);
				insertion = to;
			}
		}
	}

	return d;
}

void group_new(char* dirname) {
	mkdir(dirname, 0775);
}

vector_t make_path(vector_t* path) {
	vector_t out_path = vector_new(1);
	vector_stockstr(&out_path, DATA_PATH);

	vector_iterator iter = vector_iterate(path);

	while (vector_next(&iter)) {
		char* segment = *(char**)iter.x;
		vector_stockcpy(&out_path, strlen(segment), segment);

		if (iter.i != path->length) {
			vector_pushcpy(&out_path, "/");
			vector_pushcpy(&out_path, "\0");

			group_new(out_path.data);

			vector_pop(&out_path);
		}
	}

	vector_pushcpy(&out_path, "\0");
	return out_path;
}

text_t txt_new(char* filename) {
	text_t txt;

	txt.file = fopen(filename, "rb+");
	if (!txt.file) {
		txt.file = fopen(filename, "wb+");
	}

	txt.diffs = vector_new(sizeof(diff_t));
	return txt;
}

void add_diff(text_t* txt, diff_t* d, char* current_str) {
	char one = 1;	 //prefix uint64s with one for a prefix encoding of diff separators

	uint64_t current = 9;
	fseek(txt->file, 1, SEEK_SET);
	if (fread(&current, 8, 1, txt->file) < 1) {
		fseek(txt->file, 0, SEEK_SET);
		fwrite(&one, 1, 1, txt->file);
		fwrite(&current, 8, 1, txt->file); //pad beginning, we will rewrite later
	}
	
	uint64_t len, prev;

	fseek(txt->file, current+1, SEEK_SET);
	
	//replace current with diff
	
	if (fread(&prev, 8, 1, txt->file) < 1)
		prev = 0;

	fseek(txt->file, current, SEEK_SET);
	
	char sep[10] = {1, 0}; //separate diff
	fwrite(sep, 10, 1, txt->file);
	
	fwrite(&one, 1, 1, txt->file);
	fwrite(&prev, 8, 1, txt->file);
	
	fwrite(&one, 1, 1, txt->file);
	fwrite(&d->author, 8, 1, txt->file);

	fwrite(&one, 1, 1, txt->file);
	fwrite(&d->time, 8, 1, txt->file);

	len = (uint64_t)d->additions.length;
	fwrite(&one, 1, 1, txt->file);
	fwrite(&len, 8, 1, txt->file);

	vector_iterator add_iter = vector_iterate(&d->additions);
	while (vector_next(&add_iter)) {
		add_t* add = add_iter.x;
		fwrite(&one, 1, 1, txt->file);
		fwrite(&add->pos, 8, 1, txt->file);

		len = strlen(add->txt);
		fwrite(&one, 1, 1, txt->file);
		fwrite(&len, 8, 1, txt->file);

		fwrite(add->txt, len, 1, txt->file);
	}

	len = (uint64_t)d->deletions.length;
	fwrite(&one, 1, 1, txt->file);
	fwrite(&len, 8, 1, txt->file);

	vector_iterator del_iter = vector_iterate(&d->deletions);
	while (vector_next(&del_iter)) {
		del_t* del = del_iter.x;
		fwrite(&one, 1, 1, txt->file);
		fwrite(&del->pos, 8, 1, txt->file);

		len = strlen(del->txt);
		fwrite(&one, 1, 1, txt->file);
		fwrite(&len, 8, 1, txt->file);

		fwrite(del->txt, len, 1, txt->file);
	}

	uint64_t new_current = ftell(txt->file);

	fwrite(&one, 1, 1, txt->file);
	fwrite(&current, 8, 1, txt->file); //write prev

	len = strlen(current_str);
	fwrite(&one, 1, 1, txt->file);
	fwrite(&len, 8, 1, txt->file);

	fwrite(current_str, len, 1, txt->file);

	fseek(txt->file, 0, SEEK_SET);
	fwrite(&one, 1, 1, txt->file);
	fwrite(&new_current, 8, 1, txt->file);
}

void read_txt(text_t* txt, uint64_t start, uint64_t max) {
	fseek(txt->file, 1, SEEK_SET);
	
	uint64_t current, length, prev;
	if (fread(&current, 8, 1, txt->file)<1) {
		txt->current = NULL;
		return;
	}

	fseek(txt->file, current+1, SEEK_SET);

	//prev diff | length | data
	fread(&prev, 8, 1, txt->file);
	fseek(txt->file, 1, SEEK_CUR);
	fread(&length, 8, 1, txt->file);
	
	txt->current = heap(length+1);
	txt->current[length] = 0;

	fread(txt->current, length, 1, txt->file);

	for (uint64_t i=0; prev>0 && i<max; i++) {
		if (start>0 && i==0) {
			fseek(txt->file, 0, SEEK_END);
			if (start >= ftell(txt->file)) return;

			fseek(txt->file, start, SEEK_SET);

			char pre[10];
			if (fread(pre, 10, 1, txt->file)<1) return;
			if (memcmp(pre, (char[10]){1, 0}, 10)!=0) return;
		
			fseek(txt->file, 1, SEEK_CUR);
		} else {
			fseek(txt->file, prev+10+1, SEEK_SET); //skip prefix and one
		}
		
		fread(&prev, 8, 1, txt->file);

		diff_t* d = vector_push(&txt->diffs);
		d->additions = vector_new(sizeof(add_t));
		d->deletions = vector_new(sizeof(del_t));
		d->prev = prev;

		fseek(txt->file, 1, SEEK_CUR);
		fread(&d->author, 8, 1, txt->file);

		fseek(txt->file, 1, SEEK_CUR);
		fread(&d->time, 8, 1, txt->file);

		fseek(txt->file, 1, SEEK_CUR);
		fread(&length, 8, 1, txt->file); //length of additions

		uint64_t length2;

		for (uint64_t i=0; i<length; i++) {
			add_t* add = vector_push(&d->additions);
			
			fseek(txt->file, 1, SEEK_CUR);
			fread(&add->pos, 8, 1, txt->file);
			fseek(txt->file, 1, SEEK_CUR);
			fread(&length2, 8, 1, txt->file);
			
			add->txt = heap(length2+1);
			add->txt[length2] = 0;

			fread(add->txt, length2, 1, txt->file);
		}

		fseek(txt->file, 1, SEEK_CUR);
		fread(&length, 8, 1, txt->file); //length of deletions

		//same fokin thing
		for (uint64_t i=0; i<length; i++) {
			del_t* del = vector_push(&d->deletions);
			
			fseek(txt->file, 1, SEEK_CUR);
			fread(&del->pos, 8, 1, txt->file);
			fseek(txt->file, 1, SEEK_CUR);
			fread(&length2, 8, 1, txt->file);

			del->txt = heap(length2+1);
			del->txt[length2] = 0;

			fread(del->txt, length2, 1, txt->file);
		}
	}
}

typedef struct {
	char* str;
	unsigned long len;

	enum {dseg_add, dseg_del, dseg_current} ty;
	unsigned long diff;
} dseg;

//origin comes before, new overlaps/is adjacent in order
void split_dseg(vector_t* segs, dseg new_seg, unsigned long last_segi, unsigned long start) {
	unsigned long slen=0;

	vector_iterator iter = vector_iterate(segs);
	iter.i = last_segi;

	while (slen < new_seg.len+start) {
		vector_next(&iter);
		
		dseg* seg = iter.x;
		slen += seg->len;
	}

	unsigned long remove_from = last_segi;
	
	if (start!=0) {
		dseg* first = vector_get(segs, last_segi);
		first->len = start;
		
		slen -= start;
		
		remove_from++;
	}
	
	dseg last = *(dseg*)vector_get(segs, iter.i-1);

	if (slen > new_seg.len) {
		last.str += last.len - (slen-new_seg.len);
		last.len = slen-new_seg.len;
		
		vector_insertcpy(segs, iter.i, &last);
	}

	//remove everything in between and insert new seg at beginning
		
	if (iter.i > remove_from)
		vector_removemany(segs, remove_from, iter.i-remove_from);
	
	vector_insertcpy(segs, remove_from, &new_seg);

	iter = vector_iterate(segs);
	while (vector_next(&iter)) {
		dseg* s = iter.x;
		if (!s->str) abort();
	}
}

vector_t display_diffs(text_t* txt) {
	vector_t segs = vector_new(sizeof(dseg));
	vector_pushcpy(&segs, &(dseg){.len=strlen(txt->current), .ty=dseg_current, .str=txt->current});

	vector_iterator iter = vector_iterate(&txt->diffs);
	while (vector_next(&iter)) {
		diff_t* d = iter.x;
		
		//process removes first
		int iteradd=0;
		vector_iterator d_iter = vector_iterate(&d->deletions);

		while (1) {
			if (!vector_next(&d_iter)) {
				if (iteradd) break;
				
				d_iter = vector_iterate(&d->additions);
				iteradd=1;
				continue;
			}
			
			//i know theyre the same thing but when i find a gullible contributor there will be changes
			add_t* add = d_iter.x;
			del_t* del = d_iter.x;
			//i know i am

			//ehhhhh
			vector_iterator segiter = vector_iterate(&segs);
			unsigned long pos=0;

			dseg* seg;

			while (vector_next(&segiter)) {
				if (pos > (iteradd ? add->pos : del->pos)) break;

				seg = segiter.x;
				if (seg->ty == dseg_add) continue;

				pos += seg->len;
			}

			split_dseg(&segs,
				(dseg){.diff=iter.i-1, .len=strlen(add->txt), .ty=iteradd ? dseg_add : dseg_del,
					.str=iteradd ? add->txt : del->txt}, segiter.i-2,
					seg->ty==dseg_add ? 0 : add->pos-(pos-seg->len));
		}
	}

	return segs;
}

void diff_free(diff_t* d) {
	vector_iterator add_iter = vector_iterate(&d->additions);
	while (vector_next(&add_iter)) {
		drop(((add_t*)add_iter.x)->txt);
	}

	vector_iterator del_iter = vector_iterate(&d->deletions);
	while (vector_next(&del_iter)) {
		drop(((del_t*)del_iter.x)->txt);
	}
	
	vector_free(&d->additions);
	vector_free(&d->deletions);
}

void txt_free(text_t* txt) {
	vector_iterator diff_iter = vector_iterate(&txt->diffs);
	while (vector_next(&diff_iter)) {
		diff_free(diff_iter.x);
	}
	
	vector_free(&txt->diffs);

	drop(txt->current);
	fclose(txt->file);
}
