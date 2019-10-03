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

/*
 * This init replacement for initrds's aims to only provide basic
 * functionality to:
 *
 * - automatically create block device nodes that are visible to the
 *   running kernel (by reading sysfs)
 * - boot the most logical root filesystem based on the rule
 *      "removable devices before non-removable"
 *
 *   The initrd parses sysfs and finds all block devices and creates
 * proper /dev/ nodes for each likely block device.
 *   It adds all block devices to a simple array and determines if
 * the device is removable or not.
 *   If then goes over the array in reverse trying first to find a
 * proper rootfs on all devices that are identified as removable.
 *   If it doesn't find a rootfs it will try nonremovable devices in the
 * same reverse order.
 *   There is no defined order between sdN nodes and mmcblkN order.
 *   The initrd will keep retrying until a valid rootfs is found.
 *   Inserting a new block device should work: each iteration existing
 * device nodes are removed and remade to assure new block nodes will work.
 *
 * compile with -DDEBUG=1 for extensive dumping of debug info to stderr.
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
#include <ctype.h>
#include <sys/sysmacros.h>

#define SYSBLKDIR "/sys/class/block"

#define MAX_DEVICES 32

#ifndef DEBUG
#define debug(format, args...) ((void)0)
#else
#include <stdarg.h>
void debug(char *format, ...) {
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}
#endif

struct device {
	char node[32];
	int removable;
};

struct device devices[MAX_DEVICES];
int devcount = 0;

static void devadd(const char *name)
{
	char buf[32];
	char parent[32];
	char p[PATH_MAX];
	FILE *f;
	int l;

	if (devcount == MAX_DEVICES) {
		fprintf(stderr, "initrd: too many device nodes (%d)\n",
			MAX_DEVICES);
		return;
	}

	debug("devadd: name=%s\n", name);

	strncpy(devices[devcount].node, name, 31);
	devices[devcount].removable = 0;

	/* let's assume we're looking at "sda" */
	strncpy(parent, name, 31);

	/* strip the numeric part off the right */
	strncpy(buf, name, 31);
	l = strlen(buf);
	while (isdigit(buf[l-1])) {
		buf[l-1] = '\0';
		l--;
	}

	/* sda12 -> sda ? */
	sprintf(p, "/dev/%s", buf);
	if (access(p, R_OK) == 0)
		strncpy(parent, buf, 31);

	/* mmcblk12p14 -> mmcblk12 ? */
	if (buf[l-1] == 'p') {
		buf[l-1] = '\0';
		sprintf(p, "/dev/%s", buf);
		if (access(p, R_OK) == 0)
			strncpy(parent, buf, 31);
	}

	debug("devadd: parent=%s\n", parent);

	/* see if this is a removable device - by looking at the parent
	 * device node sysfs files */
	sprintf(p, "%s/%s/device/type", SYSBLKDIR, parent);
	if (!access(p, R_OK)) {
		char s[32];
		s[0] = '\0';

		f = fopen(p, "r");
		if (f) {
			(void) fscanf(f, "%s", &s);
			fclose(f);
		}
		/* MMC controllers see removable SD cards as type "SD" and
		 * onboard MMC flash as type "MMC" */
		if (!strcmp(s, "SD"))
			devices[devcount].removable = 1;
	} else {
		sprintf(p, "%s/%s/removable", SYSBLKDIR, parent);
		if (!access(p, R_OK)) {
			char s[32];
			s[0] = '\0';
			f = fopen(p, "r");
			if (f) {
				(void) fscanf(f, "%s", &s);
				fclose(f);
			}
			/* for non-MMC controllers we can just read the
			 * removable file content */
			if (!strcmp(s, "1"))
				devices[devcount].removable = 1;
		}
	}

	debug("devadd: removable=%d\n", devices[devcount].removable);

	/* done, inc count */
	devcount++;
}

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

		debug("walk: entry=%s\n", entry->d_name);

		if ((strcmp(".", entry->d_name) == 0) ||
		    (strcmp("..", entry->d_name) == 0))
			continue;

		if (strstr(entry->d_name, "loop"))
			continue;

		snprintf(p, PATH_MAX, "%s/%s/dev", SYSBLKDIR, entry->d_name);
		f = fopen(p, "r");
		if (!f)
			continue;
		if (fscanf(f, "%d:%d", &maj, &min) < 2) {
			fclose(f);
			fprintf(stderr, "initrd: Error reading %s\n", p);
			continue;
		}
		fclose(f);

		debug("walk: dev(maj,min)=(%d,%d)\n", maj, min);

		snprintf(p, PATH_MAX, "%s/%s", SYSBLKDIR, entry->d_name);
		len = readlink(p, path, PATH_MAX - 1);
		if (len != -1)
			path[len] = '\0';

		snprintf(p, PATH_MAX, "/dev/%s", entry->d_name);
		/* remove any existing nodes - just to be sure we do not
		 * get hit with a retry where the numbers change */
		if (access(p, W_OK) == 0)
			unlink(p);
		if (mknod(p, S_IFBLK, makedev(maj, min)))
			fprintf(stderr, "initrd: Unable to create %s\n", p);

		/* add this device to our list of known block devices */
		devadd(entry->d_name);
	}

	closedir(d);
}

static int domount(const char *path, const char *fstype)
{
	debug("domount: attempting to mount %s as %s\n", path, fstype);
	if (!mount(path, "/newroot", fstype,  MS_MGC_VAL | MS_RDONLY, NULL)) {
		fprintf(stderr, "initrd: mounted %s as %s\n", path, fstype);
		return 0;
	}
	return -1;
}

static int trymount(const char *node)
{
	char path[PATH_MAX];

	debug("trymount: attempting to mount %s\n", node);

	snprintf(path, PATH_MAX, "/dev/%s", node);

	if (!domount(path, "btrfs"))
		goto havemount;
	if (!domount(path, "ext3"))
		goto havemount;
	if (!domount(path, "ext2"))
		goto havemount;
	if (!domount(path, "vfat"))
		goto havemount;

	debug("trymount: unable to mount %s as %s\n", node, path);

	return -1;

havemount:
	if (access("/newroot/sbin/init", X_OK)) {
		debug("trymount: mounted %s but no /sbin/init found on device\n", node);
		umount("/newroot");
		return -1;
	}
	return 0;
}

static int try(int removable)
{
	int n;

	debug("try: looking at %d %s devices:\n", devcount, removable ? "REMOVABLE" : "NONREMOVABLE");

	for (n = devcount - 1; n >= 0; n--) {
		debug("try: looking at [%d]: %s (%s)\n", n, devices[n].node, devices[n].removable ? "REMOVABLE" : "NONREMOVABLE");
		if (devices[n].removable == removable) {
			debug("try: [%d].removable == %d\n", devices[n].removable, removable);
			if (!trymount(devices[n].node))
				return 0;
		}
	}

	return -1;
}

int main(int argc, char **argv)
{
	int ret;
	int c = 0;

#ifdef DEBUG
	debug("main: Sleeping for 2 seconds....");
	sleep(2);
#endif

	/* mount proc/sys/dev etc */
	if (mount("sysfs", "/sys", "sysfs", MS_MGC_VAL, NULL))
		fprintf(stderr, "initrd: Failed to mount /sys\n");
	if (mount("proc", "/proc/", "proc", MS_MGC_VAL, NULL))
		fprintf(stderr, "initrd: Failed to mount /proc\n");
	if (mount("tmpfs", "/dev/", "tmpfs", MS_MGC_VAL, NULL))
		fprintf(stderr, "initrd: Failed to mount /dev\n");

	/* basic device nodes we'll need */
	if ((mknod("/dev/console", S_IFCHR, makedev(5, 1))) ||
	    (mknod("/dev/null", S_IFCHR, makedev(3, 1))))
		fprintf(stderr, "initrd: Failed to create device nodes in /dev\n");

	/* symlinks, just in case */
	if ((symlink("/proc/self/fd", "/dev/fd")) ||
	    (symlink("fd/0", "/dev/stdin")) ||
	    (symlink("fd/1", "/dev/stdout")) ||
	    (symlink("fd/2", "/dev/stderr")))
		fprintf(stderr, "initrd: Failed to write symlinks in /dev\n");

repeat:
	devcount = 0;
	memset(devices, 0, sizeof(devices));

	/* walk sysfs and create block device nodes we need */
	walk();

#ifdef DEBUG
	debug("main: got %d devices:\n", devcount);
	{
		int n;
		for (n = 0; n < devcount; n++) {
			debug("main: dev[%d].node = %s\n", n, devices[n].node);
			debug("main: dev[%d].removable = %d\n", n, devices[n].removable);
		}
	}
	sleep(2);
#endif

	/* try to mount removable devices first */
	if (!try(1))
		goto haveroot;

	/* try to mount sd device nodes */
	if (!try(0))
		goto haveroot;

	/* Display a message, but not too often - once per 30 seconds */
	if (c++ % 10 == 2)
		fprintf(stderr, "initrd: Failed to find a rootfs, retrying\n");

	/* Give the kernel a chance to try again */
	debug("main: sleeping 3 seconds and trying again\n");
	sleep(3);
	goto repeat;

haveroot:
	/* pre-root change cleanup */
	if (umount("/sys"))
		fprintf(stderr, "initrd: Failed to umount /sys\n");
	if (umount("/dev"))
		fprintf(stderr, "initrd: Failed to umount /dev\n");
	if (umount("/proc"))
		fprintf(stderr, "initrd: Failed to umount /proc\n");

	/* root is mounted, prep and move root */
	if (chdir("/newroot"))
		fprintf(stderr, "initrd: Failed to chdir /newroot\n");
	if (mount(".", "/", NULL, MS_MOVE, NULL))
		fprintf(stderr, "initrd: Unable to mount --move /newroot\n");
	if (chroot("."))
		fprintf(stderr, "initrd: Unable to chroot to /newroot\n");
	if (chdir("/"))
		fprintf(stderr, "initrd: Failed to chdir /\n");

#ifdef DEBUG
	debug("main: Executing /sbin/init....");
	sleep(2);
#endif

	/* exec the new init */
	execv("/sbin/init", argv);

	/* this should never be reached */
	fprintf(stderr, "initrd: Failed to exec /sbin/init\n");

	/*
	 *  FATAL: Things are pretty grim if exec() fails on us...
	 */
	exit (1);
}
