// Automatically generated header.

#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>
#include "hashtable.h"
#include "vector.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdatomic.h>
#include "util.h"
#include "context.h"
#include "web.h"
permissions_t get_perms(session_t* session);
cached* article_current(ctx_t* ctx, vector_t* filepath);
vector_t article_group_list(ctx_t* ctx, filemap_object* article, articledata_t* data, vector_t* item_strs);
vector_t flatten_path(vector_t* path);
vector_t flatten_url(vector_t* path);
vector_t flatten_wikipath(vector_t* path);
int render_article(ctx_t* ctx, char** article, vector_t* refs);
void update_article_refs(ctx_t* ctx, vector_t* flattened, vector_t* add_refs, vector_t* remove_refs, uint64_t idx);
vector_t find_refs(char* content, vector_t* loc);
void refs_free(vector_t* refs);
void rerender_articles(ctx_t* ctx, vector_t* articles, vector_t* from, char* to);
int article_lock_groups(ctx_t* ctx, vector_t* path, vector_t* flattened, vector_t* groups);
void article_group_insert(ctx_t* ctx, vector_t* groups, vector_t* path, vector_t* flattened, uint64_t user_idx, filemap_partial_object* item);
void article_group_remove(ctx_t* ctx, vector_t* groups, vector_t* path, vector_t* flattened, filemap_partial_object* item);
int article_new(ctx_t* ctx, filemap_partial_object* article, article_type ty,
	vector_t* path, vector_t* flattened, uint64_t user_idx, char* html_cache);
int route_article(session_t* session, request* req, filemap_object* obj);
void route(session_t* session, request* req);
