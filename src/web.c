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

#include "context.h"
#include "router.h"
#include "reasonphrases.h"

session_t *create_session(ctx_t *ctx, int fd, struct sockaddr *addr, int addrlen) {

  session_t *session = heap(sizeof(session_t));

  session->ctx = ctx;

  session->parser.done = 1;
  session->requests = vector_new(sizeof(request));

  session->bev = bufferevent_socket_new(
        ctx->evbase, fd, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
  bufferevent_enable(session->bev, EV_READ | EV_WRITE);

  char host[NI_MAXHOST];
  int rv = getnameinfo(addr, (socklen_t)addrlen, host, sizeof(host), NULL, 0,
                       NI_NUMERICHOST);

  if (rv != 0) {
    session->client_addr = strdup("(unknown)");
  } else {
    session->client_addr = strdup(host);
  }

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

  vector_iterator path_iter = vector_iterate(&req->path);
  while (vector_next(&path_iter)) {
    drop(*(char**)path_iter.x);
  }

  map_iterator header_iter = map_iterate(&req->headers);
  while (map_next(&header_iter)) {
    drop(*(char**)header_iter.key);
    drop(*(char**)header_iter.x);
  }
}

void terminate(session_t *session) {
  vector_iterator req_iter = vector_iterate(&session->requests);
  while (vector_next(&req_iter)) {
    req_free(req_iter.x);
  }

  if (!session->parser.done && session->parser.req_parsed) {
    req_free(&session->parser.req);
  }

  bufferevent_free(session->bev);

  drop(session->client_addr);
  drop(session);
}

void respond(session_t* session, int stat, char* content, header* headers, int headers_len) {
  struct evbuffer* evbuf = bufferevent_get_output(session->bev);
  evbuffer_add_printf(evbuf, "HTTP/1.1 %i %s\r\n", stat, reason(stat));

  if (content) {
    evbuffer_add_printf(evbuf, "Content-Length:%lu\r\n", strlen(content));
  }

  for (int i=0; i<headers_len; i++) {
    evbuffer_add_printf(evbuf, "%s:%s\r\n", headers[i][0], headers[i][1]);
  }

  evbuffer_add_printf(evbuf, "\r\n");

  if (content) {
    evbuffer_add(evbuf, content, strlen(content));
  }
}

void respond_html(session_t* session, int stat, char* content) {
  respond(session, stat, content, (header[1]){{"Content-Type", "text/html; charset=UTF-8"}}, 1);
}

void respond_error(session_t* session, int stat, char* err) {
  char* template = map_find(&session->ctx->templates, &ERROR_TEMPLATE);
  respond_html(session, stat, heapstr(template, err));
  
  terminate(session);
}

void skip(char** cur) {
  if (**cur) (*cur)++;
}

void parse_ws(char** cur) {
  while (**cur == ' ') (*cur)++;
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
  char buffer[strlen(data)+1]; //temporary buffer, at least as long as data
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
      skip(&curdata);

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
  
  req->content = NULL;
  req->content_length = 0;

  if (*line == '*') {
    line++;
  } else {
    if (*line == '/') line++;

    while (*line != ' ') {
      if (strncmp(line, "../", sizeof("../"))) {
        if (!vector_pop(&req->path)) {

          vector_free(&req->path);
          vector_free(&req->query);

          return 0;
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

int parse_content(session_t* session, struct evbuffer* evbuf) {
  if (evbuffer_get_length(evbuf) >= session->parser.req.content_length) {
    if (evbuffer_remove(evbuf, session->parser.req.content, session->parser.req.content_length)==-1) {
      respond_error(session, 500, "Buffer error"); return 0;
    }
    
    //handle supported formats
    if (session->parser.req.ctype == url_formdata) {
      parse_querystring(session->parser.req.content, &session->parser.req.query);
    }

    vector_pushcpy(&session->requests, &session->parser.req);
    session->parser.done = 1;

    return 1;
  } else {
    return 0; //wait for more to arrive
  }
}

/* readcb for bufferevent after client connection header was
   checked. */
void readcb(struct bufferevent *bev, void* ctx) {
  session_t *session = (session_t *)ctx;

  struct evbuffer* evbuf = bufferevent_get_input(bev);
  
  char* line;

  //parse content
  if (!session->parser.done && session->parser.content_parsing) {
    if (!parse_content(session, evbuf)) return;
  }

  //parse request lines
  while ((line = evbuffer_readln(evbuf, NULL, EVBUFFER_EOL_CRLF))) {
    //parse new request or finish old one
    if (session->parser.done) {
      session->parser.done = 0;
      session->parser.req_parsed = 0;
      session->parser.has_content = 0;
      session->parser.content_parsing = 0;
    }
    
    if (strlen(line) == 0 && session->parser.req_parsed) {
      if (!session->parser.has_content) {
        vector_pushcpy(&session->requests, &session->parser.req);
        session->parser.done = 1;
      } else {
        const char* clength_name = "Content-Type";
        char** ctype = map_find(&session->parser.req.headers, &clength_name);

        //handle supported formats
        if (ctype && strcmp(*ctype, "application/x-www-form-urlencoded")==0) {
          session->parser.req.ctype = url_formdata;

          session->parser.req.content = heap(session->parser.req.content_length + 1);
          session->parser.req.content[session->parser.req.content_length] = 0;
        } else {
          respond_error(session, 400, "Unsupported body format"); return;
        }

        session->parser.content_parsing = 1;
        
        if (!parse_content(session, evbuf)) return;
      }
    
    //parse request
    } else if (!session->parser.req_parsed) {
      if (request_parse(line, &session->parser.req)) {
        session->parser.req_parsed = 1;
      } else {
        respond_error(session, 400, "Malformed request"); return;
      }
    } else {
      char* cur = line; //copy line to use as cursor
      if (skip_word(&cur, "Content-Length:")) {
        session->parser.req.content_length = (unsigned long)strtol(cur, &cur, 10);
        session->parser.has_content = 1;

        if (session->parser.req.content_length > CONTENT_MAX) {
          respond_error(session, 400, "Oversized content"); return;
        }
      } else {
        char* name = parse_name(&cur, ":");
        skip(&cur);
        parse_ws(&cur);
        char* val = parse_name(&cur, "");

        map_insertcpy(&session->parser.req.headers, &name, &val);
      }
    }

    fprintf(stderr, "%s\n", line);

    drop(line);
  }

  //respond to pending requests
  vector_iterator req_iter = vector_iterate(&session->requests);
  while (vector_next(&req_iter)) {
    route(session, req_iter.x);
    req_free(req_iter.x);
  }

  vector_clear(&session->requests);
}

void listen_error(struct evconnlistener* listener, void* ctx) {
  struct event_base *base = evconnlistener_get_base(listener);

  int err = EVUTIL_SOCKET_ERROR();
  event_base_loopexit(base, NULL);

  errx(1, "listener error %i %s", err, evutil_socket_error_to_string(err));
}

/* eventcb for bufferevent */
void eventcb(struct bufferevent *bev, short events, void* ctx) {
  session_t *session = (session_t *)ctx;
  
  if (events & BEV_EVENT_EOF) {
    terminate(session);
  } else if (events & (BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
    respond_error(session, 408, "Timeout / socket error");
  }
}

void acceptcb(struct evconnlistener *listener, int fd, struct sockaddr *addr,
              int addrlen, void *arg) {

  ctx_t *ctx = (ctx_t *)arg;
  session_t *session;

  session = create_session(ctx, fd, addr, addrlen);

  bufferevent_setcb(session->bev, readcb, NULL, eventcb, session);
}

void start_listen(ctx_t* ctx, const char *port) {
  struct addrinfo hints;

  memset(&hints, 0, sizeof(hints));

  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  hints.ai_flags |= AI_ADDRCONFIG;

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

  errx(1, "could not start listener");
}