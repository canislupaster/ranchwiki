#include <stdint.h>
#include <threads.h>
#include <time.h>
#include <stdatomic.h>

#include <event2/event.h>

#include <openssl/evp.h>

#include "hashtable.h"
#include "vector.h"
#include "filemap.h"

const char* ERROR_TEMPLATE = "error"; //name of error template
const char* GLOBAL_TEMPLATE = "global"; //name of global template
#define CONTENT_MAX 500
#define SESSION_TIMEOUT 3600*24*60 //60 days
#define CLEANUP_INTERVAL 24*3600

typedef enum {GET, POST} method_t;

typedef enum {url_formdata, multipart_formdata} content_type;

typedef char* query[2];
typedef char* header[2];

typedef struct {
	char* name;
	char* mime;

	char* content;
	unsigned long len;
} multipart_data;

typedef struct {
  method_t method;
  
  vector_t path; //vector of char* segments
  vector_t query; //vector of char* [2], if content type is url formdata
	vector_t files; //vector of multipart_file, if content type is multipart formdata

  map_t headers; //char* -> char*

  unsigned long content_length;
  content_type ctype;
  char* content;
} request;

typedef struct {
  char* mime;
  char* content;
	unsigned long len;
} resource;

typedef struct {
	filemap_partial_object user;
  mtx_t lock; //transaction lock

	//atomic variants of macos time_t and clock_t
	atomic_long last_access;

	atomic_ulong last_get;
	atomic_ulong last_lock;
} user_session;

typedef struct {
  struct event_base *evbase;

  char* global;
  map_t templates;
  map_t resources; //without slashes

	EVP_MD_CTX* digest_ctx;

  filemap_t user_fmap;
  filemap_list_t user_id;

  filemap_index_t user_by_name;
	filemap_index_t user_by_email;

	filemap_t article_fmap;
	filemap_list_t article_id;

	filemap_index_t article_by_name;
	filemap_index_t article_by_user;

	filemap_ordered_list_t articles_alphabetical;
	filemap_ordered_list_t articles_newest;

  map_t user_sessions;
	map_t user_sessions_by_idx;

  map_t article_lock;
} ctx_t;

typedef struct {
  ctx_t *ctx;
  struct bufferevent *bev; //buffered socket
  char *client_addr;

	user_session* user_ses;
  
  struct {
    char done; //uninitialized req
    char req_parsed; //request line parsed

    char has_content;
    char content_parsing; //set after first newline

		char* multipart_boundary;

    request req;
  } parser;

  vector_t requests;
} session_t;

void cleanup_sessions(ctx_t* ctx) {
	time_t t = time(NULL);

	map_iterator iter = map_iterate(&ctx->user_sessions);
	while (map_next(&iter)) {
		user_session* user_ses = iter.x;
		time_t last_access = atomic_load(&user_ses->last_access);
		if (user_ses->last_access-t > SESSION_TIMEOUT) {
      mtx_lock(&user_ses->lock);
      mtx_destroy(&user_ses->lock);

      map_next_delete(&iter);
     }
	}
}

//lock by key, to ensure it is one to one with list index
void lock_article(ctx_t* ctx, char* path, unsigned long sz) {
  //read lock
  map_sized_t key = {.bin=path, .size=sz};
  mtx_t* m = map_find(&ctx->article_lock, &key);

  if (!m) {
    //copy path and write lock
    key.bin = heapcpy(sz, path);

    m = map_insert(&ctx->article_lock, &key).val;
    mtx_init(m, mtx_plain);
  }
  
  mtx_lock(m);
}

void unlock_article(ctx_t* ctx, char* path, unsigned long sz) {
  map_sized_t key = {.bin=path, .size=sz};
  mtx_t* m = map_find(&ctx->article_lock, &key);

  if (m && mtx_trylock(m) == thrd_success) {
    mtx_destroy(m);

    map_remove(&ctx->article_lock, &key);
  }
}

typedef enum: uint8_t {
	perms_none = 0x0,
	perms_create_article = 0x1,
	perms_edit_article = 0x2,
	perms_delete_article = 0x4,
	perms_admin = 0x8
} permissions_t;

#define HASH_LENGTH 32 //256 bit SHA
#define MIN_USERNAME 1
#define MAX_USERNAME 20
#define MIN_PASSWORD 4

typedef struct __attribute__((__packed__)) {
	unsigned char password_hash[HASH_LENGTH];
	int32_t salt;

	permissions_t perms;
} userdata_t;

typedef filemap_object user_t;

char* user_password_error(char* password) {
	if (strlen(password) < MIN_PASSWORD)
		return "Password have at least four characters";
	else return NULL;
}

char* user_error(char* username, char* email) {
	if (strlen(username) < MIN_USERNAME || strlen(username) > MAX_USERNAME)
		return "Username needs to be at least 1 char and 20 max";

	char* at = strchr(email, '@');

	if (at && at != email) {
		char* dot = strchr(at+1, '.');
		if (!dot || dot == at+1) {
			return "Invalid email: missing dot after at";
		}
	} else {
		return "Invalid email: missing at after beginning of email";
	}

	return NULL;
}

user_t update_user(ctx_t* ctx, filemap_partial_object* obj, user_t *user, char **fields, uint64_t *lengths) {
  filemap_delete_object(&ctx->user_fmap, user);
  user_t new_user = filemap_push(&ctx->user_fmap, fields, lengths);

  filemap_list_update(&ctx->user_id, obj, &new_user);

  return new_user;
}

user_t update_user_session(session_t *session, user_t *user,
                           char **fields, uint64_t *lengths) {
  user_t new_user = update_user(session->ctx, &session->user_ses->user, user, fields, lengths);
	session->user_ses->user = filemap_partialize(&session->user_ses->user, &new_user);

	return new_user;
}

typedef enum: uint8_t {
	article_text = 0,
	article_group,
	article_img
} article_type;

typedef struct __attribute__((__packed__)) {
	article_type ty;
	uint32_t path_length;

  uint64_t contributors;

	union {
		uint64_t items;
		uint64_t referenced_by;
	};
} articledata_t;

