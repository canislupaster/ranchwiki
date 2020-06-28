// Automatically generated header.

#pragma once
#include <time.h>
#include <stdatomic.h>
#include <openssl/evp.h>
#include "vector.h"
#include <stdint.h>
#include <threads.h>
#include <event2/event.h>
#include "hashtable.h"
extern char* ERROR_TEMPLATE;
extern char* GLOBAL_TEMPLATE;
#define CONTENT_MAX 50*1024*1024 //50 mb
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
#include "filemap.h"
typedef struct {
	filemap_partial_object user;
  mtx_t lock; //transaction lock

	//atomic variants of macos time_t and clock_t
	atomic_long last_access;

	atomic_ulong last_get;
	atomic_ulong last_lock;
} user_session;
typedef enum {
  user_name_i = 0,
  user_email_i,
  user_data_i,
  user_bio_i,
  user_length_i
} user_idx;
typedef enum {
  article_data_i = 0,
  article_items_i,
  article_path_i,
  article_contrib_i,
  article_html_i,
  article_length_i
} article_idx;
typedef struct {
  atomic_ulong accessors;
  atomic_ulong accesses;
  time_t first_cache;

  char delete;

  char* data;
  unsigned long len;
} cached;
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

	filemap_ordered_list_t articles_alphabetical;
	filemap_ordered_list_t articles_newest;

  map_t user_sessions;
	map_t user_sessions_by_idx;

  map_t article_lock;

  map_t cached; //maps to file name of cached portion
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
  int closed;
} session_t;
void cleanup_sessions(ctx_t* ctx);
cached* ctx_cache_find(ctx_t* ctx, char* name);
cached* ctx_cache_new(ctx_t* ctx, char* name, char* data, unsigned long len);
void ctx_cache_done(ctx_t* ctx, cached* cache, char* name);
void ctx_cache_remove(ctx_t* ctx, char* name);
cached* ctx_fopen(ctx_t* ctx, char* name);
void lock_article(ctx_t* ctx, char* path, unsigned long sz);
void unlock_article(ctx_t* ctx, char* path, unsigned long sz);
typedef enum __attribute__((__packed__)) {
	perms_none = 0x0,
	perms_create_article = 0x1,
	perms_edit_article = 0x2,
	perms_delete_article = 0x4,
	perms_admin = 0x8
} permissions_t;
#define HASH_LENGTH 32 //256 bit SHA
typedef struct __attribute__((__packed__)) {
	unsigned char password_hash[HASH_LENGTH];
	int32_t salt;

	permissions_t perms;
} userdata_t;
typedef filemap_object filemap_object;
char* user_password_error(char* password);
char* user_error(char* username, char* email);
typedef enum __attribute__((__packed__)) {
	article_text = 0,
	article_group,
	article_img,
  article_dead
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
