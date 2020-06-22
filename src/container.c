#include <unistd.h>

#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mount.h>

#include <sched.h>

#include "util.h"

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "ye forgotten les argúmantis");
		return 1;
	}

	char* dir = argv[0];
	char* app = argv[1];

	unshare(CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUSER);

	mount(dir, dir, MS_REC | MS_BIND | MS_SLAVE);

	//mount dependencies (??????)
	char* apppath = heapstr("%s/%s", dir, app);
	char* libpath = heapstr("%s/lib", dir);
	char* usrpath = heapstr("%s/usr", dir);
	char* usrlibpath = heapstr("%s/lib", usrpath);
	char* varpath = heapstr("%s/var", dir);
	char* varlibpath = heapstr("%s/lib", varpath);
	char* binpath = heapstr("%s/bin", dir);
	char* sbinpath = heapstr("%s/sbin", dir);
	char* usrbinpath = heapstr("%s/bin", usrpath);
	char* procpath = heapstr("%s/proc", dir);

	unsigned long defflags = MS_NODEV | MS_NOSUID;
	unsigned long bindflags = defflags | MS_REC | MS_BIND | MS_READONLY;

	mkdir(apppath, 555);
	mount(app, apppath, NULL, bindflags, NULL);

	mkdir(libpath, 555);
	mount("/lib", libpath, NULL, bindflags, NULL);

	mkdir(usrpath, 555);
	mkdir(usrlibpath, 555);
	mount("/usr/lib", libpath, NULL, bindflags, NULL);

	mkdir(varpath, 555);
	mkdir(varlibpath, 555);
	mount("/var/lib", varlibpath, NULL, bindflags, NULL);

	mkdir(binpath, 555);
	mount("/bin", binpath, NULL, bindflags, NULL);

	mkdir(sbinpath, 555);
	mount("/sbin", sbinpath, NULL, bindflags, NULL);

	mkdir(usrbinpath, 555);
	mount("/bin", usrbinpath, NULL, bindflags, NULL);

	mkdir(procpath, 666);
	mount("proc", procpath, "proc", defflags, NULL);

	chroot(dir);

	mkdir("root", 666);
	syscall(SYS_pivot_root, ".", "root");

	chdir("/");
	umount2("root", MNT_DETACH);
	rmdir("root");

	mkdir("run", 777);
	chdir("run");

	execvp(app, &argv[2]);

	return 0;
}
