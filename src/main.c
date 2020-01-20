#include "stdlib.h"
#include "stdio.h"

#include "err.h"

#include "event2/event.h"

#include "context.h"
#include "web.h"

#include "util.h"

#include "tinydir.h"

#include "filemap.h"

int main() {
  filemap_t fmap;
  if (!filemap_new(&fmap, "./test-fmap.index", "./test-fmap.data")) errx(1, "file map not initialized");

  int x = filemap_insert(&fmap, "hello", "world", sizeof("hello"), sizeof("world"));
  x = filemap_insert(&fmap, "hello", "world2", sizeof("hello"), sizeof("world2"));

  filemap_mem_result res = filemap_findcpy(&fmap, "hello", sizeof("hello"));

  filemap_remove(&fmap, "hello", sizeof("hello"));

  x = filemap_insert(&fmap, "hm", "m", sizeof("hm"), sizeof("m"));
  
  filemap_free(&fmap);
}

// int main(int argc, char **argv) {
//   if (argc < 3) {
//     errx(1, "need templates directory and port as arguments");
//   }
  
//   ctx_t ctx;
//   ctx.evbase = event_base_new();

//   ctx.templates = map_new();
//   map_configure_string_key(&ctx.templates, sizeof(char*));

//   tinydir_dir dir;
//   tinydir_open(&dir, argv[1]);

//   tinydir_next(&dir); //skip .
//   tinydir_next(&dir); //skip ..

//   while (dir.has_next) {
//     tinydir_file file;
//     tinydir_readfile(&dir, &file);

//     char* filename = heapcpy(strlen(file.name), file.name);
//     char* content = read_file(file.path);

//     memset(filename + strlen(filename) - strlen(".html"), 0, strlen(".html"));

//     map_insertcpy(&ctx.templates, &filename, &content);

//     tinydir_next(&dir);
//   }

//   tinydir_close(&dir);

//   start_listen(&ctx, argv[2]);
//   event_base_loop(ctx.evbase, 0);
//   event_base_free(ctx.evbase);

//   return 0;
// }