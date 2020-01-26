// Automatically generated header.

#pragma once
#include <sys/socket.h>
#include <string.h>
#include "event2/event.h"
#include "event2/buffer.h"
#include "vector.h"
#include "hashtable.h"
#include <sys/types.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <err.h>
#include "event2/listener.h"
#include "event2/bufferevent.h"
#include "util.h"
#include "reasonphrases.h"
#include "context.h"
void respond(session_t* session, int stat, char* content, header* headers, int headers_len);
void respond_html(session_t* session, int stat, char* content);
void start_listen(ctx_t* ctx, const char *port);
