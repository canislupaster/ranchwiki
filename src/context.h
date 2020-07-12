// Automatically generated header.

#pragma once
#include <stdint.h>
#include <threads.h>
#include <time.h>
#include <stdatomic.h>
#include <event2/event.h>
#include <openssl/evp.h>
#include "hashtable.h"
#include "locktable.h"
#include "vector.h"
extern char* ERROR_TEMPLATE;
extern char* GLOBAL_TEMPLATE;
#define CONTENT_MAX 50*1024*1024 //50 mb
#define CLEANUP_INTERVAL 24*3600
#define WCACHE_INTERVAL 5*60
#define AUTH_KEYSZ 128
#define PAGE_SIZE 12
#define WORD_MIN 1
#define WORD_MAX 16
#define WORD_LIMIT 64 //articles per word before overflow
#define WORD_LOCKS 32
#define QUERY_MAX 32
typedef enum {GET, POST} method_t;
typedef enum {url_formdata, multipart_formdata} content_type;
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
	vector_t cookies; //parsed cookie header
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
	atomic_ulong last_access;
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
	unsigned char score;
	char* word;
	uint64_t pos;
} search_token;
typedef struct __attribute__((__packed__)) {
	unsigned char score;
	uint64_t pos;
	uint64_t article;
} article_tok;
typedef struct __attribute__((__packed__)) {
	article_tok tok[WORD_LIMIT];
} word_index;
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

	locktable_t word_lock;
	filemap_index_t words;
	filemap_t wordi_fmap;

	map_t wordi_cache;

  map_t article_lock;

  map_t cached; //maps to file name of cached portion
} ctx_t;
typedef struct {
  ctx_t *ctx;
  struct bufferevent *bev; //buffered socket

	user_session* user_ses;
	char* auth_tok; //set by router for update
  
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
void uses_free(user_session* uses);
user_session* uses_from_idx(ctx_t* ctx, uint64_t idx);
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
	
	uint64_t edit_time;
} articledata_t;
