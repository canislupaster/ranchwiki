#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>

#include "context.h"
#include "filemap.h"
#include "hashtable.h"
#include "util.h"
#include "vector.h"
#include "web.h"
#include "wiki.h"

// boilerplate is intentional btw

//:)

permissions_t get_perms(session_t* session) {
  filemap_field udata =
      filemap_cpyfield(&session->ctx->user_fmap, &session->user_ses->user, user_data_i);

  if (!udata.exists) {
    return perms_none;
  } else {
    permissions_t perms = ((userdata_t*)udata.val.data)->perms;
    vector_free(&udata.val);
    return perms;
  }
}

cached* article_current(ctx_t* ctx, vector_t* filepath) {
	cached* current_cache = ctx_cache_find(ctx, filepath->data);
	if (!current_cache) {
		text_t txt = txt_new(filepath->data);
		read_txt(&txt, 0);
		current_cache = ctx_cache_new(ctx, filepath->data, heapcpystr(txt.current), strlen(txt.current));

		txt_free(&txt);
	}

	return current_cache;
}

vector_t article_group_list(ctx_t* ctx, filemap_object* article, articledata_t* data, vector_t* item_strs) {
  vector_t items = {.data = article->fields[article_items_i],
    .size = sizeof(uint64_t),
    .length = data->items};

  vector_t items_arg = vector_new(sizeof(template_args));

  vector_iterator iter = vector_iterate(&items);
  while (vector_next(&iter)) {
    filemap_partial_object list_item = filemap_get_idx(&ctx->article_id, *(uint64_t*)iter.x);
    if (!list_item.exists) continue;

    articledata_t* item_data =
      (articledata_t*)filemap_cpyfield(&ctx->article_fmap, &list_item, article_data_i).val.data;

		if (item_data->ty==article_dead) {
			drop(item_data);
			continue;
		}
		
    int is_group = item_data->ty==article_group;

    vector_t item_pathdata = filemap_cpyfield(&ctx->article_fmap, &list_item, article_path_i).val;

    vector_t item_path = vector_from_strings(item_pathdata.data, item_data->path_length);
		char* item_end = vector_getstr(&item_path, item_path.length-1);

		unsigned long len = strlen(item_end);
    char* end = heap(is_group ? len+2 : len+1);
		memcpy(end, item_end, len+1);
		
		if (is_group) {
			end[len] = '/';
			end[len+1] = 0;
		}

    vector_pushcpy(&items_arg, &(template_args){.cond_args=heapcpy(sizeof(int), &is_group),
      .sub_args=heapcpy(sizeof(char*[1]), (char*[1]){end})});
    vector_pushcpy(item_strs, &end);

    drop(item_data);
    vector_free(&item_pathdata);
    vector_free(&item_path);
  }

  return items_arg;
}

vector_t flatten_path(vector_t* path) {
	vector_t flattened = vector_new(1);
	vector_flatten_strings(path, &flattened, "\0", 1);
	vector_pushcpy(&flattened, "\0");
	return flattened;
}

vector_t flatten_url(vector_t* path) {
  vector_t flattened = vector_new(1);
  vector_flatten_strings(path, &flattened, "/", 1);
  vector_pushcpy(&flattened, "\0");
  return flattened;
}

vector_t flatten_wikipath(vector_t* path) {
  vector_t flattened = vector_new(1);
	vector_stockstr(&flattened, DATA_PATH);
  vector_flatten_strings(path, &flattened, "/", 1);
  vector_pushcpy(&flattened, "\0");
  return flattened;
}

int render_article(ctx_t* ctx, char** article, vector_t* refs) {
	escape_html(article);
	vector_t vec = vector_from_string(*article);

	//bbbbbbb
	int newline=1;
	int escaped=0;
	int bold=0;
	int heading=0;

	vector_insertstr(&vec, 0, "<p>");

	vector_iterator iter = vector_iterate(&vec);
	iter.i = strlen("<p>");
	
	while (vector_next(&iter)) {
		if (escaped) {
			escaped = 0;
			continue;
		}

		char x = *(char*)iter.x;

		if (x == '\n' || x == '\r' || x==0) {
			if (heading) {
				heading = 0;
				vector_insertstr(&vec, iter.i-1, "</h2>");
				iter.i += strlen("</h2>")-1;
				continue;
			}

			if (x==0) {
				vector_insert_manycpy(&vec, iter.i-1, strlen("</p>")+1, "</p>");
				break;
			} else if (!newline) {
				vector_insertstr(&vec, iter.i-1, "</p><p>");
				iter.i += strlen("</p><p>")-1;
			}

			newline = 1;
			continue;

		} else if (x == '#') {
			if (newline) {
				heading=1;
				newline=0;

				vector_remove(&vec, iter.i-1);
				vector_insertstr(&vec, iter.i-1, "<h2>");
				iter.i += strlen("<h2>")-1;
			}

			continue;

		} else {
			newline=0;
		}

		switch (x) {
			case '\\': escaped = 1; break;
			case '*': {
				if (bold) {
					vector_insertstr(&vec, iter.i, "</b>");
				} else {
					vector_insertstr(&vec, iter.i-1, "<b>");
					iter.i += strlen("<b>") + 1;
				}

				bold=!bold;
				break;
			}

			case '[': {
				unsigned long remove_from = iter.i-1;

				vector_next(&iter);
				int wiki = *(char*)iter.x == '!';
				if (wiki) vector_next(&iter);

				char* start = iter.x;
				//wiki link

				while (*(char*)iter.x != ']' && vector_next(&iter));
				char* end = iter.x;

				if (end == start) {
					vector_free(&vec);
					return start-vec.data+1;
				}
				
				char* url = heapcpy(end-start+1, start);
				url[end-start] = 0;
				
				char* new_url;

				if (wiki) {
					vector_t w_path = vector_new(sizeof(char*));
					if (!parse_wiki_path(url, &w_path)) {
						vector_free_strings(&w_path);
						return start-vec.data+1;
					}

					if (refs) vector_pushcpy(refs, &w_path);
					
					vector_t outurl = flatten_url(&w_path);
					vector_t flattened = flatten_path(&w_path);

					filemap_partial_object obj = filemap_find(&ctx->article_by_name, flattened.data, flattened.length);
					filemap_field f_data = filemap_cpyfieldref(&ctx->article_fmap, &obj, article_data_i);

					articledata_t* data = (articledata_t*)f_data.val.data;

					if (f_data.exists && data->ty==article_img) {
						new_url = heapstr("<a href=\"/wiki/%s\" ><img alt=\"%s\" src=\"/wikisrc/%s\" /></a>", outurl.data,
								vector_getstr(&w_path, w_path.length-1), outurl.data);
					} else {
						new_url = heapstr("<a href=\"/wiki/%s\" >%s</a>", outurl.data,
								vector_getstr(&w_path, w_path.length-1));
					}

					vector_free(&outurl);
					vector_free(&flattened);

					if (f_data.exists) 
						vector_free(&f_data.val);

				} else {
					new_url = heapstr("<a href=\"%s\" >%s</a>", url, url);
				}

				vector_removemany(&vec, remove_from, iter.i-remove_from);
				vector_insert_manycpy(&vec, remove_from, strlen(new_url), new_url);
				iter.i = remove_from + strlen(new_url);

				drop(new_url);
				drop(url);
				
				break;
			}

			//...
			default: if (vec.length-iter.i >= 3
				&& strncmp(iter.x, "```", strlen("```"))==0) {

				vector_removemany(&vec, iter.i-1, strlen("```"));
				vector_insertstr(&vec, iter.i-1, "<pre>");

				while (vector_next(&iter) && strncmp(iter.x, "```", strlen("```"))!=0);

				vector_removemany(&vec, iter.i-1, strlen("```"));
				vector_insertstr(&vec, iter.i-1, "</pre>");
				break;
			}
		}
	}

	*article = vec.data;
	return 0;
}

void update_article_refs(ctx_t* ctx, vector_t* flattened, vector_t* add_refs, vector_t* remove_refs, uint64_t idx) {
	int removing=!add_refs;

	//update refs
	vector_iterator iter = vector_iterate(removing ? remove_refs : add_refs);
	while (1) {
		if (!vector_next(&iter)) {
			if (removing || !remove_refs) break;

			removing = 1;
			iter = vector_iterate(remove_refs);
			continue;
		}

		if (!removing && remove_refs) {
			//if in both add and remove, remove from remove_refs and skip
			uint64_t* elem = vector_search(remove_refs, iter.x);
			if (elem) {
				vector_remove_element(remove_refs, (char*)elem);
				continue;
			}
		}

		vector_t ref_flattened = flatten_path(iter.x);

		//avoid deadlocks
		if (ref_flattened.length != flattened->length
			&& memcmp(ref_flattened.data, flattened->data, flattened->length)==0) {

			vector_free(&ref_flattened);
			continue;
		}

		lock_article(ctx, ref_flattened.data, ref_flattened.length);

		//-_-
		filemap_partial_object ref_ref = filemap_find(&ctx->article_by_name, ref_flattened.data, ref_flattened.length);
		if (ref_ref.exists) {
			filemap_partial_object partial = filemap_deref(&ctx->article_id, &ref_ref);
			filemap_object ref = filemap_cpy(&ctx->article_fmap, &partial);

			articledata_t* data = (articledata_t*)ref.fields[article_data_i];
			if (data->ty != article_group) {
				if (!removing) {
					filemap_push_field(&ref, article_items_i, 8, &idx);
					data->referenced_by++;

					filemap_object obj = filemap_push(&ctx->article_fmap, ref.fields, ref.lengths);
					filemap_list_update(&ctx->article_id, &partial, &obj);
					filemap_delete_object(&ctx->article_fmap, &ref);
				} else {
					vector_t ref_by = {.data=ref.fields[article_items_i], .length=data->referenced_by, .size=8};
					uint64_t* by = vector_search(&ref_by, &idx);

					if (by) {
						vector_remove_element(&ref_by, (char*)by);

						ref.fields[article_items_i] = ref_by.data;
						ref.lengths[article_items_i] -= 8;

						filemap_object obj = filemap_push(&ctx->article_fmap, ref.fields, ref.lengths);
						filemap_list_update(&ctx->article_id, &partial, &obj);
						filemap_delete_object(&ctx->article_fmap, &ref);
					}
				}
			}

			filemap_object_free(&ctx->article_fmap, &ref);

		} else if (!removing) {
			articledata_t data = {.ty=article_dead, .referenced_by=1};

			filemap_object obj = filemap_push(&ctx->article_fmap,
																				(char*[]){(char*)&data, (char*)&idx, ref_flattened.data, NULL, NULL},
																				(uint64_t[]){sizeof(articledata_t), 8, ref_flattened.length, 0, 0});

			filemap_partial_object partial = filemap_add(&ctx->article_id, &obj);
			filemap_object list_obj = filemap_index_obj(&obj, &partial);
			filemap_insert(&ctx->article_by_name, &list_obj);
		}

		unlock_article(ctx, ref_flattened.data, ref_flattened.length);

		vector_free(&ref_flattened);
	}
}

vector_t find_refs(char* content, vector_t* loc) {
	vector_t refs = vector_new(sizeof(vector_t));

	while (*content) {
		if (*content == '\\' && *(content+1)) {
			content+=2;
			continue;
		}

		if (*content == '[') {
			content++;
			if (*content != '!') continue;
			else content++;

			char* start = content;
			while (*content != ']' && *(++content));

			char* url = heapcpy(content-start+1, start);
			url[content-start] = 0;

			vector_t w_path = vector_new(sizeof(char*));

			if (!parse_wiki_path(url, &w_path)) {
				vector_free(&w_path);
			} else {
				vector_pushcpy(&refs, &w_path);
			}

			//push string locations if location is enabled
			if (loc)
				vector_pushcpy(loc, &(char*[3]){url, start, content});
			else drop(url);

			if (*content) content++; //skip bracket
		}

		content++;
	}

	return refs;
}

//lousy function, error handling already a horror
void refs_free(vector_t* refs) {
	vector_iterator iter = vector_iterate(refs);
	while (vector_next(&iter)) {
		vector_free_strings(iter.x);
	}
}

void rerender_articles(ctx_t* ctx, vector_t* articles, vector_t* from, char* to) {
	vector_iterator iter = vector_iterate(articles);
	while (vector_next(&iter)) {
		filemap_partial_object partial = filemap_get_idx(&ctx->article_id, *(uint64_t*)iter.x);
		filemap_object obj = filemap_cpy(&ctx->article_fmap, &partial);
		
		if (!obj.exists) continue;

		vector_t path = vector_from_strings(obj.fields[article_path_i],
			((articledata_t*)obj.fields[article_data_i])->path_length);
		
		//little late but should work fine
		vector_t flattened = flatten_path(&path);
		lock_article(ctx, flattened.data, flattened.length);

		vector_t filepath = flatten_wikipath(&path);

		text_t txt = txt_new(filepath.data);
		read_txt(&txt, 0);

		if (from && to) {
			diff_t d = {.additions=vector_new(sizeof(add_t)), .deletions=vector_new(sizeof(del_t))};
			vector_t url_strs = vector_new(sizeof(char*));

			//search for wiki links and replace from with to
			vector_t locs = vector_new(sizeof(char*[3]));
			vector_t refs = find_refs(txt.current, &locs);

			vector_t current = vector_from_string(txt.current);

			long offset=0;
			vector_iterator iter = vector_iterate(&refs);
			while (vector_next(&iter)) {
				char** pos = vector_get(&locs, iter.i-1);
				vector_t* w_path = iter.x;

				if (vector_cmpstr(w_path, from)==0) {
					long remove_from = offset + (long)(pos[1]-txt.current);

					vector_pushcpy(&d.deletions, &(del_t){.pos=remove_from, .txt=pos[0]});
					vector_pushcpy(&d.additions, &(add_t){.pos=remove_from, .txt=to});
					vector_pushcpy(&url_strs, &pos[0]);

					vector_removemany(&current, remove_from, pos[2]-pos[1]);
					vector_insertstr(&current, remove_from, to);

					offset += (long)strlen(to) - (long)(pos[2]-pos[1]);
				} else {
					drop(pos[0]);
				}

				vector_free_strings(w_path);
			}

			txt.current = current.data;
			add_diff(&txt, &d, current.data);
			
			vector_free_strings(&url_strs);
			refs_free(&refs);
		}

		char* html_cache = heapcpystr(txt.current);

		if (render_article(ctx, &html_cache, NULL)==0) {
			filemap_object new_obj = filemap_push_updated(&ctx->article_fmap, &obj, (update_t[]){{.field=article_html_i, .new=html_cache, .len=strlen(html_cache)+1}}, 1);

			filemap_list_update(&ctx->article_id, &partial, &new_obj);
			filemap_delete_object(&ctx->article_fmap, &obj);
			
			filemap_updated_free(&ctx->article_fmap, &new_obj);
		}

		unlock_article(ctx, flattened.data, flattened.length);

		txt_free(&txt);

		drop(html_cache);
		vector_free(&filepath);
		filemap_object_free(&ctx->article_fmap, &obj);
	}
}

int article_lock_groups(ctx_t* ctx, vector_t* path, vector_t* flattened, vector_t* groups) {
	vector_iterator iter = vector_iterate(path);
	iter.rev = 1;

	unsigned long key_len = flattened->length;

	while (vector_next(&iter)) {
		unsigned long segment_len = strlen(*(char**)iter.x);

		key_len -= segment_len + 1;  //\0 is a delimeter

		lock_article(ctx, flattened->data, key_len);

		filemap_partial_object group_ref =
				filemap_find(&ctx->article_by_name, flattened->data, key_len);
		filemap_partial_object group = filemap_deref(&ctx->article_id, &group_ref);

		if (group.exists) {
			filemap_field data = filemap_cpyfield(&ctx->article_fmap, &group, article_data_i);
			if (((articledata_t*)data.val.data)->ty != article_group) {
				vector_free(&data.val);

				vector_iterator iter_unlock = vector_iterate(path);
				iter_unlock.rev = 1;

				key_len = flattened->length;

				while (vector_next(&iter_unlock)) {
					unsigned long segment_len = strlen(*(char**)iter_unlock.x);
					key_len -= segment_len + 1;

					unlock_article(ctx, flattened->data, key_len);
				}

				return 0;
			}

			vector_free(&data.val);
		}

		vector_pushcpy(groups, &group);
	}

	return 1;
}

void article_group_insert(ctx_t* ctx, vector_t* groups, vector_t* path, vector_t* flattened, uint64_t user_idx, filemap_partial_object* item) {
	vector_iterator iter = vector_iterate(groups);
	iter.rev = 1;

	unsigned long key_len = flattened->length;

	while (vector_next(&iter)) {
		char last = iter.i == 1;

		unsigned long segment_len = strlen(vector_getstr(path, iter.i-1));

		key_len -= segment_len + 1;  //\0 is a delimeter

		filemap_partial_object* new_group = iter.x;

		if (!new_group->exists) {
			articledata_t groupdata = {.path_length = path->length - iter.i,
																	.ty = article_group, .items = 1,
																	.contributors = last};

			filemap_object obj = filemap_push(
					&ctx->article_fmap,

					(char*[]){(char*)&groupdata, (char*)&item->index, flattened->data,
						last ? (char*)&user_idx : NULL, NULL},

					(uint64_t[]){sizeof(articledata_t), 8, key_len, last ? 8 : 0, 0});

			*new_group = filemap_add(&ctx->article_id, &obj);

			filemap_object obj_ref = filemap_index_obj(&obj, new_group);
			filemap_insert(&ctx->article_by_name, &obj_ref);

		} else {
			filemap_object obj =
					filemap_cpy(&ctx->article_fmap, new_group);

			articledata_t* groupdata = (articledata_t*)obj.fields[article_data_i];

			vector_t items = {.data = obj.fields[article_items_i],
												.size = sizeof(uint64_t),
												.length = groupdata->items};
			
			vector_t contribs = {.data = obj.fields[article_contrib_i],
												.size = sizeof(uint64_t),
												.length = groupdata->contributors};
			
			int exists = vector_search(&items, &item->index)!=NULL;
			int user_exists = !last || vector_search(&contribs, &user_idx)!=NULL;
			
			if (!exists || !user_exists) {
				if (!exists) {
					filemap_push_field(&obj, article_items_i, 8, &item->index);
					groupdata->items++;
				} 

				if (!user_exists) {
					filemap_push_field(&obj, article_contrib_i, 8, &user_idx);
					groupdata->contributors++;
				}

				filemap_object new_obj = filemap_push(&ctx->article_fmap,
																							obj.fields, obj.lengths);
				filemap_list_update(&ctx->article_id, new_group, &new_obj);
				filemap_delete_object(&ctx->article_fmap, &obj);
			}

			filemap_object_free(&ctx->article_fmap, &obj);
		}

		unlock_article(ctx, flattened->data, key_len);

		item = new_group;
	}
}

void article_group_remove(ctx_t* ctx, vector_t* groups, vector_t* path, vector_t* flattened, filemap_partial_object* item) {
	vector_iterator iter = vector_iterate(groups);
	iter.rev = 1;

	unsigned long key_len = flattened->length;

	while (vector_next(&iter)) {
		unsigned long segment_len = strlen(vector_getstr(path, iter.i-1));
		key_len -= segment_len + 1;

		filemap_partial_object* group = iter.x;

		if (group->exists) {
			filemap_object obj =
					filemap_cpy(&ctx->article_fmap, group);

			articledata_t* groupdata = (articledata_t*)obj.fields[article_data_i];

			vector_t items = {.data = obj.fields[article_items_i],
												.size = sizeof(uint64_t),
												.length = groupdata->items};
			
			char* item_exists = vector_search(&items, &item->index);
			
			if (item_exists) {
				vector_remove_element(&items, item_exists);
				obj.fields[article_items_i] = items.data;
				obj.lengths[article_items_i] -= 8;
				
				groupdata->items--;

				filemap_object new_obj = filemap_push(&ctx->article_fmap,
																							obj.fields, obj.lengths);
				
				filemap_list_update(&ctx->article_id, group,
														&new_obj);
				filemap_delete_object(&ctx->article_fmap, &obj);
			} else {
				filemap_object_free(&ctx->article_fmap, &obj);
				return; //no child; chain broken
			}

			filemap_object_free(&ctx->article_fmap, &obj);
			item = group;
		} else {
			return; //no parent; chain broken
		}

		unlock_article(ctx, flattened->data, key_len);
	}
}

int article_new(ctx_t* ctx, filemap_partial_object* article, article_type ty,
	vector_t* path, vector_t* flattened, uint64_t user_idx, char* html_cache) {

	filemap_object idx = filemap_findcpy(&ctx->article_by_name,
			flattened->data, flattened->length);

	vector_t referenced_by = vector_new(sizeof(uint64_t));

	//AAAAA
	if (idx.exists) {
		articledata_t* data = (articledata_t*)idx.fields[article_data_i];

		if (data->ty == article_dead) {
			vector_stockcpy(&referenced_by, data->referenced_by, idx.fields[article_items_i]);
			filemap_object_free(&ctx->article_fmap, &idx);
		} else {
			filemap_object_free(&ctx->article_fmap, &idx);
			return 0;
		}
	}

	vector_t groups = vector_new(sizeof(filemap_partial_object));
	if (!article_lock_groups(ctx, path, flattened, &groups)) {
		filemap_object_free(&ctx->article_fmap, &idx);
		vector_free(&groups);
		return 0;
	}

	// insert blank, then update groups and finally insert real thing
	*article = filemap_add(&ctx->article_id, NULL);

	article_group_insert(ctx, &groups, path, flattened, user_idx, article);
	vector_free(&groups);

	articledata_t data = {
			.path_length = path->length, .ty = ty,
			.referenced_by = referenced_by.length, .contributors=1};

	filemap_object text = filemap_push(
			&ctx->article_fmap,

			(char*[]){(char*)&data, referenced_by.data, flattened->data,
				(char*)&user_idx, html_cache},

			(uint64_t[]){sizeof(articledata_t),
				referenced_by.length*referenced_by.size,
				flattened->length, 8, strlen(html_cache)+1});

	filemap_list_update(&ctx->article_id, article, &text);

	filemap_object text_ref = filemap_index_obj(&text, article);

	filemap_insert(&ctx->article_by_name, &text_ref);

	rerender_articles(ctx, &referenced_by, NULL, NULL);
	vector_free(&referenced_by);

	return 1;
}

void req_wiki_path(request* req) {
	drop(vector_removeptr(&req->path, 0));

	vector_iterator iter = vector_iterate(&req->path);
	while (vector_next(&iter)) {
		//not very sanitary... TODO: dont implicit free
		*(char**)iter.x = percent_decode(*(char**)iter.x);
	}
}

int route_article(session_t* session, request* req, filemap_object* obj) {
	req_wiki_path(req);
	vector_t flattened = flatten_path(&req->path);
	
	filemap_partial_object article_ref;
	if (*flattened.data == 0) { //get top
		article_ref = filemap_find(&session->ctx->article_by_name, NULL, 0);
	} else {
		article_ref = filemap_find(&session->ctx->article_by_name, flattened.data, flattened.length);
	}

	*obj = filemap_cpyref(&session->ctx->article_fmap, &article_ref);

	if (!obj->exists ||
			((articledata_t*)obj->fields[article_data_i])->ty == article_dead) {

		vector_t path_formatted = flatten_url(&req->path);

		int has_perm = get_perms(session) & perms_create_article;
		if (has_perm) {
			respond_template(session, 200, "new", "New article", 1, 1,
					"Article does not exist", path_formatted.data, "");
		} else {
			char* err = heapstr("%s does not exist", path_formatted.data);
			respond_error(session, 404, err);
		}

		vector_free(&flattened);
		vector_free(&path_formatted);

		if (obj->exists)
			filemap_object_free(&session->ctx->article_fmap, obj);

		return 0;
	}

	vector_free(&flattened);
	return 1;
}

void route(session_t* session, request* req) {
  if (req->path.length == 0) {
    filemap_object top = filemap_findcpy(&session->ctx->article_by_name, "", 0);

    vector_t items_arg;
    vector_t item_strs = vector_new(sizeof(char*));

    if (top.exists) {
      articledata_t* data = (articledata_t*)top.fields[article_data_i];
      items_arg = article_group_list(session->ctx, &top, data, &item_strs);
    } else {
      items_arg = vector_new(sizeof(template_args));
    }

		if (session->user_ses->user.exists) {
			filemap_field uname = filemap_cpyfield(&session->ctx->user_fmap, &session->user_ses->user, user_name_i);
			respond_template(session, 200, "home", "ranch", 1, get_perms(session) & perms_create_article, &items_arg, uname.val.data);

			vector_free(&uname.val);
		} else {
			respond_template(session, 200, "home", "ranch", 0, 0, &items_arg, "");
		}

    filemap_object_free(&session->ctx->article_fmap, &top);
    vector_free_strings(&item_strs);

    return;
  }

  char* base = vector_getstr(&req->path, 0);

  if (strcmp(base, "login") == 0 && req->method == GET) {
    respond_template(session, 200, "login", "Login", 0, "");

  } else if (strcmp(base, "register") == 0 && req->method == POST) {
    vector_t params = query_find(
        &req->query, (char*[]){"username", "email", "password"}, 3, 1);

    if (params.length != 3) {
      respond_error(session, 400, "Username email and password not provided");
      vector_free(&params);
      return;
    }

    char *username = vector_getstr(&params, 0),
         *email = vector_getstr(&params, 1),
         *password = vector_getstr(&params, 2);

    char* err = user_error(username, email);
    if (err) {
      respond_template(session, 200, "login", "Login", 1, err);
      vector_free(&params);
      return;
    }

    char* passerr = user_password_error(password);
    if (passerr) {
      respond_template(session, 200, "login", "Login", 1, passerr);
      vector_free(&params);
      return;
    }

    filemap_partial_object name_user =
        filemap_find(&session->ctx->user_by_name,
                     username, strlen(username) + 1);
    filemap_partial_object email_user =
        filemap_find(&session->ctx->user_by_email,
                     email, strlen(email) + 1);

    if (name_user.exists || email_user.exists) {
      respond_template(session, 200, "login", "Login", 1, "Email or username already taken");
      vector_free(&params);
      return;
    }

    userdata_t data;
    data.perms = perms_none;

    RAND_bytes((unsigned char*)&data.salt, 4);
    // lmao
    EVP_DigestInit_ex(session->ctx->digest_ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(session->ctx->digest_ctx, password, strlen(password));
    EVP_DigestUpdate(session->ctx->digest_ctx, &data.salt, 4);

    EVP_DigestFinal_ex(session->ctx->digest_ctx, data.password_hash, NULL);

    filemap_object user = filemap_push(
        &session->ctx->user_fmap, (char*[]){username, email, (char*)&data, ""},
        (uint64_t[]){strlen(username) + 1, strlen(email) + 1,
                     sizeof(userdata_t), 1});

    filemap_partial_object idx = filemap_add(&session->ctx->user_id, &user);

    filemap_object user_ref = filemap_index_obj(&user, &idx);
    filemap_insert(&session->ctx->user_by_name, &user_ref);
    filemap_insert(&session->ctx->user_by_name, &user_ref);

    session->user_ses->user = idx;
    map_insertcpy(&session->ctx->user_sessions_by_idx, &idx.index, &session->user_ses);

    respond_redirect(session, "/account");

  } else if (strcmp(base, "login") == 0 && req->method == POST) {
    vector_t params =
        query_find(&req->query, (char*[]){"username", "password"}, 2, 1);

    if (params.length != 2) {
      respond_error(session, 400, "Username and password not provided");
      vector_free(&params);
      return;
    }

    char *username = vector_getstr(&params, 0),
         *password = vector_getstr(&params, 1);

    filemap_partial_object user_find =
      filemap_find(&session->ctx->user_by_name, username, strlen(username) + 1);

    filemap_partial_object user_ref =
        filemap_deref(&session->ctx->user_id, &user_find);

    filemap_object user = filemap_cpy(&session->ctx->user_fmap, &user_ref);

    if (!user.exists) {
      respond_template(session, 200, "login", "Login", 1,
                       "Username and password do not match");
      vector_free(&params);
      return;
    }

    userdata_t* data = (userdata_t*)user.fields[article_path_i];

    EVP_DigestInit_ex(session->ctx->digest_ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(session->ctx->digest_ctx, password, strlen(password));
    EVP_DigestUpdate(session->ctx->digest_ctx, &data->salt, 4);

    unsigned char password_hash[HASH_LENGTH];
    EVP_DigestFinal_ex(session->ctx->digest_ctx, password_hash, NULL);

    if (memcmp(data->password_hash, password_hash, HASH_LENGTH) != 0) {
      respond_template(session, 200, "login", "Login", 1,
                       "Username and password do not match");
      vector_free(&params);
      return;
    }

    //lock when accessing both maps, so they stay in concord
    mtx_lock(&session->user_ses->lock);
		
    user_session** old_ses =
        map_find(&session->ctx->user_sessions_by_idx, &user_ref.index);
    if (old_ses) (*old_ses)->user.exists = 0;
		
    session->user_ses->user = user_ref;

    map_insertcpy(&session->ctx->user_sessions_by_idx, &user_ref.index, &session->user_ses);
		
    mtx_unlock(&session->user_ses->lock);

    respond_redirect(session, "/account");
    filemap_object_free(&session->ctx->user_fmap, &user);
    vector_free(&params);

  } else if (strcmp(base, "account") == 0 && req->method == GET) {
    char** arg = vector_get(&req->path, 1);

    //one step, no need for lock

    filemap_object user;
    if (!arg) {
      user = filemap_cpy(&session->ctx->user_fmap, &session->user_ses->user);
      if (!user.exists) {
        respond_template(session, 200, "login", "Login", 1,
                         "You aren't logged in");
        return;
      }

      respond_template(session, 200, "account", user.fields[user_name_i], 0, "",
                       user.fields[user_name_i], user.fields[user_email_i], user.fields[user_bio_i]);

    } else {
      user =
          filemap_findcpy(&session->ctx->user_by_name,
                          *arg, strlen(*arg) + 1);

      if (!user.exists) {
        respond_error(session, 404, "User not found");
        return;
      }

      respond_template(session, 200, "profile", user.fields[user_name_i],
                       get_perms(session) & perms_admin, user.fields[user_name_i],
                       user.fields[user_bio_i]);
    }

    filemap_object_free(&session->ctx->user_fmap, &user);

  } else if (strcmp(base, "account") == 0 && req->method == POST) {
    char** target = vector_get(&req->path, 1);
    if (target) {
      if (!(get_perms(session) & perms_admin)) {
        respond_error(session, 403, "You are not opped in my minecraft server");
        return;
      }

      vector_t params = query_find(&req->query, (char*[]){"permissions"}, 1, 1);

      if (params.length != 1) {
        respond_error(session, 400, "Permissions missing");
        vector_free(&params);
        return;
      }

      filemap_partial_object name_user =
        filemap_find(&session->ctx->user_by_name, *target, strlen(*target) + 1);

      if (!name_user.exists) {
        respond_error(session, 404, "User not found");
        vector_free(&params);
        return;
      }

      filemap_partial_object list_user = filemap_deref(&session->ctx->user_id, &name_user);

      user_session** user_ses_ref = map_find(&session->ctx->user_sessions_by_idx, &list_user.index);
			user_session* user_ses = user_ses_ref ? *user_ses_ref : NULL;
      if (user_ses) mtx_lock(&user_ses->lock);

      filemap_object usee = filemap_cpy(&session->ctx->user_fmap, &list_user);

      char* perms_str = vector_getstr(&params, 0);
      uint8_t usee_perms = (uint8_t)atoi(perms_str);

      userdata_t* data = (userdata_t*)usee.fields[user_data_i];
      data->perms = usee_perms;

      filemap_object new_u = filemap_push(&session->ctx->user_fmap, usee.fields, usee.lengths);
      filemap_list_update(&session->ctx->user_id, &list_user, &new_u);
      filemap_delete_object(&session->ctx->user_fmap, &usee);

      if (user_ses) {
        user_ses->user = filemap_partialize(&list_user, &new_u);
        mtx_unlock(&user_ses->lock);
      }

      respond_template(session, 200, "profile", usee.fields[user_name_i], 1,
                       usee.fields[user_name_i], usee.fields[user_bio_i]);

      filemap_object_free(&session->ctx->user_fmap, &usee);
      vector_free(&params);

      return;
    }

    mtx_lock(&session->user_ses->lock);

    filemap_object user =
        filemap_cpy(&session->ctx->user_fmap, &session->user_ses->user);

    if (!user.exists) {
      respond_template(session, 200, "login", "Login", 1,
                       "You aren't logged in");
      
      mtx_unlock(&session->user_ses->lock);
      return;
    }

    vector_t params =
        query_find(&req->query, (char*[]){"username", "email", "bio"}, 3, 1);

    if (params.length != 3) {
      respond_error(session, 400, "Username email and biography not provided");
      mtx_unlock(&session->user_ses->lock);
      vector_free(&params);
      return;
    }

    char *username = vector_getstr(&params, 0),
         *email = vector_getstr(&params, 1),
         *bio = vector_getstr(&params, 2);

    char* err = user_error(username, email);
    if (err) {
      respond_template(session, 200, "account", user.fields[user_name_i], 1, err,
                       user.fields[user_name_i], user.fields[user_email_i], user.fields[user_bio_i]);
      
      filemap_object_free(&session->ctx->user_fmap, &user);
      mtx_unlock(&session->user_ses->lock);
      vector_free(&params);
      return;
    }

    int name_change = strcmp(user.fields[user_name_i], username) != 0,
        email_change = strcmp(user.fields[user_email_i], email) != 0,
        bio_change = strcmp(user.fields[user_bio_i], bio) != 0;

    if (!name_change && !email_change && !bio_change) {
      respond_template(session, 200, "account", user.fields[user_name_i], 0, "",
                       user.fields[user_name_i], user.fields[user_email_i], user.fields[user_bio_i]);
      
      filemap_object_free(&session->ctx->user_fmap, &user);
      mtx_unlock(&session->user_ses->lock);
      vector_free(&params);
      return;
    }

    filemap_partial_object name_user =
        filemap_find(&session->ctx->user_by_name,
                     username, strlen(username) + 1);

    if (name_change && name_user.exists) {
      respond_template(session, 200, "account", user.fields[user_name_i], 1,
                       "Username already taken", user.fields[user_name_i], user.fields[user_email_i],
                       user.fields[user_bio_i]);

      filemap_object_free(&session->ctx->user_fmap, &user);
      mtx_unlock(&session->user_ses->lock);
      vector_free(&params);
      return;
    }

    filemap_partial_object email_user =
        filemap_find(&session->ctx->user_by_email,
                     email, strlen(email) + 1);

    if (email_change && email_user.exists) {
      respond_template(session, 200, "account", user.fields[user_name_i], 1,
                       "Email is already in use", username, email, bio);
      filemap_object_free(&session->ctx->user_fmap, &user);
      mtx_unlock(&session->user_ses->lock);
      vector_free(&params);
      return;
    }

    // delete old indices
    if (name_change) {
      filemap_remove(&session->ctx->user_by_name, user.fields[user_name_i], strlen(user.fields[user_name_i]) + 1);
    }

    if (email_change) {
      filemap_remove(&session->ctx->user_by_email, user.fields[user_email_i], strlen(user.fields[user_email_i]) + 1);
    }

    // insert new user and update (new) indices
    if (name_change || email_change || bio_change) {
      filemap_object new = filemap_push(&session->ctx->user_fmap,
													(char*[]){username, email, user.fields[user_data_i], bio},
                          (uint64_t[]){strlen(username) + 1, strlen(email) + 1,
                                       user.lengths[user_data_i], strlen(bio) + 1});
			
			filemap_list_update(&session->ctx->user_id, &session->user_ses->user, &new);
			filemap_delete_object(&session->ctx->user_fmap, &user);

      filemap_object new_ref = filemap_index_obj(&new, &session->user_ses->user);

      if (name_change) filemap_insert(&session->ctx->user_by_name, &new_ref);
      if (email_change) filemap_insert(&session->ctx->user_by_email, &new_ref);
    }

    respond_template(session, 200, "account", username, 1, "Updated profile",
                     username, email, bio);
    filemap_object_free(&session->ctx->user_fmap, &user);
    vector_free(&params);

    mtx_unlock(&session->user_ses->lock);

  } else if (strcmp(base, "password") == 0 && req->method == POST) {
    mtx_lock(&session->user_ses->lock);

    filemap_object user =
        filemap_cpy(&session->ctx->user_fmap, &session->user_ses->user);

    if (!user.exists) {
      respond_template(session, 200, "login", "Login", 1,
                       "You aren't logged in");
      return;
    }

    vector_t params = query_find(&req->query, (char*[]){"password"}, 1, 1);

    if (params.length != 1) {
      respond_error(session, 400, "Password not provided");
      filemap_object_free(&session->ctx->user_fmap, &user);
      
      mtx_unlock(&session->user_ses->lock);
      vector_free(&params);
      return;
    }

    char* password = vector_getstr(&params, 0);
    char* passerr = user_password_error(password);

    if (passerr) {
      respond_template(session, 200, "account", user.fields[user_name_i], 1, passerr,
                       user.fields[user_name_i], user.fields[user_email_i], user.fields[user_bio_i]);

      filemap_object_free(&session->ctx->user_fmap, &user);
      mtx_unlock(&session->user_ses->lock);
      vector_free(&params);
      return;
    }

    userdata_t* data = (userdata_t*)user.fields[user_data_i];

    EVP_DigestInit_ex(session->ctx->digest_ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(session->ctx->digest_ctx, password, strlen(password));
    EVP_DigestUpdate(session->ctx->digest_ctx, &data->salt, 4);

    EVP_DigestFinal_ex(session->ctx->digest_ctx, data->password_hash, NULL);

    // no length changes, refer to old data and updated userdata_t
    filemap_object new = filemap_push(&session->ctx->user_fmap, user.fields, user.lengths);
		filemap_list_update(&session->ctx->user_id, &session->user_ses->user, &new);
		filemap_delete_object(&session->ctx->user_fmap, &user);

    respond_template(session, 200, "account", user.fields[user_name_i], 1,
                     "Changed password successfully", user.fields[user_name_i],
                     user.fields[user_email_i], user.fields[user_bio_i]);
    filemap_object_free(&session->ctx->user_fmap, &user);

    mtx_unlock(&session->user_ses->lock);

  } else if (strcmp(base, "logout") == 0) {
    mtx_lock(&session->user_ses->lock);

    if (session->user_ses->user.exists) {
      map_remove(&session->ctx->user_sessions_by_idx, &session->user_ses->user.index);
    }

    session->user_ses->user.exists = 0;

    mtx_unlock(&session->user_ses->lock);
    respond_redirect(session, "/");

  } else if (strcmp(base, "new") == 0 && req->method == GET) {
    int has_perm = get_perms(session) & perms_create_article;
    respond_template(session, 200, "new", "New article", has_perm, 0, "", "", "");

  } else if (strcmp(base, "new") == 0 && req->method == POST) {
    //lock for consistent contributors
    mtx_lock(&session->user_ses->lock);
    
    if (!(get_perms(session) & perms_create_article)) {
      respond_template(session, 200, "new", "New article", 0, 0, "", "");
      mtx_unlock(&session->user_ses->lock);
      return;
    }

    vector_t params =
        query_find(&req->query, (char*[]){"path", "content"}, 2, 1);

    if (params.length != 2) {
      respond_error(session, 400, "Path and content of article not provided");
      mtx_unlock(&session->user_ses->lock);
      vector_free(&params);
      return;
    }

    char* content = vector_getstr(&params, 1);

    char* path_str = vector_getstr(&params, 0);

    vector_t path = vector_new(sizeof(char*));
    if (!parse_wiki_path(path_str, &path)) {
      respond_template(session, 200, "new", "New article", 1, 1, "Invalid path",
                       path_str, content);

      mtx_unlock(&session->user_ses->lock);
      vector_free_strings(&path);
      vector_free(&params);
      return;
    }

		char* html_cache = heapcpystr(content);
		vector_t refs = vector_new(sizeof(vector_t));

		unsigned long col = render_article(session->ctx, &html_cache, &refs);

		if (col!=0) {
			char* err = heapstr("Syntax error at column %lu", col);
			respond_template(session, 200, "new", "New article", 1, 1,
					err, path_str, content);

			mtx_unlock(&session->user_ses->lock);

			drop(html_cache);
			refs_free(&refs);

			vector_free_strings(&path);
			vector_free(&params);
			return;
		}

		vector_t flattened = flatten_path(&path);

		//ensure slot is reserved
		lock_article(session->ctx, flattened.data, flattened.length);

		filemap_partial_object article;
		if (!article_new(session->ctx, &article, article_text,
				&path, &flattened, session->user_ses->user.index, html_cache)) {

			respond_template(session, 200, "new", "New article", 1, 1,
					"Article with that path already exists",
					path_str, content);

			mtx_unlock(&session->user_ses->lock);
			unlock_article(session->ctx, flattened.data, flattened.length);

			drop(html_cache);
			refs_free(&refs);

			vector_free_strings(&path);
			vector_free(&flattened);
			vector_free(&params);
			return;
		}

    mtx_unlock(&session->user_ses->lock);

		update_article_refs(session->ctx, &flattened, &refs, NULL, article.index);
		refs_free(&refs);
		
    // display/file path
    vector_t out_path = make_path(&path);

		//add diff
		diff_t d = {.additions = vector_new(sizeof(add_t)),
			.deletions = vector_new(sizeof(del_t))};
		vector_pushcpy(&d.additions, &(add_t){.pos = 0, .txt = content});

		text_t txt = txt_new(out_path.data);
		add_diff(&txt, &d, content);

		vector_free(&d.additions);
		vector_free(&d.deletions);
		
		vector_t url = flatten_url(&path);
    vector_insertstr(&url, 0, "wiki/");

		unlock_article(session->ctx, flattened.data, flattened.length);

		//redirect
    respond_redirect(session, url.data);

		drop(html_cache);
    vector_free_strings(&path);
    vector_free(&flattened);
    vector_free(&out_path);
    vector_free(&params);
    vector_free(&url);

		//very similar to /new but changes for multipart and files
  } else if (strcmp(base, "upload")==0 && req->method==POST) {
    mtx_lock(&session->user_ses->lock);
    
    if (!(get_perms(session) & perms_create_article)) {
      respond_template(session, 200, "new", "New article", 0, 0, "", "");
      mtx_unlock(&session->user_ses->lock);
      return;
    }

		//search multipart data
    vector_t params = multipart_find(&req->files, (char*[]){"path", "file"}, 2, 1);

    if (params.length != 2) {
      respond_error(session, 400, "Path and file not provided");
      mtx_unlock(&session->user_ses->lock);
      vector_free(&params);
      return;
    }

    multipart_data* path_mp = vector_get(&params, 0);
    multipart_data* content = vector_get(&params, 1);
		
		char* path_str = heapcpy(path_mp->len+1, path_mp->content);
		path_str[path_mp->len] = 0;
		
		if (!content->mime) {
			respond_error(session, 400, "Missing mime for file");
			mtx_unlock(&session->user_ses->lock);
			vector_free(&params);
			return;
		}

    vector_t path = vector_new(sizeof(char*));
    if (!parse_wiki_path(path_str, &path)) {
      respond_template(session, 200, "new", "New article", 1, 1, "Invalid path",
                       path_str, "");

      mtx_unlock(&session->user_ses->lock);
			drop(path_str);

      vector_free_strings(&path);
      vector_free(&params);
      return;
    }

		vector_t flattened = flatten_path(&path);

		lock_article(session->ctx, flattened.data, flattened.length);

		filemap_partial_object article;
		if (!article_new(session->ctx, &article, article_img,
				&path, &flattened, session->user_ses->user.index, content->mime)) {

			respond_template(session, 200, "new", "New article", 1, 1,
					"Article with that path already exists",
					path_str, "");

			mtx_unlock(&session->user_ses->lock);
			unlock_article(session->ctx, flattened.data, flattened.length);
			drop(path_str);

			vector_free_strings(&path);
			vector_free(&flattened);
			vector_free(&params);
			return;
		}

    mtx_unlock(&session->user_ses->lock);
		
    vector_t out_path = make_path(&path);

		FILE* f = fopen(out_path.data, "w");
		fwrite(content->content, content->len, 1, f);
		fclose(f);

		unlock_article(session->ctx, flattened.data, flattened.length);

		vector_t url = flatten_url(&path);
    vector_insertstr(&url, 0, "wiki/");

    respond_redirect(session, url.data);
		vector_free(&url);
    
		drop(path_str);

		vector_free(&out_path);
    vector_free_strings(&path);
    vector_free(&flattened);
    vector_free(&params);

	} else if (strcmp(base, "edit")==0 && req->method==GET) {
		if (!(get_perms(session) & perms_edit_article)) {
      respond_template(session, 200, "edit", "Edit article", 0, 0, "", "", "", "");
      return;
    }
		
		req_wiki_path(req);
		vector_t flattened = flatten_path(&req->path);

		filemap_partial_object ref = filemap_find(&session->ctx->article_by_name, flattened.data, flattened.length);
		if (!ref.exists) {
			respond_error(session, 404, "Article cannot be edited; it doesn't exist");
			vector_free(&flattened);
			return;
		}

		vector_t data = filemap_cpyfieldref(&session->ctx->article_fmap, &ref, article_data_i).val;
		if (((articledata_t*)data.data)->ty != article_text) {
			respond_template(session, 200, "edit", "Edit article", 1, 1, "Cannot edit a non-textual article", "", "", "");
			vector_free(&flattened);
			vector_free(&data);
			return;
		}

		vector_t wpath = flatten_wikipath(&req->path);
		cached* current = article_current(session->ctx, &wpath);
		
		vector_t path = flatten_url(&req->path);
		respond_template(session, 200, "edit", "Edit article", 1, 0, "", path.data, path.data, current->data);

		ctx_cache_done(session->ctx, current, wpath.data);

		vector_free(&flattened);
		vector_free(&wpath);
		vector_free(&path);
		vector_free(&data);

	} else if (strcmp(base, "edit")==0 && req->method==POST) {
    mtx_lock(&session->user_ses->lock);
    
    if (!(get_perms(session) & perms_edit_article)) {
      respond_template(session, 200, "edit", "Edit article", 0, 0, "", "", "", "");
      mtx_unlock(&session->user_ses->lock);
      return;
    }

    vector_t params =
        query_find(&req->query, (char*[]){"old-path", "new-path", "content"}, 3, 1);

    if (params.length != 3) {
      respond_error(session, 400, "Paths and content of article not provided");
      mtx_unlock(&session->user_ses->lock);
      vector_free(&params);
      return;
    }

    char* path_str = vector_getstr(&params, 0);
    char* new_path_str = vector_getstr(&params, 1);

    char* content = vector_getstr(&params, 2);

    vector_t path = vector_new(sizeof(char*));
    vector_t new_path = vector_new(sizeof(char*));

    if (!parse_wiki_path(path_str, &path) || !parse_wiki_path(new_path_str, &new_path)) {
      respond_template(session, 200, "edit", "Edit article", 1, 1, "Invalid path",
                       path_str, new_path_str, content);

      mtx_unlock(&session->user_ses->lock);
      vector_free_strings(&new_path);
      vector_free_strings(&path);
      vector_free(&params);
      return;
    }

		vector_t flattened = flatten_path(&path);
		vector_t new_flattened = flatten_path(&new_path);

		lock_article(session->ctx, flattened.data, flattened.length);

		filemap_partial_object article_ref = filemap_find(&session->ctx->article_by_name, flattened.data, flattened.length);
		filemap_partial_object article = filemap_deref(&session->ctx->article_id, &article_ref);
		filemap_object obj = filemap_cpy(&session->ctx->article_fmap, &article);
		
		articledata_t* data = obj.exists ? (articledata_t*)obj.fields[article_data_i] : NULL;
		if (!obj.exists || data->ty != article_text) {
      respond_template(session, 200, "edit", "Edit article", 1, 1, "Article does not exist / is not a text",
                       path_str, new_path_str, content);

      mtx_unlock(&session->user_ses->lock);
			filemap_object_free(&session->ctx->article_fmap, &obj);
			unlock_article(session->ctx, flattened.data, flattened.length);

      vector_free_strings(&new_path);
      vector_free_strings(&path);
      vector_free(&params);
			return;
		}

		int path_change = vector_cmpstr(&path, &new_path)!=0;

		filemap_partial_object new_article;
		filemap_object new_article_obj;

		vector_t new_referenced_by = vector_new(sizeof(uint64_t));
		vector_t new_groups;

		if (path_change) {
			lock_article(session->ctx, new_flattened.data, new_flattened.length);

			filemap_partial_object new_article_ref =
				filemap_find(&session->ctx->article_by_name, new_flattened.data, new_flattened.length);
			new_article = filemap_deref(&session->ctx->article_id, &new_article_ref);

			new_article_obj = filemap_cpy(&session->ctx->article_fmap, &new_article);

			int group_res;
			articledata_t* new_data;

			//shorten error handling...
			if (!new_article.exists) {
				new_groups = vector_new(sizeof(filemap_partial_object));
				group_res = article_lock_groups(session->ctx, &new_path, &new_flattened, &new_groups);
			} else {
				new_data = (articledata_t*)new_article_obj.fields[article_data_i];
				if (new_data->ty == article_dead)
					vector_stockcpy(&new_referenced_by, data->referenced_by, new_article_obj.fields[article_items_i]);
			}

			if ((new_article.exists && new_data->ty!=article_dead) || !group_res) {
				respond_template(session, 200, "edit", "Edit article", 1, 1, "New path is already taken", //TODO: which segment
						path_str, new_path_str, content);

				mtx_unlock(&session->user_ses->lock);

				filemap_object_free(&session->ctx->article_fmap, &obj);
				if (new_article.exists) filemap_object_free(&session->ctx->article_fmap, &new_article_obj);

				unlock_article(session->ctx, flattened.data, flattened.length);
				unlock_article(session->ctx, new_flattened.data, new_flattened.length);

				vector_free_strings(&new_path);
				vector_free_strings(&path);
				vector_free(&params);
				return;
			}
		}

		char* html_cache = heapcpystr(content);
		vector_t refs = vector_new(sizeof(vector_t));

		unsigned long col = render_article(session->ctx, &html_cache, &refs);

		if (col!=0) {
			char* err = heapstr("Syntax error at column %lu", col);
			respond_template(session, 200, "edit", "Edit article", 1, 1,
					err, path_str, new_path_str, content);

			mtx_unlock(&session->user_ses->lock);
			filemap_object_free(&session->ctx->article_fmap, &obj);

			unlock_article(session->ctx, flattened.data, flattened.length);
			if (path_change)
				unlock_article(session->ctx, new_flattened.data, new_flattened.length);

			if (path_change && new_article.exists)
				filemap_object_free(&session->ctx->article_fmap, &new_article_obj);
			filemap_object_free(&session->ctx->article_fmap, &obj);

			drop(html_cache);
			refs_free(&refs);
			vector_free(&new_referenced_by);

      vector_free_strings(&new_path);
			vector_free_strings(&path);
			vector_free(&params);
			return;
		}

		//1. if path change, remove from old dir and unnecessary folders, add into new
		//2. if path change, use referenced by to find and replace references between brackets
		// (remember to replace in article itself as well, bc those are ignored)
		//3. find disjoint union of references if content changed and update referenced by
		//4. find changes, add diff
		//5. change content

		vector_t wpath = flatten_wikipath(&path);
		text_t txt = txt_new(wpath.data);
		read_txt(&txt, 0);

		int content_change = strcmp(txt.current, content)!=0;

		vector_t* flattened_path = path_change ? &new_flattened : &flattened;

		//revise refs / add diff
		if (content_change) {
			vector_t old_refs = find_refs(txt.current, NULL);
			update_article_refs(session->ctx, flattened_path,
													&refs, &old_refs, article.index);
			refs_free(&old_refs);

			diff_t d = find_changes(txt.current, content);
			if (path_change) {
				vector_t new_wpath = flatten_wikipath(&new_path);

				text_t new_txt = txt_new(new_wpath.data);
				add_diff(&new_txt, &d, content);
				vector_free(&new_wpath);
			} else {
				add_diff(&txt, &d, content);
			}

			diff_free(&d);
		}
		
		//update object (if at all)
		filemap_object new_obj;
		if (content_change || path_change) {
			vector_t contribs = {.data = obj.fields[article_contrib_i], .size = 8, .length = data->contributors};
			
			if (vector_search(&contribs, &session->user_ses->user.index)==0) {
				filemap_push_field(&obj, article_contrib_i, 8, &session->user_ses->user.index);
			}

			new_obj = filemap_push_updated(&session->ctx->article_fmap, &obj,
				(update_t[]){{.field=article_path_i, .new=flattened_path->data, .len=flattened_path->length},
					{.field=article_html_i, .new=html_cache, .len=strlen(html_cache)+1}}, 2);

			if (path_change) {
				new_article = filemap_add(&session->ctx->article_id, &new_obj);
			} else {
				filemap_list_update(&session->ctx->article_id, &article, &new_obj);
			}

			filemap_delete_object(&session->ctx->article_fmap, &obj);
		}

		vector_t url;

		if (path_change) {
			filemap_object idx_obj = filemap_index_obj(&new_obj, &new_article);
			filemap_insert(&session->ctx->article_by_name, &idx_obj);

			article_group_insert(session->ctx, &new_groups, &new_path, &new_flattened,
				session->user_ses->user.index, &new_article);
			vector_free(&new_groups);
			
			vector_t old_groups = vector_new(sizeof(filemap_partial_object));
			if (article_lock_groups(session->ctx, &path, &flattened, &old_groups))
				article_group_remove(session->ctx, &old_groups, &path, &flattened, &article);
			vector_free(&old_groups);
			
			filemap_remove(&session->ctx->article_by_name, flattened.data, flattened.length);
			
			url = flatten_url(&new_path);
			vector_t referenced_by = {.data=obj.fields[article_items_i], .size=8, .length=data->referenced_by};
			rerender_articles(session->ctx, &referenced_by, &path, url.data);

			vector_free(&referenced_by);
		} else {
			url = flatten_url(&path);
		}
		
		mtx_unlock(&session->user_ses->lock);

		ctx_cache_remove(session->ctx, wpath.data);

		unlock_article(session->ctx, flattened.data, flattened.length);
		if (path_change)
			unlock_article(session->ctx, new_flattened.data, new_flattened.length);

    vector_insertstr(&url, 0, "wiki/");

		//redirect
		respond_redirect(session, url.data);

		//holy shit
		drop(html_cache);
		refs_free(&refs);
		txt_free(&txt);

		if (path_change && new_article.exists)
			filemap_object_free(&session->ctx->article_fmap, &new_article_obj);
		filemap_object_free(&session->ctx->article_fmap, &obj);
		
		if (path_change || content_change)
			filemap_updated_free(&session->ctx->article_fmap, &new_obj);
		
    vector_free_strings(&path);
		vector_free_strings(&new_path);

    vector_free(&flattened);
		vector_free(&wpath);
		vector_free(&url);
		
    if (path_change)
			vector_free(&new_flattened);
		
    vector_free(&params);
		
	} else if (strcmp(base, "delete")==0 && req->method==POST) {
		if (!(get_perms(session) & perms_delete_article)) {
			respond_error(session, 500, "You can't delete stuff");
			return;
		}

		req_wiki_path(req);
		vector_t flattened = flatten_path(&req->path);

		lock_article(session->ctx, flattened.data, flattened.length);

		filemap_partial_object ref = filemap_find(&session->ctx->article_by_name, flattened.data, flattened.length);
		filemap_partial_object article = filemap_deref(&session->ctx->article_id, &ref);

		filemap_object obj = filemap_cpy(&session->ctx->article_fmap, &article);
		if (!obj.exists) {
			respond_error(session, 404, "Article does not exist");
			unlock_article(session->ctx, flattened.data, flattened.length);
			return;
		}
		
		articledata_t* data = (articledata_t*)obj.fields[article_data_i];
		
		if (data->ty != article_text && data->ty != article_img) {
			respond_error(session, 422, "Article is not a text article, perhaps it is already dead");
			unlock_article(session->ctx, flattened.data, flattened.length);
			filemap_object_free(&session->ctx->article_fmap, &obj);
			return;
		}

		int img = data->ty == article_img;
		data->ty = article_dead;
		
		filemap_object new_obj = filemap_push(&session->ctx->article_fmap, obj.fields, obj.lengths);
		filemap_list_update(&session->ctx->article_id, &article, &new_obj);
		filemap_delete_object(&session->ctx->article_fmap, &obj);
		
		vector_t referenced_by = {.data=obj.fields[article_items_i], .size=8, .length=data->referenced_by};
		rerender_articles(session->ctx, &referenced_by, NULL, NULL);
		
		vector_t groups = vector_new(sizeof(filemap_partial_object));
		article_group_remove(session->ctx, &groups, &req->path, &flattened, &article);
		vector_free(&groups);

		vector_t wpath = flatten_wikipath(&req->path);
		ctx_cache_remove(session->ctx, wpath.data);
		
		if (!img) {
			text_t txt = txt_new(wpath.data);
			read_txt(&txt, 0);

			diff_t d = {.additions=vector_new(sizeof(add_t)), .deletions=vector_new(sizeof(del_t))};
			vector_pushcpy(&d.deletions, &(del_t){.txt=txt.current, .pos=0});

			add_diff(&txt, &d, "");

			vector_free(&d.additions);
			vector_free(&d.deletions);
			txt_free(&txt);
		}
		
		unlock_article(session->ctx, flattened.data, flattened.length);
		
		drop(vector_popptr(&req->path));

		vector_t redir = flatten_url(&req->path);
		vector_insertstr(&redir, 0, "/wiki/");

		respond_redirect(session, redir.data);

		filemap_object_free(&session->ctx->article_fmap, &obj);
		vector_free(&flattened);
		vector_free(&wpath);
		vector_free(&redir);

	} else if (strcmp(base, "wiki") == 0) {
		filemap_object obj;
		if (!route_article(session, req, &obj)) return;

    articledata_t* data = (articledata_t*)obj.fields[article_data_i];
    vector_t path = vector_from_strings(obj.fields[article_path_i], data->path_length);

		char* title = vector_getstr(&path, path.length-1);
		if (!title) title = "root";

    vector_t path_arg = vector_new(sizeof(template_args));
    
    vector_t urls = vector_new(sizeof(char*));
    vector_expand_strings(&path, &urls, "/wiki/", "/", "");

    vector_iterator iter = vector_iterate(&path);
    while (vector_next(&iter)) {
      char** sub_args = heap(sizeof(char*[2]));

      sub_args[0] = vector_getstr(&urls, iter.i-1);
      sub_args[1] = *(char**)iter.x;
      
      vector_pushcpy(&path_arg, &(template_args){.sub_args=sub_args});
    }

    switch (data->ty) {
			case article_img:
      case article_text: {
        vector_t contribs = {.data = obj.fields[article_contrib_i],
          .size = sizeof(uint64_t),
          .length = data->contributors};
        
        vector_t contribs_arg = vector_new(sizeof(template_args));
        vector_t contribs_strs = vector_new(sizeof(char*));

        vector_iterator iter = vector_iterate(&contribs);
        while (vector_next(&iter)) {
          filemap_partial_object list_user = filemap_get_idx(&session->ctx->user_id, *(uint64_t*)iter.x);
          if (!list_user.exists) continue;

          filemap_field uname = filemap_cpyfield(&session->ctx->user_fmap, &list_user, user_name_i);
          vector_pushcpy(&contribs_arg, &(template_args){.sub_args=heapcpy(sizeof(char**), &uname.val.data)});
          vector_pushcpy(&contribs_strs, &uname.val.data);
        }

				permissions_t perms = get_perms(session);

				vector_t url = flatten_url(&path);

				if (data->ty == article_img) {
					respond_template(session, 200, "article", title, 1, 1,
							perms & perms_edit_article, perms & perms_delete_article,
							&path_arg, &contribs_arg, title, NULL, url.data);
					vector_free(&url);

				} else {
					respond_template(session, 200, "article", title, 1, 0,
							perms & perms_edit_article, perms & perms_delete_article,
							&path_arg, &contribs_arg, title, obj.fields[article_html_i], url.data);
				}

        vector_free_strings(&contribs_strs);

        break;
      }

      case article_group: {
        vector_t item_strs = vector_new(sizeof(char*));
        vector_t items_arg = article_group_list(session->ctx, &obj, data, &item_strs);

        respond_template(session, 200, "article", title, 0,0,0,0, &path_arg, &items_arg, title, NULL, NULL);
        vector_free_strings(&item_strs);
        break;
      }

      default: {
				 //????
				respond_error(session, 422, "Cannot be displayed");
			}
    }

    filemap_object_free(&session->ctx->article_fmap, &obj);

    vector_free(&path); //references obj
    vector_free_strings(&urls);

	} else if (strcmp(base, "wikisrc")==0) {
		filemap_object obj;
		if (!route_article(session, req, &obj)) return;

    articledata_t* data = (articledata_t*)obj.fields[article_data_i];
    vector_t path = vector_from_strings(obj.fields[article_path_i], data->path_length);

		vector_t wpath = flatten_wikipath(&path);

    switch (data->ty) {
			case article_img: {
				cached* cache = ctx_fopen(session->ctx, wpath.data);

				respond(session, 200, cache->data, cache->len, (header[]){
					{"Content-Type", obj.fields[article_html_i]}}, 1);

				ctx_cache_done(session->ctx, cache, wpath.data);

				break;
			}
      case article_text: {
				cached* current = article_current(session->ctx, &wpath);
				respond(session, 200, current->data, current->len, (header[]){{"Content-Type", "text/plain"}}, 1);
				ctx_cache_done(session->ctx, current, wpath.data);
				break;
      }

      default: {
				respond_error(session, 422, "");
			}
    }

    filemap_object_free(&session->ctx->article_fmap, &obj);
		vector_free(&wpath);
    vector_free(&path);

	} else {
    resource* res = map_find(&session->ctx->resources,
                             vector_get(&req->path, req->path.length - 1));

    if (!res) {
      respond_error(session, 404, "Page not found");
      return;
    }

    respond(session, 200, res->content, res->len,
            (header[1]){{"Content-Type", res->mime}}, 1);
  }
}
