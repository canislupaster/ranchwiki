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

void terminate(session_t *session) {
  bufferevent_free(session->bev);

  free(session->client_addr);
  free(session);
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
          while (*line != ' ') {
            line++; //skip ? or &
            char* key = parse_name(&line, "=");
            line++;
            char* val = parse_name(&line, "& ");

            vector_pushcpy(&req->query, &(query){key, val});
          }
          
          break;
        }
      }
    }
  }

  return 1;
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

/* readcb for bufferevent after client connection header was
   checked. */
void readcb(struct bufferevent *bev, void* ctx) {
  session_t *session = (session_t *)ctx;

  struct evbuffer* evbuf = bufferevent_get_input(bev);
  
  char* line;
  
  while ((line = evbuffer_readln(evbuf, NULL, EVBUFFER_EOL_CRLF))) {
    //parse new request or finish old one
    if (session->parser.done) {
      session->parser.done = 0;
      session->parser.req_parsed = 0;

    } else if (strlen(line) == 0 && session->parser.req_parsed) {
      
      vector_pushcpy(&session->requests, &session->parser.req);
      session->parser.done = 1;
      free(line); continue;
    }
    
    //parse request
    if (!session->parser.req_parsed) {
      if (request_parse(line, &session->parser.req)) {
        session->parser.req_parsed = 1;
      } else {
        char* template = map_find(&session->ctx->templates, &ERROR_TEMPLATE);
        respond_html(session, 400, heapstr(template, "Malformed request"));

        terminate(session);
        return;
      }
    }

    free(line);
  }

  //respond to pending requests
  vector_iterator req_iter = vector_iterate(&session->requests);
  while (vector_next(&req_iter)) {
    route(session, req_iter.x);
  }
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
    char* template = map_find(&session->ctx->templates, &ERROR_TEMPLATE);
    respond_html(session, 408, heapstr(template, "Timeout / internal socket error"));
    
    terminate(session);
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