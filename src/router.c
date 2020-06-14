#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdatomic.h>
#include <stdint.h>
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
      filemap_cpyfield(&session->ctx->user_fmap, &session->user_ses->user, 2);

  if (!udata.exists) {
    return perms_none;
  } else {
    permissions_t perms = ((userdata_t*)udata.val.data)->perms;
    vector_free(&udata.val);
    return perms;
  }
}

void route(session_t* session, request* req) {
  if (req->path.length == 0) {

		if (session->user_ses->user.exists) {
			filemap_field uname = filemap_cpyfield(&session->ctx->user_fmap, &session->user_ses->user, 0);
			respond_template(session, 200, "home", "ranch", 1, get_perms(session) & perms_create_article, uname.val.data, "");

			vector_free(&uname.val);
		} else {
			respond_template(session, 200, "home", "ranch", 0, 0, "", "");
		}
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

    user_t user = filemap_push(
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

    userdata_t* data = (userdata_t*)user.fields[2];

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

    session->user_ses->user = user_ref;

    user_session** old_ses =
        map_find(&session->ctx->user_sessions_by_idx, &user_ref.index);
    if (old_ses) (*old_ses)->user.exists = 0;

    map_insertcpy(&session->ctx->user_sessions_by_idx, &user_ref.index, &session->user_ses);

    mtx_unlock(&session->user_ses->lock);

    respond_redirect(session, "/account");
    filemap_object_free(&session->ctx->user_fmap, &user);
    vector_free(&params);

  } else if (strcmp(base, "account") == 0 && req->method == GET) {
    char** arg = vector_get(&req->path, 1);

    //one step, no need for lock

    user_t user;
    if (!arg) {
      user = filemap_cpy(&session->ctx->user_fmap, &session->user_ses->user);
      if (!user.exists) {
        respond_template(session, 200, "login", "Login", 1,
                         "You aren't logged in");
        return;
      }

      respond_template(session, 200, "account", user.fields[0], 0, "",
                       user.fields[0], user.fields[1], user.fields[3]);

    } else {
      user =
          filemap_findcpy(&session->ctx->user_by_name,
                          *arg, strlen(*arg) + 1);

      if (!user.exists) {
        respond_error(session, 404, "User not found");
        return;
      }

      respond_template(session, 200, "profile", user.fields[0],
                       get_perms(session) & perms_admin, user.fields[0],
                       user.fields[3]);
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

      user_session** user_ses = map_find(&session->ctx->user_sessions_by_idx, &list_user.index);
      if (user_ses) mtx_lock(&(*user_ses)->lock);

      user_t usee = filemap_cpy(&session->ctx->user_fmap, &list_user);

      char* perms_str = vector_getstr(&params, 0);
      uint8_t usee_perms = (uint8_t)atoi(perms_str);

      userdata_t* data = (userdata_t*)usee.fields[2];
      data->perms = usee_perms;

      update_user(session->ctx, &list_user, &usee, usee.fields, usee.lengths);

      if (user_ses) mtx_unlock(&(*user_ses)->lock);

      respond_template(session, 200, "profile", usee.fields[0], 1,
                       usee.fields[0], usee.fields[3]);

      filemap_object_free(&session->ctx->user_fmap, &usee);
      vector_free(&params);

      return;
    }

    mtx_lock(&session->user_ses->lock);

    user_t user =
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
      respond_template(session, 200, "account", user.fields[0], 1, err,
                       user.fields[0], user.fields[1], user.fields[3]);
      
      filemap_object_free(&session->ctx->user_fmap, &user);
      mtx_unlock(&session->user_ses->lock);
      vector_free(&params);
      return;
    }

    int name_change = strcmp(user.fields[0], username) != 0,
        email_change = strcmp(user.fields[1], email) != 0,
        bio_change = strcmp(user.fields[3], bio) != 0;

    if (!name_change && !email_change && !bio_change) {
      respond_template(session, 200, "account", user.fields[0], 0, "",
                       user.fields[0], user.fields[1], user.fields[3]);
      
      filemap_object_free(&session->ctx->user_fmap, &user);
      mtx_unlock(&session->user_ses->lock);
      vector_free(&params);
      return;
    }

    filemap_partial_object name_user =
        filemap_find(&session->ctx->user_by_name,
                     username, strlen(username) + 1);

    if (name_change && name_user.exists) {
      respond_template(session, 200, "account", user.fields[0], 1,
                       "Username already taken", user.fields[0], user.fields[1],
                       user.fields[3]);
      filemap_object_free(&session->ctx->user_fmap, &user);
      mtx_unlock(&session->user_ses->lock);
      vector_free(&params);
      return;
    }

    filemap_partial_object email_user =
        filemap_find(&session->ctx->user_by_email,
                     email, strlen(email) + 1);

    if (email_change && email_user.exists) {
      respond_template(session, 200, "account", user.fields[0], 1,
                       "Email is already in use", username, email, bio);
      filemap_object_free(&session->ctx->user_fmap, &user);
      mtx_unlock(&session->user_ses->lock);
      vector_free(&params);
      return;
    }

    // delete old indices
    if (name_change) {
      filemap_partial_object prev_name_user =
          filemap_find(&session->ctx->user_by_name,
                       user.fields[0], strlen(user.fields[0]) + 1);
      filemap_remove(&session->ctx->user_by_name, &prev_name_user);
    }

    if (email_change) {
      filemap_partial_object prev_email_user =
          filemap_find(&session->ctx->user_by_email,
                       user.fields[1], strlen(user.fields[1]) + 1);
      filemap_remove(&session->ctx->user_by_email, &prev_email_user);
    }

    // insert new user and update (new) indices
    if (name_change || email_change || bio_change) {
      user_t new = update_user_session(session, &user,
                          (char*[]){username, email, user.fields[2], bio},
                          (uint64_t[]){strlen(username) + 1, strlen(email) + 1,
                                       user.lengths[2], strlen(bio) + 1});

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

    user_t user =
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
      respond_template(session, 200, "account", user.fields[0], 1, passerr,
                       user.fields[0], user.fields[1], user.fields[3]);

      filemap_object_free(&session->ctx->user_fmap, &user);
      mtx_unlock(&session->user_ses->lock);
      vector_free(&params);
      return;
    }

    userdata_t* data = (userdata_t*)user.fields[2];

    EVP_DigestInit_ex(session->ctx->digest_ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(session->ctx->digest_ctx, password, strlen(password));
    EVP_DigestUpdate(session->ctx->digest_ctx, &data->salt, 4);

    EVP_DigestFinal_ex(session->ctx->digest_ctx, data->password_hash, NULL);

    // no length changes, refer to old data and updated userdata_t
    update_user_session(session, &user, user.fields, user.lengths);

    respond_template(session, 200, "account", user.fields[0], 1,
                     "Changed password successfully", user.fields[0],
                     user.fields[1], user.fields[3]);
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

    vector_t flattened = vector_new(1);
    vector_flatten_strings(&path, &flattened, "\0", 1);
    vector_pushcpy(&flattened, "\0");

    filemap_partial_object idx = filemap_find(&session->ctx->article_by_name,
                                              flattened.data, flattened.length);

    if (idx.exists) {
      respond_template(session, 200, "new", "New article", 1, 1,
                       "Article with that path already exists",
                       path_str, content);

      mtx_unlock(&session->user_ses->lock);
      vector_free_strings(&path);
      vector_free(&flattened);
      vector_free(&params);
      return;
    }

    articledata_t data = {
        .path_length = path.length, .ty = article_text, .referenced_by = 1, .contributors=1};

    unsigned long key_len = flattened.length;

    // insert blank, then update groups and finally insert real thing
    filemap_partial_object article =
        filemap_add(&session->ctx->article_id, NULL);
    filemap_partial_object item = article;
    filemap_partial_object last_item;

    vector_iterator iter = vector_iterate(&path);
    iter.rev = 1;

    while (vector_next(&iter)) {
      char last = iter.i == 1;

      unsigned long segment_len = strlen(*(char**)iter.x);

      key_len -= segment_len + 1;  //\0 is a delimeter

      lock_article(session->ctx, flattened.data, key_len);

      filemap_partial_object group_ref =
          filemap_find(&session->ctx->article_by_name, flattened.data, key_len);

      filemap_partial_object new_group;

      if (!group_ref.exists) {
        articledata_t groupdata = {.path_length = path.length - iter.i,
                                   .ty = article_group,
                                   .items = 1};

        filemap_object obj = filemap_push(
            &session->ctx->article_fmap,

            (char*[]){(char*)&groupdata, (char*)&item.index, flattened.data,
              last ? (char*)&session->user_ses->user.index : NULL},

            (uint64_t[]){sizeof(articledata_t), 8, key_len, last ? 8 : 0});

        new_group = filemap_add(&session->ctx->article_id, &obj);

        filemap_object obj_ref = filemap_index_obj(&obj, &new_group);
        filemap_insert(&session->ctx->article_by_name, &obj_ref);

      } else {
        new_group = filemap_deref(&session->ctx->article_id, &group_ref);

        filemap_object obj =
            filemap_cpy(&session->ctx->article_fmap, &new_group);

        articledata_t* groupdata = (articledata_t*)obj.fields[0];

        vector_t items = {.data = obj.fields[1],
                          .size = sizeof(uint64_t),
                          .length = groupdata->items};

        void* exists = vector_search(&items, &item.index);
        if (!exists || last) {
          filemap_delete_object(&session->ctx->article_fmap, &obj);

          if (!exists) {
            groupdata->items++;
            obj.lengths[1] += 8;

            vector_pushcpy(&items, &item.index);
          } 

          if (last) {
            vector_t contribs = {.data = obj.fields[3],
                            .size = sizeof(uint64_t),
                            .length = groupdata->contributors};

            groupdata->contributors++;
            obj.lengths[3] += 8;

            vector_pushcpy(&contribs, &session->user_ses->user.index);
          }

          filemap_object new_obj = filemap_push(&session->ctx->article_fmap,
                                                obj.fields, obj.lengths);
          filemap_list_update(&session->ctx->article_id, &new_group,
                              &new_obj);
        }

        filemap_object_free(&session->ctx->article_fmap, &obj);
      }

      unlock_article(session->ctx, flattened.data, key_len);

      if (last) last_item = new_group;
      item = new_group;
    }

    filemap_object text = filemap_push(
        &session->ctx->article_fmap,
        (char*[]){(char*)&data, (char*)&last_item.index, flattened.data,
          (char*)&session->user_ses->user.index},
        (uint64_t[]){sizeof(articledata_t), 8, flattened.length, 8});

    filemap_list_update(&session->ctx->article_id, &article, &text);

    filemap_object text_ref = filemap_index_obj(&text, &article);

    filemap_insert(&session->ctx->article_by_name, &text_ref);

    // display/file path
    vector_t out_path = vector_new(1);

    iter = vector_iterate(&path);

    while (vector_next(&iter)) {
      char* segment = *(char**)iter.x;
      vector_stockcpy(&out_path, strlen(segment), segment);

      if (iter.i != path.length) {
        vector_pushcpy(&out_path, "/");
        vector_pushcpy(&out_path, "\0");

        group_new(out_path.data);

        // create diff at filename
      } else {
        diff_t d = {.additions = vector_new(sizeof(add_t)),
                    .deletions = vector_new(sizeof(del_t))};
        vector_pushcpy(&d.additions, &(add_t){.pos = 0, .txt = content});

        vector_pushcpy(&out_path, "\0");

        text_t txt = txt_new(out_path.data);
        add_diff(&txt, &d, content);

        vector_free(&d.additions);
        vector_free(&d.deletions);
      }

      vector_pop(&out_path);
    }

    mtx_unlock(&session->user_ses->lock);

    vector_insert_manycpy(&out_path, 0, strlen("wiki/"), "wiki/");
    respond_redirect(session, out_path.data);
    vector_free(&out_path);

    vector_free_strings(&path);
    vector_free(&flattened);
    vector_free(&params);

  } else if (strcmp(base, "wiki") == 0) {
    vector_remove(&req->path, 0);

    vector_iterator iter = vector_iterate(&req->path);
    while (vector_next(&iter)) {
      //not very sanitary... TODO: dont implicit free
      *(char**)iter.x = percent_decode(*(char**)iter.x);
    }

    vector_t flattened = vector_new(1);
    vector_flatten_strings(&req->path, &flattened, "\0", 1);
    vector_pushcpy(&flattened, "\0");

    filemap_partial_object article_ref = filemap_find(
        &session->ctx->article_by_name,
        flattened.data, flattened.length);

    filemap_object obj = filemap_cpyref(&session->ctx->article_fmap, &article_ref);

    if (!obj.exists) {
      vector_t path_formatted = vector_new(1);
      vector_flatten_strings(&req->path, &path_formatted, "/", 1);
      vector_pushcpy(&path_formatted, "\0");

      int has_perm = get_perms(session) & perms_create_article;
      if (has_perm) {
        respond_template(session, 200, "new", "New article", 1, 1,
                         "article does not exist", path_formatted.data, "");
      } else {
        char* err = heapstr("%s does not exist", path_formatted.data);
        respond_error(session, 404, err);
       }

      vector_free(&flattened);
      vector_free(&path_formatted);

      return;
    }

    articledata_t* data = (articledata_t*)obj.fields[0];
    vector_t path = vector_from_strings(obj.fields[2], data->path_length);
    char* title = vector_getstr(&path, path.length-1);

    vector_t path_arg = vector_new(sizeof(template_args));
    
    vector_t urls = vector_new(sizeof(char*));
    vector_expand_strings(&path, &urls, "/wiki/", "/", "");

    iter = vector_iterate(&path);
    while (vector_next(&iter)) {
      char** sub_args = heap(sizeof(char*[2]));

      sub_args[0] = vector_getstr(&urls, iter.i-1);
      sub_args[1] = heapcpystr(*(char**)iter.x);
      
      vector_pushcpy(&path_arg, &(template_args){.sub_args=sub_args});
    }

    switch (data->ty) {
      case article_text: {
        vector_t filepath = vector_new(1);
        vector_flatten_strings(&path, &filepath, "/", 1);

        text_t txt = txt_new(filepath.data);
        read_txt(&txt, 0);
        
        vector_t contribs = {.data = obj.fields[3],
          .size = sizeof(uint64_t),
          .length = data->contributors};
        
        vector_t contribs_arg = vector_new(sizeof(template_args));
        vector_t contribs_strs = vector_new(sizeof(char*));

        vector_iterator iter = vector_iterate(&contribs);
        while (vector_next(&iter)) {
          filemap_partial_object list_user = filemap_get_idx(&session->ctx->user_id, *(uint64_t*)iter.x);
          if (!list_user.exists) continue;

          filemap_field uname = filemap_cpyfield(&session->ctx->user_fmap, &list_user, 0);
          vector_pushcpy(&contribs_arg, &(template_args){.sub_args=heapcpy(sizeof(char**), &uname.val.data)});
          vector_pushcpy(&contribs_strs, &uname.val.data);
        }

        respond_template(session, 200, "article", title, 1, &path_arg, &contribs_arg, title, txt.current);
        vector_free_strings(&contribs_strs);

        txt_free(&txt);
        break;
      }

      case article_group: {
        vector_t items = {.data = obj.fields[1],
          .size = sizeof(uint64_t),
          .length = data->items};

        vector_t items_arg = vector_new(sizeof(template_args));
        vector_t item_strs = vector_new(sizeof(char*));
        
        vector_iterator iter = vector_iterate(&items);
        while (vector_next(&iter)) {
          filemap_partial_object list_item = filemap_get_idx(&session->ctx->article_id, *(uint64_t*)iter.x);
          if (!list_item.exists)  continue;

          articledata_t* item_data =
            (articledata_t*)filemap_cpyfield(&session->ctx->user_fmap, &list_item, 0).val.data;
          vector_t item_pathdata = filemap_cpyfield(&session->ctx->user_fmap, &list_item, 2).val;

          vector_t item_path = vector_from_strings(item_pathdata.data, item_data->path_length);
          char* end = heapcpystr(vector_getstr(&item_path, item_path.length-1));

          vector_pushcpy(&items_arg, &(template_args){.sub_args=heapcpy(sizeof(char**), &end)});
          vector_pushcpy(&item_strs, &end);
          
          drop(item_data);
          vector_free(&item_pathdata);
          vector_free(&item_path);
        }

        respond_template(session, 200, "article", title, 0, &path_arg, NULL, &items_arg, title, NULL);
        vector_free_strings(&item_strs);
        break;
      }

      default:;
    }

    filemap_object_free(&session->ctx->article_fmap, &obj);

    vector_free(&flattened);
    vector_free(&path); //references obj
    vector_free(&urls); //referenced by template

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
