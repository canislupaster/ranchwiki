#include "stdlib.h"
#include "stdio.h"

#include "err.h"

#include "event2/event.h"

#include "context.h"
#include "web.h"

#include "util.h"
#include "vector.h"

#include "tinydir.h"
#include "threads.h"

#include "filemap.h"
#include "wiki.h"

const char* TEMPLATE_EXT = ".html";

int main(int argc, char **argv) {
  if (argc < 3) {
    errx(1, "need templates directory and port as arguments");
  }
  
  ctx_t ctx;
  ctx.evbase = event_base_new();

  ctx.templates = map_new();
  map_configure_string_key(&ctx.templates, sizeof(char*));

  ctx.resources = map_new();
  map_configure_string_key(&ctx.resources, sizeof(char*));

  tinydir_dir dir;
  tinydir_open(&dir, argv[1]);

  tinydir_next(&dir); //skip .
  tinydir_next(&dir); //skip ..

  while (dir.has_next) {
    tinydir_file file;
    tinydir_readfile(&dir, &file);

    char* filename = heapcpy(strlen(file.name), file.name);
    char* content = read_file(file.path);

    if (strlen(filename) > strlen(TEMPLATE_EXT) &&
      strcmp(filename+strlen(filename)-strlen(TEMPLATE_EXT), TEMPLATE_EXT)==0) {
      
      memset(filename + strlen(filename) - strlen(TEMPLATE_EXT), 0, strlen(TEMPLATE_EXT));
      
      map_insertcpy(&ctx.templates, &filename, &content);
    } else {
      char* extension = ext(filename);
      char* mime;

      if (strcmp(ext, "css")==0) {
        mime = "text/css";
      } else {
        mime = "application/octet-stream";
      }

      map_insertcpy(&ctx.resources, &filename, &(resource){.content=content, .mime=mime});
    }

    tinydir_next(&dir);
  }

  tinydir_close(&dir);

  start_listen(&ctx, argv[2]);
  event_base_loop(ctx.evbase, 0);
  event_base_free(ctx.evbase);

  return 0;
}