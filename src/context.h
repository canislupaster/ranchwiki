// Automatically generated header.

#pragma once
#include "hashtable.h"
#include "event2/event.h"
#include "vector.h"
extern char* ERROR_TEMPLATE;
typedef enum {GET, POST} method_t;
typedef char* query[2];
typedef char* header[2];
typedef struct {
  method_t method;
  
  vector_t path; //vector of char* segments
  vector_t query; //vector of char* [2]

  map_t headers; //char* -> char*
} request;
typedef struct {
  struct event_base *evbase;
  map_t templates;
} ctx_t;
typedef struct {
  ctx_t *ctx;
  struct bufferevent *bev; //buffered socket
  char *client_addr;
  
  struct {
    int done; //uninitialized req
    int req_parsed;
    request req;
  } parser;

  vector_t requests;
} session_t;
