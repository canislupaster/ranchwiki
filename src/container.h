// Automatically generated header.

#pragma once
#include <unistd.h>
#include <stdint.h>
#include <limits.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sched.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <err.h>
#include <errno.h>
#include "util.h"
#define _GNU_SOURCE
typedef struct {
	int out[2];
	int err[2];
	char* ip;
	char* dir;
	char* app;
	char** argv;
} runctx_t;
int run(void* arg);
int main(int argc, char** argv);
