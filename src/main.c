#include <assert.h>
#include <err.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#include "context.h"
#include "filemap.h"
#include "hashtable.h"
#include "locktable.h"
#include "threads.h"
#include "tinydir.h"
#include "util.h"
#include "vector.h"
#include "web.h"
#include "router.h"
#include "wiki.h"

ctx_t* global_ctx;

const char* TEMPLATE_EXT = ".html";

void save_ctx(ctx_t* ctx) {
  printf("Saving...\n");
  filemap_free(&ctx->user_fmap);
  filemap_list_free(&ctx->user_id);
  filemap_index_free(&ctx->user_by_name);
  filemap_index_free(&ctx->user_by_email);

  filemap_free(&ctx->article_fmap);
  filemap_list_free(&ctx->article_id);
  filemap_index_free(&ctx->article_by_name);

	filemap_ordered_free(&ctx->articles_newest);
	filemap_ordered_free(&ctx->articles_alphabetical);

	filemap_free(&ctx->wordi_fmap);
	filemap_index_free(&ctx->words);
}

void cleanup_callback(int fd, short what, void* arg) {
  ctx_t* ctx = arg;
  cleanup_sessions(ctx);
}

void wcache_callback(int fd, short what, void* arg) {
  ctx_t* ctx = arg;

	filemap_wcached(&ctx->user_fmap);
	filemap_list_wcached(&ctx->user_id);
	filemap_index_wcached(&ctx->user_by_email);
	filemap_index_wcached(&ctx->user_by_name);

	filemap_wcached(&ctx->article_fmap);
	filemap_list_wcached(&ctx->article_id);
	filemap_index_wcached(&ctx->article_by_name);
}

void interrupt_callback(int signal, short events, void* arg) {
  ctx_t* ctx = arg;
  event_base_loopbreak(ctx->evbase);
}

void sighandler(int sig, siginfo_t* info, void* arg) {
	save_ctx(global_ctx);
}

int util_main(void* udata) {
	printf("util started\n");

  ctx_t* ctx = udata;

  while (1) {
		char* in = NULL;
		size_t sz = 0;
		getline(&in, &sz, stdin);

    vector_t arg = vector_split_str(in, " \n");
		if (arg.length == 0) {
			drop(in);
			vector_free(&arg);
			continue;
		}
		
		if (strlen(vector_getstr(&arg, arg.length-1))==0) vector_pop(&arg);

		if (strcmp(vector_getstr(&arg, 0), "rank")==0 && arg.length == 3) {
      char* name = vector_getstr(&arg, 1);

      filemap_partial_object name_user_ref = filemap_find(&ctx->user_by_name, name, strlen(name)+1);
			if (!name_user_ref.exists) {
				perror("User not found");
				drop(in);
				vector_free(&arg);
				continue;
			}

      filemap_partial_object list_user = filemap_deref(&ctx->user_id, &name_user_ref);

      filemap_object user = filemap_cpy(&ctx->user_fmap, &list_user);

      userdata_t* data = (userdata_t*)user.fields[user_data_i];
      data->perms = (char)strtol(vector_getstr(&arg, 2), NULL, 0);
			
			user_session* ses = uses_from_idx(ctx, list_user.index);
      if (ses) mtx_lock(&ses->lock);

      filemap_object new_u = filemap_push(&ctx->user_fmap, user.fields, user.lengths);
      filemap_list_update(&ctx->user_id, &list_user, &new_u);
      filemap_delete_object(&ctx->user_fmap, &user);

      if (ses) {
        ses->user = filemap_partialize(&list_user, &new_u);
        mtx_unlock(&ses->lock);
      }

      filemap_object_free(&ctx->user_fmap, &user);
      printf("Permissions updated\n");
			
		} else if (strcmp(vector_getstr(&arg, 0), "diff")==0 && arg.length==3) {
			char* path_str = vector_getstr(&arg, 1);
			int maxd = (int)strtol(vector_getstr(&arg, 2), NULL, 10);
			
			vector_t path = vector_new(sizeof(char*));
			if (!parse_wiki_path(path_str, &path) || maxd==0) {
				fprintf(stderr, "couldnt parse wiki path / max diffs is zero");
				
				vector_free_strings(&path);
				vector_free(&arg);
				drop(in);
				continue;
			}
			
			vector_t wpath = flatten_wikipath(&path);
			vector_free_strings(&path);
			
			text_t txt = txt_new(wpath.data);
			read_txt(&txt, 0, maxd);
			
			if (!txt.current) {
				fprintf(stderr, "text does not exist");
				
				vector_free(&wpath);
				vector_free(&arg);
				drop(in);
				continue;
			}

			vector_t segs = display_diffs(&txt);
			vector_iterator iter = vector_iterate(&segs);
			while (vector_next(&iter)) {
				dseg* seg = iter.x;
				char* start = seg->str;

				diff_t* diff = vector_get(&txt.diffs, seg->diff);

				filemap_partial_object author_list = filemap_get_idx(&ctx->user_id, diff->author);
				filemap_field uname = filemap_cpyfield(&ctx->user_fmap, &author_list, user_name_i);

				if (uname.exists) printf("\ndiff #%lu made by %s\n", seg->diff, uname.val.data);
				if (uname.exists) vector_free(&uname.val);

				while (seg->str-start < seg->len) {
					if (skip_newline(&seg->str)) printf("\n");
					char* linestart = seg->str;
					
					while (seg->str-start < seg->len && *seg->str != '\r' && *seg->str != '\n') seg->str++;

					switch (seg->ty) {
						case dseg_add: printf("+"); break;
						case dseg_del: printf("-"); break;
						case dseg_current: printf("="); break;
					}
					
					printf(" | %.*s", (int)(seg->str-linestart), linestart);
				}
			}
			
			printf("\n");
			
			txt_free(&txt);
			vector_free(&segs);
			vector_free(&wpath);
			
		} else if (strcmp(vector_getstr(&arg, 0), "quit")==0) {
      event_base_loopbreak(ctx->evbase);
    } else {
      fprintf(stderr, "Action %s not found\n", vector_getstr(&arg, 0));
    }

		vector_free(&arg);
    drop(in);
  }

  return 0;
}

int main(int argc, char** argv) {
  // filemap_t fmap = filemap_new("./test-fmap", 2, 1);
  // filemap_ordered_list_t list =
  // filemap_ordered_list_new("./test-fmap-ordered", 2, 1);
  //
  // filemap_object obj = filemap_push(&fmap, (char*[2]){"hello", "world"},
  // (uint64_t[2]){strlen("hello"), strlen("world")}); filemap_ord_partial_object
  // partial = filemap_ordered_insert(&list, 2, &obj);
  //
  // obj = filemap_push(&fmap, (char*[2]){"hello2", "world2"},
  // (uint64_t[2]){strlen("hellox"), strlen("worldx")});
  // filemap_ordered_insert(&list, 1, &obj);
  //
  // obj = filemap_push(&fmap, (char*[2]){"hello3", "world3"},
  // (uint64_t[2]){strlen("hellox"), strlen("worldx")});
  // filemap_ordered_insert(&list, 3, &obj);
  //
  // vector_t vec = filemap_ordered_page(&list, 0, 1);
  // vector_t vec2 = filemap_ordered_page(&list, 1, 2);
  //
  // vector_iterator iter = vector_iterate(&vec);
  // while (vector_next(&iter)) {
  //	filemap_ord_partial_object* partial = iter.x;
  //	printf("%llu\n", partial->partial.index);
  //}
  //
  // printf("p. 2\n");
  //
  // iter = vector_iterate(&vec2);
  // while (vector_next(&iter)) {
  //	filemap_ord_partial_object* partial = iter.x;
  //	printf("%llu\n", partial->partial.index);
  //}
  //
  // filemap_ordered_remove(&list, &partial);
  //
  // filemap_free(&fmap);
  // filemap_ordered_free(&list);
  //
  // return 0;

	//reset buffering for use with pipes
	setvbuf(stdout, NULL, _IOLBF, PIPE_BUF);
	setvbuf(stderr, NULL, _IOLBF, PIPE_BUF); //needed for formatting for some reason, lest program crashes?? buf needs to match pipe buf??

	printf("starting ranch...\n");

  if (argc < 3) {
    errx(1, "need templates directory and port as arguments\n");
  }

  evthread_use_pthreads();

  ctx_t ctx;
  ctx.evbase = event_base_new();

	global_ctx = &ctx;

  struct sigaction sact;
	sact.sa_flags = SA_SIGINFO;
  sact.sa_sigaction = &sighandler;
	
  sigaction(SIGSEGV, &sact, NULL);
	sigaction(SIGABRT, &sact, NULL);

  ctx.digest_ctx = EVP_MD_CTX_create();

  ctx.templates = map_new();
  map_configure_string_key(&ctx.templates, sizeof(template_t));

  ctx.resources = map_new();
  map_configure_string_key(&ctx.resources, sizeof(resource));

  ctx.cached = map_new();
	ctx.cached.free = free_string;
	
  map_distribute(&ctx.cached);
  map_configure_string_key(&ctx.cached, sizeof(cached));

  // user, email, data, bio
  ctx.user_id = filemap_list_new("./user_id", 0);

  ctx.user_fmap = filemap_new("./users", user_length_i, 0);
  ctx.user_fmap.alias = &ctx.user_id;

  ctx.user_by_name = filemap_index_new(&ctx.user_fmap, "./users_by_name", user_name_i, 0);
  ctx.user_by_email =
      filemap_index_new(&ctx.user_fmap, "./users_by_email", user_email_i, 0);

  ctx.user_sessions = map_new();
  ctx.user_sessions.free = free_string;

  map_distribute(&ctx.user_sessions);
  map_configure_sized_key(&ctx.user_sessions, sizeof(user_session*));

  ctx.user_sessions_by_idx = map_new();
  map_distribute(&ctx.user_sessions_by_idx);
  map_configure_uint64_key(&ctx.user_sessions_by_idx, AUTH_KEYSZ); //key for user_sessions

  ctx.article_lock = map_new();
  ctx.article_lock.free = free_sized;

  map_distribute(&ctx.article_lock);
  map_configure_sized_key(&ctx.article_lock, sizeof(mtx_t));

  ctx.article_id = filemap_list_new("./article_id", 0);  // avoid update hell

  ctx.article_fmap = filemap_new("./articles", article_length_i, 0);
  ctx.article_fmap.alias = &ctx.article_id;

  ctx.article_by_name =
      filemap_index_new(&ctx.article_fmap, "./articles_by_name", article_path_i, 0);

	ctx.articles_newest = filemap_ordered_list_new("./article_new", PAGE_SIZE, 0);
	ctx.articles_alphabetical = filemap_ordered_list_new("./article_abc", PAGE_SIZE, 0);

	ctx.wordi_fmap = filemap_new("./wordi", 2, 0);
	ctx.words = filemap_index_new(&ctx.wordi_fmap, "./word", 0, 0);

	ctx.word_lock = locktable_new(WORD_LOCKS);
	ctx.wordi_cache = map_new(sizeof(filemap_partial_object));
	map_configure_string_key(&ctx.wordi_cache, sizeof(filemap_partial_object));

  tinydir_dir dir;
  tinydir_open(&dir, argv[1]);

  for (;dir.has_next; tinydir_next(&dir)) {
    tinydir_file file;
    tinydir_readfile(&dir, &file);

		if (strcmp(file.name, ".")==0 || strcmp(file.name, "..")==0 || strcmp(file.name, "./")==0)
			continue;

    char* filename = heapcpystr(file.name);

		//copypaste until C has tuples
    FILE* f = fopen(file.path, "rb");
		
    fseek(f, 0, SEEK_END);
    unsigned long len = ftell(f);

    char* data = heap(len+1);
    fseek(f, 0, SEEK_SET);
    fread(data, len, 1, f);
		
		data[len] = 0;

    fclose(f);

    if (strlen(filename) > strlen(TEMPLATE_EXT) &&
        strcmp(filename + strlen(filename) - strlen(TEMPLATE_EXT),
               TEMPLATE_EXT) == 0) {
      memset(filename + strlen(filename) - strlen(TEMPLATE_EXT), 0,
             strlen(TEMPLATE_EXT));

      if (strcmp(filename, GLOBAL_TEMPLATE) == 0) {
        ctx.global = data;
      } else {
        template_t template = template_new(data);
        map_insertcpy(&ctx.templates, &filename, &template);
        drop(data);
      }
    } else {
      char* extension = ext(filename);
      char* mime;

      if (strcmp(extension, ".css") == 0) {
        mime = "text/css";
			} else if (strcmp(extension, ".png") == 0) {
				mime = "image/png";
      } else if (strcmp(extension, ".ico") == 0) {
				mime = "image/x-icon";
      } else {
        mime = "application/octet-stream";
      }

      map_insertcpy(
          &ctx.resources, &filename,
          &(resource){
              .content = data, .len = len, .mime = mime});
    }
  }

  tinydir_close(&dir);

	printf("starting util...\n");

  thrd_t util;
  thrd_create(&util, util_main, &ctx);

  start_listen(&ctx, argv[2]);

  struct event* cleanup = event_new(ctx.evbase, -1, EV_PERSIST, cleanup_callback, &ctx);
	event_add(cleanup, &(struct timeval){.tv_sec=CLEANUP_INTERVAL});

  struct event* wcache = event_new(ctx.evbase, -1, EV_PERSIST, wcache_callback, &ctx);
	event_add(wcache, &(struct timeval){.tv_sec=WCACHE_INTERVAL});

  struct event* interrupt =
      evsignal_new(ctx.evbase, SIGINT, interrupt_callback, &ctx);
  event_add(interrupt, NULL);

#if BUILD_DEBUG
  memcheck_init();
#endif

  event_base_loop(ctx.evbase, 0);
  event_base_free(ctx.evbase);

  EVP_MD_CTX_destroy(ctx.digest_ctx);
  EVP_cleanup();

	save_ctx(&ctx);

#if BUILD_DEBUG
  memcheck();
#endif

  return 0;
}
