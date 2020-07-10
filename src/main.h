// Automatically generated header.

#pragma once
#include <assert.h>
#include <err.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "hashtable.h"
#include "locktable.h"
#include "threads.h"
#include "tinydir.h"
#include "util.h"
#include "vector.h"
extern char* TEMPLATE_EXT;
#include "context.h"
void save_ctx(ctx_t* ctx);
void cleanup_callback(int fd, short what, void* arg);
void wcache_callback(int fd, short what, void* arg);
void interrupt_callback(int signal, short events, void* arg);
void sighandler(int sig, siginfo_t* info, void* arg);
int util_main(void* udata);
int main(int argc, char** argv);
