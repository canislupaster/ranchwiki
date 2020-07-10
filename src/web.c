// web server part of thing
// most copied from
// https://nghttp2.org/documentation/tutorial-server.html
// i hope i am not infringing on any licenses

#include <sys/socket.h>
#include <sys/types.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <err.h>

#include "event2/event.h"
#include "event2/buffer.h"
#include "event2/listener.h"
#include "event2/bufferevent.h"

#include "util.h"
#include "vector.h"
#include "hashtable.h"
#include "threads.h"

#include "context.h"
#include "reasonphrases.h"

#define TIMEOUT 120

session_t *create_session(ctx_t *ctx, int fd, struct sockaddr *addr, int addrlen) {

  session_t *session = heap(sizeof(session_t));

  session->ctx = ctx;

  session->closed = 0;

  session->parser.done = 1;
  session->requests = vector_new(sizeof(request));

  session->bev = bufferevent_socket_new(
        ctx->evbase, fd, BEV_OPT_CLOSE_ON_FREE);
  bufferevent_enable(session->bev, EV_READ | EV_WRITE);

  char host[NI_MAXHOST];
  int rv = getnameinfo(addr, (socklen_t)addrlen, host, sizeof(host), NULL, 0,
                       NI_NUMERICHOST);

  if (rv != 0) {
    return NULL;
  } else {
    session->client_addr = strdup(host);
  }
	
	//initialize user session
	char* key = strdup(host);
	map_insert_result session_res = map_insert_locked(&session->ctx->user_sessions, &key);

	if (!session_res.exists) {
		*(user_session**)session_res.val = heap(sizeof(user_session));
		session->user_ses = *(user_session**)session_res.val;

		mtx_init(&session->user_ses->lock, mtx_plain);

		atomic_store(&session->user_ses->last_get, 0);
		atomic_store(&session->user_ses->last_lock, 0);
		atomic_store(&session->user_ses->last_access, 0);

		session->user_ses->user.exists = 0;
	} else {
		session->user_ses = *(user_session**)session_res.val;
	}
	
	rwlock_unwrite(session->ctx->user_sessions.lock);

  return session;
}

void req_free(request* req) {
  if (req->content) {
    drop(req->content);
  }

  vector_iterator query_iter = vector_iterate(&req->query);
  while (vector_next(&query_iter)) {
    query* q = query_iter.x;
    drop((*q)[0]);
    drop((*q)[1]);
  }

  vector_iterator mdata_iter = vector_iterate(&req->files);
  while (vector_next(&mdata_iter)) {
    multipart_data* d = mdata_iter.x;
    drop(d->name);
    drop(d->mime);
    drop(d->content);
  }

	vector_free_strings(&req->path);

  map_iterator header_iter = map_iterate(&req->headers);
  while (map_next(&header_iter)) {
    drop(*(char**)header_iter.key);
    drop(*(char**)header_iter.x);
  }

  vector_free(&req->query);
  vector_free(&req->files);
	map_free(&req->headers);
}

void terminate(session_t *session) {
  vector_iterator req_iter = vector_iterate(&session->requests);
  while (vector_next(&req_iter)) {
    req_free(req_iter.x);
  }

  if (!session->parser.done)
    if (session->parser.multipart_boundary)
      drop(session->parser.multipart_boundary);

  if (!session->parser.done && session->parser.req_parsed) {
    req_free(&session->parser.req);
  }

  bufferevent_disable(session->bev, EV_READ);

	mtx_unlock(&session->user_ses->lock);
	
  drop(session->client_addr);
  session->closed = 1;
}

void respond(session_t* session, int stat, char* content, unsigned long len, header* headers, int headers_len) {
  struct evbuffer* evbuf = bufferevent_get_output(session->bev);
  evbuffer_add_printf(evbuf, "HTTP/1.1 %i %s\r\n", stat, reason(stat));

  if (content) {
    evbuffer_add_printf(evbuf, "Content-Length:%lu\r\n", len);
  }

  for (int i=0; i<headers_len; i++) {
    evbuffer_add_printf(evbuf, "%s:%s\r\n", headers[i][0], headers[i][1]);
  }

  evbuffer_add_printf(evbuf, "\r\n");

  if (content) {
    evbuffer_add(evbuf, content, len);
  }
}

void respond_redirect(session_t* session, char* url) {
	respond(session, 302, "", 0, (header[1]){{"Location", url}}, 1);
}

void respond_html(session_t* session, int stat, char* content) {
  respond(session, stat, content, strlen(content), (header[1]){{"Content-Type", "text/html; charset=UTF-8"}}, 1);
}

void escape_html(char** str) {
	vector_t vec = vector_from_string(*str);
	vector_iterator iter = vector_iterate(&vec);
	while (vector_next(&iter)) {
		char* escaped;

		switch (*(char*)iter.x) {
			case '<': escaped="&lt;"; break;
			case '>': escaped="&gt;"; break;
			case '&': escaped="&amp;"; break;
			case '"': escaped="&quot;"; break;
			case '\'': escaped="&#39;"; break;
			default: escaped=NULL;
		}

		if (escaped) {
			vector_remove(&vec, iter.i-1);
			vector_insert_manycpy(&vec, iter.i-1, strlen(escaped), escaped);
			iter.i += strlen(escaped);
		}
	}

	*str = vec.data;
}

typedef struct {
	char* str;
	unsigned long skip; //if a condition, length, including !%
	unsigned long max_args; //required arguments
	unsigned long max_cond;
  unsigned long max_loop;
	vector_t substitutions;
} template_t;

typedef struct {
	unsigned long idx;
	unsigned long offset;

	template_t* insertion;
	char inverted; //inverted condition
  char loop;
  char noescape;
} substitution_t;

typedef struct {
  int* cond_args;
  vector_t** loop_args;
  char** sub_args;
} template_args;

template_t template_new(char* data) {
	template_t template;
	
	//use same cursor method as used in percent parsing
	unsigned long template_len = strlen(data)+1;
	template.str = heap(template_len);
	memset(template.str, 0, template_len);

	template.skip = 0;
	template.max_args = 0;
	template.max_cond = 0;
	template.max_loop = 0;

	template.substitutions = vector_new(sizeof(substitution_t));

	char* write_cursor = template.str;
	char* read_cursor = data;

	while (*read_cursor) {
		if (strncmp(read_cursor, "!%", 2)==0) {
			if (read_cursor > data && *(read_cursor-1)=='!') {
				read_cursor++;
				*write_cursor = '%';
			} else {
				template.skip = (read_cursor+2)-data;
				break;
			}
			
		} else if (*read_cursor == '%') {
			read_cursor++;

			//escape
			if (*read_cursor == '%') {
				*write_cursor = '%';

			//condition
			} else if (*read_cursor == '!') {
				read_cursor++;

				substitution_t* cond = vector_push(&template.substitutions);
				cond->offset = write_cursor-template.str;
				
        cond->inverted = 0;
        cond->loop = 0;

				if (*read_cursor == '*') {
          cond->loop = 1;
          read_cursor++;
        } else if (*read_cursor == '!') {
					cond->inverted = 1;
					read_cursor++;
				}

				cond->idx = *read_cursor - '0';
				if (cond->loop && cond->idx >= template.max_loop)
          template.max_loop = cond->idx+1;
				else if (cond->idx >= template.max_cond)
          template.max_cond = cond->idx+1;

				char* cond_start = ++read_cursor;
				template_t insertion = template_new(cond_start);

				if (insertion.max_args > template.max_args)
					template.max_args = insertion.max_args;
				
				if (insertion.max_cond > template.max_cond)
					template.max_cond = insertion.max_cond;

				if (insertion.max_loop > template.max_loop)
					template.max_loop = insertion.max_loop;

				read_cursor += insertion.skip;

				cond->insertion = heapcpy(sizeof(template_t), &insertion);
				continue;
				
			//index subsitution
			} else {
        substitution_t sub = {.offset=write_cursor-template.str};

        if (*read_cursor == '#') {
          sub.noescape = 1;
          read_cursor++;
        } else {
          sub.noescape = 0;
        }

				sub.idx = *read_cursor - '0';
				if (sub.idx >= template.max_args) template.max_args = sub.idx+1;

				vector_pushcpy(&template.substitutions, &sub);

				read_cursor++;
				continue;
			}
		} else {
			*write_cursor = *read_cursor;
		}
			
		write_cursor++;
		read_cursor++;
	}

	return template;
}

void template_length(template_t* template, unsigned long* len, template_args* args) {
	*len += strlen(template->str);

	vector_iterator iter = vector_iterate(&template->substitutions);
	while (vector_next(&iter)) {
		substitution_t* sub = iter.x;
		if (sub->insertion) {
      if (sub->loop) {
        vector_iterator iter = vector_iterate(args->loop_args[sub->idx]);
        while (vector_next(&iter))
          template_length(sub->insertion, len, iter.x);

        continue;
      }

			if (sub->inverted ^ !args->cond_args[sub->idx]) continue;
			template_length(sub->insertion, len, args);
		} else {
			char* arg = args->sub_args[sub->idx];

      if (sub->noescape) {
        *len += strlen(arg);
        continue;
      }

			while (*arg) {
				switch (*arg) {
					case '<': *len+=strlen("&lt;"); break;
					case '>': *len+=strlen("&gt;"); break;
					case '&': *len+=strlen("&amp;"); break;
					case '\'': *len+=strlen("&#39;"); break;
					case '"': *len+=strlen("&quot;"); break;
					default: (*len)++;
				}

				arg++;
			}
		}
	}
}

void template_substitute(template_t* template, char** out, template_args* args) {
	char* template_ptr = template->str;

	vector_iterator iter = vector_iterate(&template->substitutions);
	while (vector_next(&iter)) {
		substitution_t* sub = iter.x;

		unsigned long sub_before = sub->offset - (template_ptr - template->str);
		if (sub_before > 0) {
			memcpy(*out, template_ptr, sub_before);
			*out += sub_before;
			template_ptr += sub_before;
		}

		if (sub->insertion) {
      if (sub->loop) {
        vector_iterator iter = vector_iterate(args->loop_args[sub->idx]);
        while (vector_next(&iter)) {
          template_args* args = iter.x;
          template_substitute(sub->insertion, out, args);
          
          //convenience free
          if (args->sub_args)
            drop(args->sub_args);
          if (args->cond_args)
            drop(args->cond_args);
          if (args->loop_args)
            drop(args->loop_args);
         }

         vector_free(args->loop_args[sub->idx]);

        continue;
      }

			if (sub->inverted ^ !args->cond_args[sub->idx]) continue;
			template_substitute(sub->insertion, out, args);
		} else {
			char* arg = args->sub_args[sub->idx];

      if (sub->noescape) {
        while (*arg) *((*out)++) = *(arg++);
				continue;
      }

			while (*arg) {
				char* escaped;
 
				switch (*arg) {
					case '<': escaped="&lt;"; break;
					case '>': escaped="&gt;"; break;
					case '&': escaped="&amp;"; break;
					case '"': escaped="&quot;"; break;
					case '\'': escaped="&#39;"; break;
					default: escaped=NULL;
				}

				if (escaped) {
					memcpy(*out, escaped, strlen(escaped));
					*out += strlen(escaped);
				} else {
          **out = *arg;
          (*out)++;
				}

				arg++;
			}
		}
	}

	unsigned long rest = strlen(template->str) - (template_ptr-template->str);
	memcpy(*out, template_ptr, rest);
	*out += rest;
}

char* do_template(template_t* template, va_list args) {
  //allocate arrays on stack, then reference
	int cond_args[template->max_cond];
	for (unsigned long i=0; i<template->max_cond; i++) {
		cond_args[i] = va_arg(args, int);
	}

	vector_t* loop_args[template->max_loop];
	for (unsigned long i=0; i<template->max_loop; i++) {
		loop_args[i] = va_arg(args, vector_t*);
	}

	char* sub_args[template->max_args];
	for (unsigned long i=0; i<template->max_args; i++) {
		sub_args[i] = va_arg(args, char*);
	}

  template_args t_args = {.cond_args=cond_args, .loop_args=loop_args, .sub_args=sub_args};

	unsigned long len = 0;
	template_length(template, &len, &t_args);

	char* out = heap(len+1);
	char* out_ptr = out;
	template_substitute(template, &out_ptr, &t_args);
	out[len] = 0;

	return out;
}

void respond_template(session_t* session, int stat, char* template_name, char* title, ...) {
	va_list args;
	
	template_t* template = map_find(&session->ctx->templates, &template_name);

	va_start(args, title);
	
	char* template_output = do_template(template, args);
	
	va_end(args);

	char* escaped_title = heapcpystr(title);
	escape_html(&escaped_title);

	char* global_output = heapstr(session->ctx->global, escaped_title, template_output);

  respond_html(session, stat, global_output);

	drop(escaped_title);
  drop(template_output);
  drop(global_output);
}

void respond_error(session_t* session, int stat, char* err) {
  respond_template(session, stat, ERROR_TEMPLATE, err, err);
}

void skip(char** cur) {
  if (**cur) (*cur)++;
}

void parse_ws(char** cur) {
  while (**cur == ' ') (*cur)++;
}

void skip_until(char** cur, char* delim) {
  while (!strchr(delim, **cur) && **cur) (*cur)++;
}

void skip_while(char** cur, char* delim) {
  while (strchr(delim, **cur) && **cur) (*cur)++;
}

int skip_newline(char** cur) {
  if (**cur == '\r') skip(cur);
  if (**cur == '\n') {
    skip(cur);
    return 1;
  } else {
    return 0;
  }
}

char* parse_name(char** cur, char* delim) {
  char* start = *cur;
  while (!strchr(delim, **cur) && **cur) (*cur)++;

  char *str = heap(*cur - start + 1);
  memcpy(str, start, *cur - start);
  str[*cur - start] = 0;

  return str;
}

int skip_word(char** cur, const char* word) {
  if (strncmp(*cur, word, strlen(word))==0) {
    (*cur) += strlen(word);
    return 1;
  } else {
    return 0;
  }
}

// vector_t parse_list(char* data, char* delim) {
//   vector_t list = vector_new(sizeof(char*));

//   while (*data) {
//     parse_name(&data, delim);
//     data++; //skip delim
//   }
  
// }

//kinda copied from https://nachtimwald.com/2017/09/24/hex-encode-and-decode-in-c/
//since im too lazy to type all these ifs
char hexchar(char hex) {
  if (hex >= '0' && hex <= '9') {
		return hex - '0';
	} else if (hex >= 'A' && hex <= 'F') {
		return hex - 'A' + 10;
	} else if (hex >= 'a' && hex <= 'f') {
		return hex - 'a' + 10;
	} else {
		return 0;
	}
}

//frees original string
char* percent_decode(char* data) {
  char buffer[strlen(data)+1]; //temporary buffer, at most as long as data
  memset(buffer, 0, strlen(data)+1);

  unsigned long cur=0; //write cursor

  char* curdata = data; //copy data ptr
  
  while (*curdata) {
    if (*curdata == '+') {
      buffer[cur] = ' '; cur++;
    } else if (*curdata == '%') {
      skip(&curdata);
      char x = hexchar(*curdata) * 16;
      skip(&curdata);
      x += hexchar(*curdata);

      buffer[cur] = x; cur++;
    } else {
      buffer[cur] = *curdata; cur++;
    }

    curdata++;
  }

  drop(data);
  return heapcpy(strlen(buffer)+1, buffer); //truncate and copy buffer
}

void parse_querystring(char* line, vector_t* vec) {
  while (*line) {
    char* key = percent_decode(parse_name(&line, "="));
    skip(&line);
    char* val = percent_decode(parse_name(&line, "& "));
    
    vector_pushcpy(vec, &(query){key, val});

    if (*line == ' ') break;
    skip(&line); //skip &
  }
}

vector_t query_find(vector_t *vec, char **params, int num_params, int strict) {
  vector_t res = vector_new(sizeof(char*));

  if ((strict && vec->length != num_params) || vec->length > num_params)
    return res;

  vector_iterator iter = vector_iterate(vec);
  while (vector_next(&iter)) {
    char **q = iter.x;

    for (int i = 0; i < num_params; i++) {
      if (strcmp(q[0], params[i]) == 0) {
        vector_setcpy(&res, i, &q[1]);
      }
    }
  }

  return res;
}

//not sure i can make this generic without offsets
vector_t multipart_find(vector_t *vec, char **params, int num_params, int strict) {
	vector_t res = vector_new(sizeof(multipart_data));
	
  if ((strict && vec->length != num_params) || vec->length > num_params)
    return res;

  vector_iterator iter = vector_iterate(vec);
  while (vector_next(&iter)) {
    multipart_data* q = iter.x;

    for (int i = 0; i < num_params; i++) {
      if (strcmp(q->name, params[i]) == 0) {
        vector_setcpy(&res, i, q);
      }
    }
  }

  return res;
}

int request_parse(char* line, request* req) {
  parse_ws(&line);

  if (skip_word(&line, "GET")) {
    req->method = GET;
  } else if (skip_word(&line, "POST")) {
    req->method = POST;
  } else {
    return 0;
  }

  parse_ws(&line);
  
  req->path = vector_new(sizeof(char*));
  req->query = vector_new(sizeof(query));
  req->files = vector_new(sizeof(multipart_data));
  
  req->content = NULL;
  req->content_length = 0;

  if (*line == '*') {
    line++;
  } else {
    if (*line == '/') line++;

    while (*line != ' ') {
      if (strncmp(line, "../", strlen("../"))==0) {
        char* top = vector_popptr(&req->path);
        if (!top) {

          vector_free_strings(&req->path);
          vector_free(&req->query);

          return 0;
        } else {
          drop(top);
        }
      } else {
        char* segment = parse_name(&line, " /?");
        vector_pushcpy(&req->path, &segment);

        if (*line == '/') line++;
        if (*line == '?') {
          skip(&line); //skip ?
          parse_querystring(line, &req->query);
          break;
        }
      }
    }
  }

  req->headers = map_new();
  map_configure_string_key(&req->headers, sizeof(char*));

  return 1;
}

vector_t parse_header_value(char* val) {
	vector_t vec = vector_new(sizeof(char*[2]));

	vector_pushcpy(&vec, &(char*[]){heapcpy(1, ""), parse_name(&val, ",;")});

	while (*val && *val == ',') {
		val++;
		vector_pushcpy(&vec, &(char*[]){heapcpy(1, ""), parse_name(&val, ",;")});
	}

	while (*val) {
		if (*val==';') val++;

		parse_ws(&val);

		char* name = parse_name(&val, "=");

		while (*val && (*val == '=' || *val == ',')) {
			val++;
			
			//skip quotes
			if (*val=='"') {
				val++;
				vector_pushcpy(&vec, &(char*[]){heapcpystr(name), parse_name(&val, "\"")});
				if (*val=='"') val++;
			} else {
				vector_pushcpy(&vec, &(char*[]){heapcpystr(name), parse_name(&val, ",;")});
			}
		}
		
		drop(name);
	}

	return vec;
}

//drops the vector
//if extensibility is needed, follow the pattern used for query extraction
char* header_find_value(vector_t* val, char* key) {
	vector_iterator iter = vector_iterate(val);

	char* res = NULL;
	while (vector_next(&iter)) {
		char** assoc = iter.x;
		if (strcmp(assoc[0], key)==0) {
			res = assoc[1];
		} else {
			drop(assoc[1]);
		}

		drop(assoc[0]);
	}

	vector_free(val);

	return res;
}

int parse_content(session_t* session, struct evbuffer* evbuf) {
  if (evbuffer_get_length(evbuf) >= session->parser.req.content_length) {
    if (evbuffer_remove(evbuf, session->parser.req.content, session->parser.req.content_length)==-1) {
      respond_error(session, 500, "Buffer error");
      terminate(session);
      return 0;
    }
    
    //handle supported formats
    if (session->parser.req.ctype == url_formdata) {
			printf("query: %s\n", session->parser.req.content);
      parse_querystring(session->parser.req.content, &session->parser.req.query);
    } else if (session->parser.req.ctype == multipart_formdata) {
			char* content = session->parser.req.content;

			while (!skip_word(&content, session->parser.multipart_boundary) && *content) content++;
			skip_newline(&content);
			
			while (*content) {
				//parse preamble
				char* disposition=NULL, *mime=NULL;
				while (*content && !skip_newline(&content)) {
					if (skip_word(&content, "Content-Type:")) {
						parse_ws(&content);
						if (!mime) mime = parse_name(&content, "\r\n");
            skip_newline(&content);
					} else if (skip_word(&content, "Content-Disposition:")) {
						parse_ws(&content);
						if (!disposition) disposition = parse_name(&content, "\r\n");
            skip_newline(&content);
					} else {
						skip_until(&content, "\r\n");
            skip_newline(&content);
					}
				}

				if (!disposition) {
					if (disposition) drop(disposition);
					if (mime) drop(mime);

					respond_error(session, 500, "No disposition or mime");
					terminate(session);
					return 0;
				}

				vector_t val = parse_header_value(disposition);
				char* disposition_name = header_find_value(&val, "name");
				drop(disposition);
				
				char* data_start = content;
				
				unsigned long len = strlen(session->parser.multipart_boundary);
				while (strncmp(content, session->parser.multipart_boundary, len)!=0
							 && content - session->parser.req.content < session->parser.req.content_length)
					content++;
				
				unsigned long content_len = content-data_start;
				
				//shrink non-mime content
				if (!mime && *(content-1) == '\n') {
					content_len--;
					if (*(content-2) == '\r') content_len--;
				}
				
				multipart_data data = {.name=disposition_name, .mime=mime,
					.content=heapcpy(content_len, data_start), .len=content_len};
				vector_pushcpy(&session->parser.req.files, &data);

				if (*content) {
					content += strlen(session->parser.multipart_boundary);
					if (skip_word(&content, "--")) break;
					skip_newline(&content);
				}
			}

      drop(session->parser.multipart_boundary);
		}

    vector_pushcpy(&session->requests, &session->parser.req);
    session->parser.done = 1;

    return 1;
  } else {
		mtx_unlock(&session->user_ses->lock);
    return 0; //wait for more to arrive
  }
}


void route(session_t* session, request* req);

/* readcb for bufferevent after client connection header was
   checked. */
void readcb(struct bufferevent *bev, void* ctx) {
  session_t *session = (session_t *)ctx;
	mtx_lock(&session->user_ses->lock);

  struct evbuffer* evbuf = bufferevent_get_input(bev);
  
  char* line;

  //parse content
  if (!session->parser.done && session->parser.content_parsing) {
    if (!parse_content(session, evbuf)) return;
  }

  //parse request lines
  while ((line = evbuffer_readln(evbuf, NULL, EVBUFFER_EOL_CRLF))) {
    printf("%s\n", line); //log req

    //parse new request or finish old one
    if (session->parser.done) {
      session->parser.done = 0;
      session->parser.req_parsed = 0;
      session->parser.has_content = 0;
      session->parser.content_parsing = 0;
			session->parser.multipart_boundary = NULL;
    }
    
    if (strlen(line) == 0 && session->parser.req_parsed) {
      if (!session->parser.has_content) {
        vector_pushcpy(&session->requests, &session->parser.req);
        session->parser.done = 1;
      } else {
        const char* clength_name = "Content-Type";
        char** ctype = map_find(&session->parser.req.headers, &clength_name);

				const char* multipart_name = "multipart/form-data";

        //handle supported formats
        if (ctype && strcmp(*ctype, "application/x-www-form-urlencoded")==0) {
          session->parser.req.ctype = url_formdata;
				} else if (ctype && strncmp(*ctype, multipart_name, strlen(multipart_name))==0) {
					session->parser.req.ctype = multipart_formdata;

					vector_t ctype_val = parse_header_value(*ctype);
					session->parser.multipart_boundary = header_find_value(&ctype_val, "boundary");

          char* prefixed = heapstr("--%s", session->parser.multipart_boundary);
          drop(session->parser.multipart_boundary);
          session->parser.multipart_boundary = prefixed;

					if (!session->parser.multipart_boundary) {
						respond_error(session, 400, "no boundary provided with multipart request");
						terminate(session);
						return;
					}
        } else {
          respond_error(session, 400, "unsupported body format");
          terminate(session);
          return;
        }

				session->parser.req.content = heap(session->parser.req.content_length + 1);
				session->parser.req.content[session->parser.req.content_length] = 0;

        session->parser.content_parsing = 1;
        
				if (!parse_content(session, evbuf)) return;
      }
    
    //parse request
    } else if (!session->parser.req_parsed) {
      if (request_parse(line, &session->parser.req)) {
        session->parser.req_parsed = 1;
      } else {
        respond_error(session, 400, "Malformed request");
				terminate(session);
				return;
			}
    } else {
      char* cur = line; //copy line to use as cursor
      if (skip_word(&cur, "Content-Length:")) {
        session->parser.req.content_length = strtoul(cur, &cur, 10);
        session->parser.has_content = 1;

        if (session->parser.req.content_length > CONTENT_MAX) {
          respond_error(session, 400, "Oversized content");
					terminate(session);
					return;
				}
      } else {
        char* name = parse_name(&cur, ":");
        skip(&cur);
        parse_ws(&cur);
        char* val = parse_name(&cur, "");

        map_insertcpy(&session->parser.req.headers, &name, &val);
      }
    }

    drop(line);
  }

  //respond to pending requests
  vector_iterator req_iter = vector_iterate(&session->requests);
  while (vector_next(&req_iter)) {
    route(session, req_iter.x);

		//store last access for session expiry
		atomic_store(&session->user_ses->last_access, time(NULL));

    req_free(req_iter.x);
  }

  vector_clear(&session->requests);
	mtx_unlock(&session->user_ses->lock);
}

void listen_error(struct evconnlistener* listener, void* ctx) {
  struct event_base *base = evconnlistener_get_base(listener);

  int err = EVUTIL_SOCKET_ERROR();
  event_base_loopexit(base, NULL);

  fprintf(stderr, "listener error %i %s\n", err, evutil_socket_error_to_string(err));
}

void eventcb(struct bufferevent *bev, short events, void* ctx) {
  session_t *session = (session_t *)ctx;
  
  if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
		mtx_lock(&session->user_ses->lock);
		
    bufferevent_free(session->bev);
    if (!session->closed) terminate(session);
    drop(session);
	}
}

void acceptcb(struct evconnlistener *listener, int fd, struct sockaddr *addr,
              int addrlen, void *arg) {

  ctx_t *ctx = (ctx_t *)arg;
  session_t *session;

  session = create_session(ctx, fd, addr, addrlen);
	if (!session) return;

  struct timeval tout = {.tv_sec=TIMEOUT, .tv_usec=0};
  bufferevent_set_timeouts(session->bev, &tout, NULL);
  bufferevent_setcb(session->bev, readcb, NULL, eventcb, session);
}

void start_listen(ctx_t* ctx, const char *port) {
  struct addrinfo hints;

  memset(&hints, 0, sizeof(hints));

  hints.ai_family = AF_INET;
	hints.ai_protocol = IPPROTO_TCP;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE || AI_NUMERICSERV || AI_ADDRCONFIG;

  struct addrinfo *res;
  int rv = getaddrinfo(NULL, port, &hints, &res);
  
  if (rv != 0) {
    errx(1, "could not resolve server address");
  }

  //search for viable address
  for (struct addrinfo* cur = res; cur; cur = cur->ai_next) {
    struct evconnlistener *listener;
    
    listener = evconnlistener_new_bind(
        ctx->evbase, acceptcb, ctx, LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, 16,
        cur->ai_addr, (int)cur->ai_addrlen);

    if (listener) {
      evconnlistener_set_error_cb(listener, listen_error);
      freeaddrinfo(res);

      return;
    }
  }

  errx(1, "could not start listener %i", errno);
}
