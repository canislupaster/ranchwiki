#include "context.h"
#include "web.h"

#include "util.h"

void route(session_t* session, request* req) {
  if (req->path.length == 0) {
    char* x = "home";
    char* template = *(char**)map_find(&session->ctx->templates, &x);
    
    respond_html(session, 200, heapstr(template, "hi"));
  } else {
    resource* res = map_find(&session->ctx->resources,
      *(char**)vector_get(&req->path, req->path.length-1));
    
    respond(session, 200, res->content, (header[1]){{"Content-Type", res->mime}}, 1);
  }
}