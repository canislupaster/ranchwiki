#include <unistd.h>
#include <sys/syscall.h>
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

	unshare(CLONE_NEWNS | CLONE_NEWPID);

	mount(dir, dir, MS_REC | MS_BIND | MS_SLAVE);

	//mount dependencies (??????)
	char* apppath = heapstr("%s/%s", dir, app);
	char* libpath = heapstr("%s/lib", dir);
	char* usrpath = heapstr("%s/usr", dir);
	char* usrlibpath = heapstr("%s/lib", usrpath);
	char* varpath = heapstr("%s/var", dir);
	char* varlibpath = heapstr("%s/lib", varpath);

	mkdir(apppath, 555);
	mount(app, apppath, MS_REC | MS_BIND | MS_READONLY);

	mkdir(libpath, 555);
	mount("/lib", libpath, MS_REC | MS_BIND | MS_READONLY);

	mkdir(usrpath);
	mkdir(usrlibpath, 555);
	mount("/usr/lib", libpath, MS_REC | MS_BIND | MS_READONLY);

	mkdir(varpath);
	mkdir(varlibpath, 555);
	mount("/var/lib", varlibpath, MS_REC | MS_BIND | MS_READONLY);

	chroot(dir);
	mkdir("root", 666);
	syscall(SYS_pivot_root, ".", "root");
	umount2(".", MNT_DETACH);
	rmdir("root");

	execvp(app, &argv[2]);

	return 0;
}
