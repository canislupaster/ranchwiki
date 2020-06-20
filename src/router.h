// Automatically generated header.

#pragma once
#include <openssl/evp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>
#include "hashtable.h"
#include "vector.h"
#include <openssl/rand.h>
#include "util.h"
#include "context.h"
void route(session_t* session, request* req);
