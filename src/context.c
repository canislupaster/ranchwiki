#include <stdint.h>
#include <threads.h>
#include <time.h>
#include <stdatomic.h>

#include <event2/event.h>

#include <openssl/evp.h>

#include "hashtable.h"
#include "locktable.h"
#include "vector.h"
#include "filemap.h"

const char* ERROR_TEMPLATE = "error"; //name of error template
const char* GLOBAL_TEMPLATE = "global"; //name of global template
#define CONTENT_MAX 50*1024*1024 //50 mb
#define SESSION_TIMEOUT 3600*24*60 //60 days
#define CLEANUP_INTERVAL 24*3600
#define WCACHE_INTERVAL 5*60
#define CACHE_EXPIRY 3600 //seconds after which to expire cache's weighting 
#define CACHE_MIN 1000 //accesses per hour to qualify in cache
#define AUTH_KEYSZ 128

#define PAGE_SIZE 12

#define WORD_MIN 1
#define WORD_MAX 16
#define WORD_LIMIT 64 //articles per word before overflow
#define WORD_LOCKS 32
#define QUERY_MAX 32

#define SECRET_PATH "secret"

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

// data, referenced by/items, path, contributors, html cache
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

void uses_free(user_session* uses) {
	mtx_destroy(&uses->lock);
	drop(uses);
}

user_session* uses_from_idx(ctx_t* ctx, uint64_t idx, char write) {
	if (write) rwlock_write(ctx->user_sessions_by_idx.lock);
	else rwlock_read(ctx->user_sessions_by_idx.lock);
	
	char* ses_key = map_find_unlocked(&ctx->user_sessions_by_idx, &idx);
	
	if (!write) rwlock_unread(ctx->user_sessions_by_idx.lock);
	
	if (ses_key) {
		return *(user_session**)map_find(&ctx->user_sessions, &(map_sized_t){.bin=ses_key, .size=AUTH_KEYSZ});
	} else {
		return NULL;
	}
}

void cleanup_sessions(ctx_t* ctx) {
	time_t t = time(NULL);

	map_iterator iter = map_iterate(&ctx->user_sessions);
	while (map_next(&iter)) {
		user_session* user_ses = iter.x;
		time_t last_access = (time_t)atomic_load(&user_ses->last_access);
		
		if (difftime(last_access, t) > SESSION_TIMEOUT) {
			mtx_lock(&user_ses->lock);
			uses_free(user_ses);
			map_remove(&ctx->user_sessions_by_idx, &user_ses->user.index);
			map_next_delete(&iter);
		}
	}
}

cached* ctx_cache_find(ctx_t* ctx, char* name) {
	rwlock_read(ctx->cached.lock);
	cached* cache = map_find_unlocked(&ctx->cached, &name);
	
	if (cache) atomic_fetch_add(&cache->accessors, 1);
	rwlock_unread(ctx->cached.lock);
	
	return cache;
}

cached* ctx_cache_new(ctx_t* ctx, char* name, char* data, unsigned long len) {
	cached new = {.data=data, .len=len};
	new.first_cache = time(NULL);
	atomic_init(&new.accesses, 0);
	atomic_init(&new.accessors, 1);

	map_insert_result insres = map_insertcpy_noexist(&ctx->cached, &name, &new);

	//in case of high traffic, use map lock as synchronization
	//hah! high traffic!??! never lmao
	if (insres.exists) {
		drop(data);
		drop(name);
	}

	return insres.val;
	//oh boy i am much pious with my shitey sitey
}

void ctx_cache_done(ctx_t* ctx, cached* cache, char* name) {
	time_t t = time(NULL);

	unsigned long accs = atomic_fetch_add(&cache->accesses, 1)+1;

	int delete = accs*3600/difftime(t, cache->first_cache) < CACHE_MIN;
	if (delete) rwlock_write(ctx->cached.lock);
	
	unsigned long acc = atomic_fetch_sub(&cache->accessors, 1);

	//small race...
	if (acc == 1 && difftime(t, cache->first_cache) >= CACHE_EXPIRY) {
		cache->first_cache = t;
		atomic_store(&cache->accesses, 1);

	} else if (delete && acc==1) {
		drop(cache->data);
		map_remove_unlocked(&ctx->cached, &name);
	}
	
	if (delete) rwlock_unwrite(ctx->cached.lock);
}

void ctx_cache_remove(ctx_t* ctx, char* name) {
	cached* cache = map_find(&ctx->cached, &name);
	if (cache) {
		atomic_fetch_add(&cache->accessors, 1);

		rwlock_write(ctx->cached.lock);
		unsigned long acc = atomic_fetch_sub(&cache->accessors, 1);

		if (acc == 1) {
			drop(cache->data);
			map_remove_unlocked(&ctx->cached, &name);
		}
		
		rwlock_unwrite(ctx->cached.lock);
	}
}

cached* ctx_fopen(ctx_t* ctx, char* name) {
	cached* cache = ctx_cache_find(ctx, name);

	if (!cache) {
		FILE* f = fopen(name, "r");
		if (!f) return NULL;
		
		fseek(f, 0, SEEK_END);
		unsigned long len = ftell(f);

		char* data = heap(len);
		fseek(f, 0, SEEK_SET);
		fread(data, len, 1, f);

		fclose(f);

		cache = ctx_cache_new(ctx, heapcpystr(name), data, len);
	}

	return cache;
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
	
	if (!m) return; //insertion synchronization error (find-then-insert) or a mysterious bug
	
	mtx_unlock(m);

	if (m && mtx_trylock(m) == thrd_success) {
		mtx_destroy(m);

		map_remove(&ctx->article_lock, &key);
	}
}

#define PERMS_CREATE 1
#define PERMS_EDIT 2
#define PERMS_DELETE 3
#define PERMS_ADMIN 4
#define PERMS_SECRET 5

#define HASH_LENGTH 32 //256 bit SHA
#define MIN_USERNAME 1
#define MAX_USERNAME 20
#define MIN_PASSWORD 4

typedef struct __attribute__((__packed__)) {
	unsigned char password_hash[HASH_LENGTH];
	int32_t salt;

	unsigned char perms;
} userdata_t;

typedef filemap_object filemap_object;

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

int setrank(ctx_t* ctx, filemap_partial_object* list_user, filemap_object* user, unsigned char maxperm, unsigned char setperm) {
	user_session* ses = uses_from_idx(ctx, list_user->index, 1);
	if (ses) mtx_lock(&ses->lock);

	*user = filemap_cpy(&ctx->user_fmap, list_user);

	userdata_t* data = (userdata_t*)user->fields[user_data_i];
	
	if (data->perms >= maxperm) {
		rwlock_unwrite(ctx->user_sessions_by_idx.lock);
		if (ses) mtx_unlock(&ses->lock);
		filemap_object_free(&ctx->user_fmap, user);
		
		return 0;
	}
	
	data->perms = setperm;

	filemap_object new_u = filemap_push(&ctx->user_fmap, user->fields, user->lengths);
	filemap_list_update(&ctx->user_id, list_user, &new_u);
	filemap_delete_object(&ctx->user_fmap, user);

	if (ses) {
		ses->user = filemap_partialize(list_user, &new_u);
		mtx_unlock(&ses->lock);
	}
	
	rwlock_unwrite(ctx->user_sessions_by_idx.lock);

	return 1;
}

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

