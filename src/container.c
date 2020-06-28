#define _GNU_SOURCE
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

typedef struct {
	int out[2];
	int err[2];
	char* ip;
	char* dir;
	char* app;
	char** argv;
} runctx_t;

int run(void* arg) {
	runctx_t* runctx = arg;
	char* dir = runctx->dir;
	char* app = runctx->app;

	close(runctx->out[0]);
	close(runctx->err[0]);

	close(1);
	close(2);

	dup2(runctx->out[1], 1);
	dup2(runctx->err[1], 2);

	//reset buffering
	setvbuf(stdout, NULL, _IOLBF, PIPE_BUF);
	setvbuf(stderr, NULL, _IOLBF, PIPE_BUF); //needed for formatting for some reason, lest program crashes?? buf needs to match pipe buf??

	fflush(stdout);
	fflush(stderr);

	//get last segment
	char* name = app + strlen(app)-1;
	while (name > app && *name != '/') name--;
	if (*name == '/') name++;

	//mount dependencies
	char* runpath = heapstr("%s/run", dir);
	char* apppath = heapstr("%s/%s", runpath, name);
	char* relpath = heapstr("./%s", name);

	char* libpath = heapstr("%s/lib", dir);

	char* usrpath = heapstr("%s/usr", dir);
	char* usrlibpath = heapstr("%s/lib", usrpath);
	char* varpath = heapstr("%s/var", dir);
	char* varlibpath = heapstr("%s/lib", varpath);

	char* binpath = heapstr("%s/bin", dir);
	char* sbinpath = heapstr("%s/sbin", dir);
	char* usrbinpath = heapstr("%s/bin", usrpath);

	unsigned long defflags = MS_NODEV | MS_NOSUID;
	unsigned long bindflags = defflags | MS_REC | MS_BIND;
	unsigned long remountflags = bindflags | MS_REMOUNT | MS_BIND | MS_RDONLY;

	umask(0000);

	printf("mount root & dir\n");
	if (mount(NULL, "/", NULL, MS_REC | MS_SLAVE, NULL)==-1) {
		errx(errno, "mount root failed");
	}

	mkdir(dir, 0555);
	chmod(dir, 0555);

	if (mount(dir, dir, NULL, MS_REC | MS_BIND, NULL)==-1) {
		errx(errno, "mount dir failed");
	}

	printf("mk run dir\n");
	mkdir(runpath, 0777);

	struct stat rundir_stat;
	if (stat(runpath, &rundir_stat)==-1 || !(rundir_stat.st_mode & S_IFDIR)) {
		errx(errno, "mkdir failed");
	}

	printf("mount app %s\n", apppath);

	FILE* mkapp = fopen(apppath, "w");
	fclose(mkapp);

	chmod(apppath, 0555);

	if (mount(app, apppath, NULL, bindflags, NULL)==-1) {
		errx(errno, "mount app failed\n");
	}

	if (runctx->ip) {
		char* netns = heapstr("%snetns", name);
		printf("setting ip (netns %s)\n", netns);

		int setnsfd = open(heapstr("/var/run/netns/%s", netns), O_RDONLY);

		if (setnsfd >= 0) {
			system(heapstr("ip netns delete %s", netns));
		}

		system(heapstr("ip netns add %s", netns));
		setnsfd = open(heapstr("/var/run/netns/%s", netns), O_RDONLY);
		
		system(heapstr("ip link add %sveth0 netns %s type veth peer name %sveth1", name, netns, name));

		//use consecutive ip for veth1
		char* lastdigit = runctx->ip + strlen(runctx->ip)-1;
		*lastdigit = ((*lastdigit-'0'+1) % 100)+'0';

		system(heapstr("ifconfig %sveth1 %s up", name, runctx->ip));

		if (setns(setnsfd, CLONE_NEWNET)==-1) {
			errx(errno, "setns failed\n");
		}

		//undo
		*lastdigit = ((*lastdigit-'0'-1) % 100)+'0';

		system(heapstr("ifconfig %sveth0 %s up", name, runctx->ip));
		system("ifconfig lo 127.0.0.1 up");
	}

	//sorry but i dont want to define functions for something already hardcoded

	printf("mount libs\n");
	mkdir(libpath, 0555);
	mount("/lib", libpath, NULL, bindflags, NULL);
	mount(NULL, libpath, NULL, remountflags, NULL);

	mkdir(usrpath, 0555);
	mkdir(usrlibpath, 0555);
	mount("/usr/lib", usrlibpath, NULL, bindflags, NULL);
	mount(NULL, usrlibpath, NULL, remountflags, NULL);

	mkdir(varpath, 0555);
	mkdir(varlibpath, 0555);
	mount("/var/lib", varlibpath, NULL, bindflags, NULL);
	mount(NULL, varlibpath, NULL, remountflags, NULL);

	printf("mount bins\n");
	mkdir(binpath, 0555);
	mount("/bin", binpath, NULL, bindflags, NULL);
	mount(NULL, binpath, NULL, remountflags, NULL);

	mkdir(sbinpath, 0555);
	mount("/sbin", sbinpath, NULL, bindflags, NULL);
	mount(NULL, sbinpath, NULL, remountflags, NULL);

	mkdir(usrbinpath, 0555);
	mount("/usr/bin", usrbinpath, NULL, bindflags, NULL);
	mount(NULL, usrbinpath, NULL, remountflags, NULL);

	char* procpath = heapstr("%s/proc", dir);

	printf("mount proc\n");
	mkdir(procpath, 0666);
	if (mount("proc", procpath, "proc", defflags, NULL)==-1) {
		errx(errno, "mount /proc failed");
	}

	printf("pivot root\n");
	chdir(dir);

	mkdir("root", 0666);
	if (syscall(SYS_pivot_root, ".", "root")==-1) {
		errx(errno, "pivot root failed\n");
	}

	if (chroot("/")==-1) {
		errx(errno, "chroot failed\n");
	}

	chdir("/");

	if (umount2("root", MNT_DETACH)==-1) {
		errx(errno, "umount failed\n");
	}

	rmdir("root");

	chdir("run");

	printf("unshare\n");
	unshare(CLONE_NEWUSER | CLONE_NEWNS); //unshare user namespace so mounts are readonly
	//proc then has capabilities but isnt (in) root

	//lock uids, which can only be written to once
	char* nomap = "0 0 0";

	FILE* f = fopen("/proc/1/uid_map", "w");
	fwrite(&nomap, strlen(nomap), 1, f);
	fclose(f);

	//lock gids too
	f = fopen("/proc/1/gid_map", "w");
	fwrite(&nomap, strlen(nomap), 1, f);
	fclose(f);

	printf("running\n");

	//app, args
	if (execvp(relpath, runctx->argv)==-1) {
		errx(errno, "exec failed\n");
	}

	return 0;
}

int main(int argc, char** argv) {
	if (argc < 4) {
		fprintf(stderr, "ye forgotten les argúmantis\n");
		return 1;
	}

	runctx_t runctx = {.dir=argv[1], .ip=argv[2], .app=argv[3], .argv=&argv[3]};
	if (strcmp(runctx.ip, "none")==0) runctx.ip = NULL;

	pipe2(runctx.out, O_NONBLOCK);
	pipe2(runctx.err, O_NONBLOCK);

	int newout = dup(1);
	int newerr = dup(2);

	int outlog = creat("./log.txt", 0777);
	int errlog = creat("./errlog.txt", 0777);

	dprintf(newout, "clone\n");

	//cmd c
	char* stack = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	//cmd v
	
	pid_t p = clone(&run, stack+4096, SIGCHLD | CLONE_NEWNS | CLONE_NEWPID, &runctx);
	dprintf(newout, "pid=%i\n", (int)p);

	close(runctx.out[1]);
	close(runctx.err[1]);

	struct pollfd pfd = {.fd=runctx.out[0], .events=POLLIN};
	struct pollfd pfd_err = {.fd=runctx.err[0], .events=POLLIN};

	char buf[PIPE_BUF];
	ssize_t len;

	while (waitpid(p, NULL, WNOHANG)==0) {
		if (poll(&pfd, 1, 500)>0) {
			while ((len = read(runctx.out[0], buf, PIPE_BUF)) && len>0) {
				write(newout, buf, len);
				write(outlog, buf, len);
			}
		}
		
		if (poll(&pfd_err, 1, 0)>0) {
			while ((len = read(runctx.err[0], buf, PIPE_BUF)) && len>0) {
				write(newerr, buf, len);
				write(errlog, buf, len);
			}
		}
	}

	dprintf(newout, "exited\n");

	close(outlog);
	close(errlog);
	
	return 0;
}
