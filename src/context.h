// Automatically generated header.

#pragma once
#include "event2/event.h"
#include "hashtable.h"
#include "vector.h"
extern char* ERROR_TEMPLATE;
#define CONTENT_MAX 500
typedef enum {GET, POST} method_t;
typedef enum {url_formdata} content_type;
typedef char* query[2];
typedef char* header[2];
typedef struct {
  method_t method;
  
  vector_t path; //vector of char* segments
  vector_t query; //vector of char* [2]

  map_t headers; //char* -> char*

  unsigned long content_length;
  content_type ctype;
  char* content;
} request;
typedef struct {
  char* mime;
  char* content;
} resource;
typedef struct {
  struct event_base *evbase;
  map_t templates;
  map_t resources; //without slashes
} ctx_t;
typedef struct {
  ctx_t *ctx;
  struct bufferevent *bev; //buffered socket
  char *client_addr;
  
  struct {
    char done; //uninitialized req
    char req_parsed; //request line parsed

    char has_content;
    char content_parsing; //set after first newline
    request req;
  } parser;

  vector_t requests;
} session_t;
