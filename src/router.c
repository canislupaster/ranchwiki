#include "context.h"
#include "web.h"

#include "util.h"

void route(session_t* session, request* req) {
  char** x = &"home";
  char* template = *(char**)map_find(&session->ctx->templates, x);
  respond_html(session, 408, heapstr(template, "hi"));
}