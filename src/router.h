// Automatically generated header.

#pragma once
#include <openssl/evp.h>
#include <stdint.h>
#include <string.h>
#include "vector.h"
#include <openssl/rand.h>
#include <stdatomic.h>
#include <threads.h>
#include "hashtable.h"
#include "util.h"
#include "context.h"
void route(session_t* session, request* req);
