
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
#include <sys/mount.h>
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <syscall.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/fs.h>


#define SYSBLKDIR "/sys/class/block"
static void walk(void)
{
	DIR *d;
	FILE *f;
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

		snprintf(p, PATH_MAX, "%s/%s/dev", SYSBLKDIR, entry->d_name);
		f = fopen(p, "r");
		if (!f)
			continue;
		if (fscanf(f, "%d:%d", &maj, &min) < 2) {
			fclose(f);
			fprintf(stderr, "Error reading %s\n", p);
			continue;
		}
		fclose(f);

		snprintf(p, PATH_MAX, "%s/%s", SYSBLKDIR, entry->d_name);
		len = readlink(p, path, PATH_MAX - 1);
		if (len != -1)
			path[len] = '\0';

		snprintf(p, PATH_MAX, "/dev/%s", entry->d_name);
		fprintf(stderr, "Creating %s (b: %d, %d)\n", p, maj, min);
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

		if (!mount(p, "/newroot", "btrfs", MS_MGC_VAL | MS_RDONLY, NULL))
			goto havemount;
		fprintf(stderr, "failed to mount as btrfs: %s\n", strerror(errno));
		if (!mount(p, "/newroot", "ext3", MS_MGC_VAL | MS_RDONLY, NULL))
			goto havemount;
		fprintf(stderr, "failed to mount as ext3: %s\n", strerror(errno));
		if (!mount(p, "/newroot", "ext2", MS_MGC_VAL | MS_RDONLY, NULL))
			goto havemount;
		fprintf(stderr, "failed to mount as ext2: %s\n", strerror(errno));
		if (!mount(p, "/newroot", "vfat", MS_MGC_VAL | MS_RDONLY, NULL))
			goto havemount;
		fprintf(stderr, "failed to mount as vfat: %s\n", strerror(errno));

		continue;

havemount:
		fprintf(stderr, "Mounted %s\n", p);

		if (access("/newroot/sbin/init", X_OK | R_OK)) {
			umount("/newroot");
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
	int ret;

	/* mount proc/sys/dev etc */
	if (mount("sysfs", "/sys", "sysfs", MS_MGC_VAL, NULL))
		fprintf(stderr, "Failed to mount /sys\n");
	if (mount("proc", "/proc/", "proc", MS_MGC_VAL, NULL))
		fprintf(stderr, "Failed to mount /proc\n");
	if (mount("tmpfs", "/dev/", "tmpfs", MS_MGC_VAL, NULL))
		fprintf(stderr, "Failed to mount /dev\n");

	/* basic device nodes we'll need */
	if ((mknod("/dev/console", S_IFCHR, makedev(5, 1))) ||
	    (mknod("/dev/null", S_IFCHR, makedev(3, 1))))
		fprintf(stderr, "Failed to create device nodes in /dev\n");

	/* symlinks, just in case */
	if ((symlink("/proc/self/fd", "/dev/fd")) ||
	    (symlink("fd/0", "/dev/stdin")) ||
	    (symlink("fd/1", "/dev/stdout")) ||
	    (symlink("fd/2", "/dev/stderr")))
		fprintf(stderr, "Failed to write symlinks in /dev\n");

	/* walk sysfs and create block device nodes we need */
	walk();

	/* try to mount mmc device nodes */
	if (try("mmc"))
		goto haveroot;

	/* try to mount sd device nodes */
	if (try("sd"))
		goto haveroot;

	fprintf(stderr, "Failed to find a rootfs\n");
	exit (1);

haveroot:
	/* pre-root change cleanup */
	if (umount("/sys"))
		fprintf(stderr, "Failed to umount /sys\n");
	if (umount("/dev"))
		fprintf(stderr, "Failed to umount /dev\n");
	if (umount("/proc"))
		fprintf(stderr, "Failed to umount /proc\n");

	/* root is mounted, prep and move root */
	if (chdir("/newroot"))
		fprintf(stderr, "Failed to chdir /newroot\n");
	if (mount(".", "/", NULL, MS_MOVE, NULL))
		fprintf(stderr, "Unable to mount --move /newroot\n");
	if (chroot("."))
		fprintf(stderr, "Unable to chroot to /newroot\n");
	if (chdir("/"))
		fprintf(stderr, "Failed to chdir /\n");

	/* exec the new init */
	execv("/sbin/init", argv);

	/* this should never be reached */
	fprintf(stderr, "Failed to exec /sbin/init\n");
	exit (1);
}
