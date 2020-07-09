// Automatically generated header.

#pragma once
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>
#include "hashtable.h"
#include "locktable.h"
#include "util.h"
#include "vector.h"
vector_t
flatten_wikipath(vector_t* path);
