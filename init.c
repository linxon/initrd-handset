
/*
 * (C) Copyright 2010 Intel Corporation
 *
 * Author: Auke Kok <auke-jan.h.kok@intel.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <syscall.h>
#include <fcntl.h>


#define SYSBLKDIR "/sys/class/block"
static void walk(void)
{
	DIR *d;
	struct dirent *entry;
	char path[PATH_MAX];
	ssize_t len;
	unsigned int maj;
	unsigned int min;

	d = opendir(SYSBLKDIR);
	if (!d)
		return;

	while ((entry = readdir(d))) {
		char p[PATH_MAX];

		if ((strcmp(".", entry->d_name) == 0) ||
		    (strcmp("..", entry->d_name) == 0))
			continue;

		sscanf(entry->d_name, "%d:%d", &maj, &min);

		snprintf(p, PATH_MAX, "%s/%s", SYSBLKDIR, entry->d_name);
		len = readlink(p, path, PATH_MAX - 1);
		if (len != -1)
			path[len] = '\0';

		snprintf(p, PATH_MAX, "/dev/%s", entry->d_name);
		mknod(p, S_IFBLK, makedev(maj, min));

	}

	closedir(d);
}

static int try(const char *match)
{
	DIR *d;
	struct dirent *entry;
	char path[PATH_MAX];
	ssize_t len;

	d = opendir("/dev");
	if (!d)
		return 0;

	while ((entry = readdir(d))) {
		char p[PATH_MAX];

		if (!strstr(entry->d_name, match))
			continue;

		snprintf(p, PATH_MAX, "/dev/%s", entry->d_name);
		fprintf(stderr, "Trying %s\n", p);

		if (!mount(p, "/mnt", "btrfs", 0, "ro"))
			goto havemount;
		if (!mount(p, "/mnt", "ext3", 0, "ro"))
			goto havemount;
		if (!mount(p, "/mnt", "ext2", 0, "ro"))
			goto havemount;
		if (!mount(p, "/mnt", "vfat", 0, "ro"))
			goto havemount;

		return 1;

havemount:
		fprintf(stderr, "Mounted %s\n", p);

		if (access("/mnt/sbin/init", X_OK | R_OK)) {
			umount("/mnt");
			continue;
		}

		fprintf(stderr, "Found /sbin/init on %s\n", p);

		closedir(d);
		return 1;
	}

	closedir(d);
	return 0;
}


int main(int argc, char **argv)
{
	/* mount proc/sys/dev etc */
	if (!mount("sysfs", "/sys", "sysfs", 0, NULL))
		fprintf(stderr, "Failed to mount /sys\n");
	if (!mount("proc", "/dev/", "procfs", 0, NULL))
		fprintf(stderr, "Failed to mount /proc\n");
	if (!mount("tmpfs", "/dev/", "tmpfs", 0, NULL))
		fprintf(stderr, "Failed to mount /dev\n");

	/* basic device nodes we'll need */
	if ((!mknod("/dev/console", S_IFCHR, makedev(5, 1))) ||
	    (!mknod("/dev/null", S_IFCHR, makedev(3, 1))))
		fprintf(stderr, "Failed to create device nodes in /dev\n");


	/* symlinks, just in case */
	if ((!symlink("/proc/self/fd", "/dev/fd")) ||
	    (!symlink("fd/0", "/dev/stdin")) ||
	    (!symlink("fd/1", "/dev/stdout")) ||
	    (!symlink("fd/2", "/dev/stderr")))
		fprintf(stderr, "Failed to write symlinks in /dev\n");

	/* walk sysfs and create block device nodes we need */
	int u = umask(0000);
	walk();
	umask(u);

	/* try to mount mmc device nodes */
	if (try("mmc"))
		goto haveroot;

	/* try to mount sd device nodes */
	if (try("sd"))
		goto haveroot;

	fprintf(stderr, "Failed to find a rootfs\n");
	exit (1);

haveroot:
	exit (1);
	/* root is mounted, prep and pivot_root */

	if (!chdir("/"))
		fprintf(stderr, "Failed to chdir /\n");
	syscall(__NR_pivot_root, "/mnt", "/mnt"); //FIXME: check dst
	if (!chdir("/"))
		fprintf(stderr, "Failed to chdir /\n");

	/* exec the new init */
	execv("/sbin/init", argv);

	/* this should never be reached */
	fprintf(stderr, "Failed to exec /sbin/init\n");
	exit (1);
}
