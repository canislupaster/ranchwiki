// Automatically generated header.

#pragma once
#include <string.h>
#include <err.h>
#include "event2/event.h"
#include "event2/bufferevent.h"
#include "vector.h"
#include "reasonphrases.h"
#include <sys/socket.h>
#include <sys/types.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include "event2/buffer.h"
#include "event2/listener.h"
#include "util.h"
#include "hashtable.h"
#include "context.h"
void respond(session_t* session, int stat, char* content, unsigned long len, header* headers, int headers_len);
void respond_redirect(session_t* session, char* url);
typedef struct {
	char* str;
	unsigned long skip; //if a condition, length, including !%
	unsigned long max_args; //required arguments
	unsigned long max_cond;
  unsigned long max_loop;
	vector_t substitutions;
} template_t;
typedef struct {
  int* cond_args;
  vector_t** loop_args;
  char** sub_args;
} template_args;
template_t template_new(char* data);
void respond_template(session_t* session, int stat, char* template_name, char* title, ...);
void respond_error(session_t* session, int stat, char* err);
char* percent_decode(char* data);
vector_t query_find(vector_t* query, char** params, int num_params, int strict);
void start_listen(ctx_t* ctx, const char *port);
