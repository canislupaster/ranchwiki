// Automatically generated header.

#pragma once
#include <sys/socket.h>
#include <sys/types.h>
#include <ctype.h>
#include <err.h>
#include "event2/bufferevent.h"
#include "hashtable.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "event2/event.h"
#include "event2/buffer.h"
#include "event2/listener.h"
#include "util.h"
#include "vector.h"
#include "reasonphrases.h"
#include "context.h"
void respond_html(session_t* session, int stat, char* content);
