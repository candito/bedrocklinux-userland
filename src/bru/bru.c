/*
 * bru.c
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      version 2 as published by the Free Software Foundation.
 *
 * Copyright (c) 2013-2014 Daniel Thau <danthau@bedrocklinux.org>
 *
 * This program will union the contents of another directory with the contents
 * under the mount point.  The first argument should be the desired mount
 * point, the second argument should be the alternative location to union, and
 * the remaining arguments should be a list of things to be redirected to the
 * alternative location.  Everything not in the list of arguments from the
 * third argument onward will default to the contents under the mount point.
 *
 * For example, if you would like a handful of small but often accessed files
 * that are typically accessed in /tmp to instead be directed to /dev/shm, you
 * can do:
 *
 *     bru /tmp /dev/shm file1 file2 file3
 *     |   |    |        |           |
 *     |   |    |        +-----------+---- files to be directed into /dev/shm
 *     |   |    +------------------------- alternative location
 *     |   +------------------------------ mount point and default location
 *     +---------------------------------- bru executable name
 *
 * Makes heavy use of this FUSE API reference:
 *     http://fuse.sourceforge.net/doxygen/structfuse__operations.html
 *
 * If you're using a standard Linux glibc-based stack, compile with:
 *     gcc -g -Wall `pkg-config fuse --cflags --libs` bru.c -o bru
 *
 * If you're using musl, compile with:
 *     musl-gcc -Wall bru.c -o bru -lfuse
 */

#define _XOPEN_SOURCE 500

/*
 * explicitly mentioned as required in the following fuse tutorial:
 * http://sourceforge.net/apps/mediawiki/fuse/index.php?title=Hello_World
 */
#define FUSE_USE_VERSION 29
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <dirent.h>    /* DIR       */
#include <stdlib.h>    /* malloc    */
#include <unistd.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <sys/ioctl.h> /* will need for ioctl */
#include <poll.h>      /* will need for poll  */

/*
 * Global variables.
 */

int mount_fd;       /* file descriptor for directory under mount */
int alt_fd;         /* alt directory file descriptor */
char** alt_files;   /* list of files to go to alt directory */
int alt_file_count; /* number of items in above array */
int* alt_file_lens; /* length of items in above array */

/*
 * Macros
 */

/*
 * This macro will change the pwd to either the mount point or alt point
 * depending on whether the argument is within alt_files.
 *
 * From here, relative file paths provided to filesystem calls will correspond
 * to the proper file.
 */
#define CHDIR_REF(path)                                        \
do {                                                           \
	if (fchdir(mount_fd) < 0) {                                \
		return -errno;                                         \
	}                                                          \
	int i;                                                     \
	for (i=0; i < alt_file_count; i++) {                       \
		if (strncmp(alt_files[i], path, alt_file_lens[i]) == 0 \
				&& (path[alt_file_lens[i]] == '\0'             \
				    || path[alt_file_lens[i]] == '/')) {       \
			if (fchdir(alt_fd) < 0) {                          \
				return -errno;                                 \
			}                                                  \
			break;                                             \
		}                                                      \
	}                                                          \
} while (0)

/*
 * While most system calls will use the above CHDIR_REF() macro, a few need to
 * know the file descriptor directly in order to use the *at calls.
 *
 * Note we cannot use do/while(0) because we want our change to fd to be in the
 * calling scope.
 */
#define GET_FD_REF(path, fd, i)                                \
fd = mount_fd;                                                 \
for (i=0; i < alt_file_count; i++) {                           \
	if (strncmp(alt_files[i], path, alt_file_lens[i]) == 0     \
			&& (path[alt_file_lens[i]] == '\0'                 \
				|| path[alt_file_lens[i]] == '/')) {           \
		fd = alt_fd;                                           \
		break;                                                 \
	}                                                          \
}

/*
 * This macro sets the filesystem uid and gid to that of the calling user.
 * This allows the kernel to take care of permissions for us.
 */
#define SET_CALLER_UID()                               \
do {                                                   \
	struct fuse_context *context = fuse_get_context(); \
	setegid(context->gid);                             \
	seteuid(context->uid);                             \
} while (0)

/*
 * The FUSE API and Linux APIs do not match up perfectly.  One area they seem
 * to differ is that the Linux system calls tend to return -1 on error and set
 * errno to the value corresponding to the error.  FUSE, on the other hand,
 * wants (the negated version of) the error number returned.  This macro will
 * test for an error condition returned from a Linux system call and adjust it
 * for FUSE.
 */
#define SET_RET_ERRNO() if(ret < 0) ret = -errno;

/*
 * Given a full path, make it relative to root.  This is useful because
 * incoming paths will appear to be absolute when we want them relative to the
 * mount point.
 */
#define MAKE_RELATIVE(path) path = ((path[1] == '\0') ? "." : path+1);

/*
 * The following functions up until main() are all specific to FUSE.  See
 * FUSE's documentation - and general C POSIX filesystem documentation - for
 * details.
 *
 * It seems FUSE will handle details such as whether a given filesystem call
 * should resolve a symlink for us, and so we should always utilize the
 * versions of calls which work directly on symlinks rather than those which
 * resolve symlinks.  For example, lstat() should be utilized rather than
 * stat() when implementing getattr().
 *
 * Similarly, things like pread() and pwrite() should be utilized over read()
 * and write(); if the user wants non-p read() or write() FUSE will handle that
 * for us.
 *
 *
 * TODO/NOTE: some of the more obscure items below have not been tested, and
 * were simply written by comparing APIs.
 */

static int bru_getattr(const char *path, struct stat *stbuf)
{
	SET_CALLER_UID();
	MAKE_RELATIVE(path);
	CHDIR_REF(path);

	int ret = lstat(path, stbuf);

	SET_RET_ERRNO();
	return ret;
}

/*
 * The Linux readlink() manpage says it will not create the terminating null.
 * However, FUSE apparently does expect a terminating null or commands like `ls
 * -l` and `readlink` will respond incorrectly.  Linux readlink() will return the
 * number of bytes placed in the buffer; thus, we can add the terminating null
 * ourselves at the following byte.
 *
 * Moreover, the FUSE API says to truncate if we're over `bufsize`; so compare
 * `bufsize` to the number of bytes readlink() write to ensure we're not going
 * over `bufsize` when we write the terminating null.
 *
 * A simpler approach would be to zero-out the memory before having readlink()
 * write over it.  However, that is probably slower.
 *
 * Finally, note that readlink() returns the number of bytes placed in the
 * buffer if successful and a -1 otherwise.  FUSE readlink(), however, wants 0
 * for success.
 */

static int bru_readlink(const char *path, char *buf, size_t bufsize)
{
	SET_CALLER_UID();
	MAKE_RELATIVE(path);
	CHDIR_REF(path);

	/*
	 * Alternative approach zero out out the buffer:
	 * memset(buf, '\0', bufsize);
	 * TODO: Benchmark if this is faster.
	 */
	int bytes_read = readlink(path, buf, bufsize);
	int ret = 0;
	if(bytes_read < 0)
		ret = -errno;
	else if(bytes_read <= bufsize)
		buf[bytes_read] = '\0';

	return ret;
}

static int bru_mknod(const char *path, mode_t mode, dev_t dev)
{
	SET_CALLER_UID();
	MAKE_RELATIVE(path);
	CHDIR_REF(path);

	int ret = mknod(path, mode, dev);

	SET_RET_ERRNO();
	return ret;
}

static int bru_mkdir(const char *path, mode_t mode)
{
	SET_CALLER_UID();
	MAKE_RELATIVE(path);
	CHDIR_REF(path);

	int ret = mkdir(path, mode);

	SET_RET_ERRNO();
	return ret;
}

static int bru_unlink(const char *path)
{
	SET_CALLER_UID();
	MAKE_RELATIVE(path);
	CHDIR_REF(path);

	int ret = unlink(path);

	SET_RET_ERRNO();
	return ret;
}

static int bru_rmdir(const char *path)
{
	SET_CALLER_UID();
	MAKE_RELATIVE(path);
	CHDIR_REF(path);

	int ret = rmdir(path);

	SET_RET_ERRNO();
	return ret;
}

static int bru_symlink(const char *symlink_string, const char *path)
{
	SET_CALLER_UID();
	MAKE_RELATIVE(path);
	CHDIR_REF(path);

	int ret = symlink(symlink_string, path);

	SET_RET_ERRNO();
	return ret;
}

/*
 * rename() cannot work across filesystems/partitions due to how it works
 * under-the-hood. The way Linux checks if it is valid is by comparing the
 * mount points - even if both mount points are of the same
 * filesystem/partition, it still disallows the operation.
 *
 * Some programs, such as `mv`, will fall back to a copy/unlink if rename()
 * doesn't work.  However, others - such as groupadd - do not.  They seem
 * to assume that if two files are in the same directory - such as
 * "/etc/group-" and "/etc/group" - are in the root of same directory, they are
 * likely on the same filesystem.  This is typically a sane assumption, but
 * not with bru.  Hence we cannot simply pass rename() along as we do in
 * some of the other system calls here.  Instead, we check for EXDEV
 * (indicating the issue discussed above has happened) and, if we get that,
 * fall back to copy/unlink as something like `mv` would do.
 *
 * This could theoretically break applications which depend on rename() to
 * detect if files are on the same or different filesystems for something
 * outside outside the scope of bru.
 *
 * TODO: if an error occurs in the copy/unlink section, would we rather
 * return EXDEV or the error that actually happened?  Returning a read() or
 * write() error in a rename() could be a bit confusing, but hiding what
 * actually happened behind EXDEV could be problematic as well.
 */
static int bru_rename(const char *old_path, const char *new_path)
{
	SET_CALLER_UID();
	MAKE_RELATIVE(old_path);
	MAKE_RELATIVE(new_path);

	int ret = 0;

	/*
	 * We have two files we care about and we can't use CHDIR_REF for both as
	 * there's only one cwd.  Instead we can use the *at system calls to
	 * reference against the file descriptors CHDIR_REF is doing more directly.
	 */
	int i;
	int old_ref_fd;
	GET_FD_REF(old_path, old_ref_fd, i);
	int new_ref_fd;
	GET_FD_REF(new_path, new_ref_fd, i);

	/*
	 * Try rename() normally, first.
	 */
	if (renameat(old_ref_fd, old_path, new_ref_fd, new_path) < 0) {
		ret = -errno;
	}

	/*
	 * If it did *NOT* result in an EXDEV error, return.
	 */
	if (ret != -EXDEV) {
		SET_RET_ERRNO();
		return ret;
	}

	/*
	 * The rename() operation resulted in EXDEV. Falling back to copy/unlink.
	 */
	
	/*
	 * Unlink target if it exists.
	 */
	ret = unlinkat(new_ref_fd, new_path, 0);

	/*
	 * Open old_path for reading and create new_path for writing,
	 * being careful to transfer permissions.
	 */
	struct stat old_path_stat;
	fstatat(old_ref_fd, old_path, &old_path_stat, AT_SYMLINK_NOFOLLOW);

	int old_path_fd = openat(old_ref_fd, old_path, O_RDONLY);
	if (old_path_fd < 0) {
		return -errno;
	}
	int new_path_fd = openat(new_ref_fd, new_path, O_CREAT|O_WRONLY|O_TRUNC, old_path_stat);
	if (new_path_fd < 0) {
		return -errno;
	}

	int bufsize = 8192;
	char buffer[bufsize]; /* 8k */

	/*
	 * Copy
	 */
	int transfered;
	while (1) {
		transfered = read(old_path_fd, buffer, bufsize);
		if (transfered < 0) {
			/*
			 * Error occurred, clean up and quit.
			 */
			close(old_path_fd);
			close(new_path_fd);
			ret = transfered;
			SET_RET_ERRNO();
			return ret;
		}
		/*
		 * Completed copy.
		 */
		if (transfered == 0) {
			break;
		}
		transfered = write(new_path_fd, buffer, transfered);
		if (transfered < 0) {
			/*
			 * Error occurred, clean up and quit
			 */
			close(old_path_fd);
			close(new_path_fd);
			ret = transfered;
			SET_RET_ERRNO();
			return ret;
		}
	}

	/*
	 * Copy should have went well at this point.  Close both files.
	 */
	close(old_path_fd);
	close(new_path_fd);
	/*
	 * Unlink old file
	 */
	ret = unlinkat(old_ref_fd, old_path, 0);
	/*
	 * Check for error during unlink
	 */
	SET_RET_ERRNO();
	return ret;
}

static int bru_link(const char *old_path, const char *new_path){
	SET_CALLER_UID();
	MAKE_RELATIVE(old_path);
	MAKE_RELATIVE(new_path);

	/*
	 * We have two files we care about and we can't use CHDIR_REF for both as
	 * there's only one cwd.  Instead we can use the *at system calls to
	 * reference against the file descriptors CHDIR_REF is doing more directly.
	 */
	int i;
	int old_ref_fd;
	GET_FD_REF(old_path, old_ref_fd, i);
	int new_ref_fd;
	GET_FD_REF(new_path, new_ref_fd, i);

	int ret = linkat(old_ref_fd, old_path, new_ref_fd, new_path, AT_SYMLINK_FOLLOW);

	SET_RET_ERRNO();
	return ret;
}

static int bru_chmod(const char *path, mode_t mode){
	SET_CALLER_UID();
	MAKE_RELATIVE(path);
	CHDIR_REF(path);
	
	int ret = chmod(path, mode);

	SET_RET_ERRNO();
	return ret;
}

static int bru_chown(const char *path, uid_t owner, gid_t group){
	SET_CALLER_UID();
	MAKE_RELATIVE(path);
	CHDIR_REF(path);
	
	int ret = lchown(path, owner, group);

	SET_RET_ERRNO();
	return ret;
}

static int bru_truncate(const char *path, off_t length){
	SET_CALLER_UID();
	MAKE_RELATIVE(path);
	CHDIR_REF(path);

	int ret = truncate(path, length);

	SET_RET_ERRNO();
	return ret;
}

/*
 * Unlike POSIX open(), it seems the return value should be 0 for success, not
 * the file descriptor.
 */
static int bru_open(const char *path, struct fuse_file_info *fi)
{
	SET_CALLER_UID();
	MAKE_RELATIVE(path);
	CHDIR_REF(path);

	int ret = open(path, fi->flags);

	if (ret < 0) {
		ret = -errno;
	} else {
		fi->fh = ret;
		ret = 0;
	}

	return ret;
}

static int bru_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	SET_CALLER_UID();

	int ret = pread(fi->fh, buf, size, offset);

	SET_RET_ERRNO();
	return ret;
}

static int bru_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	SET_CALLER_UID();

	int ret = pwrite(fi->fh, buf, size, offset);

	SET_RET_ERRNO();
	return ret;
}

/*
 * Using statvfs instead of statfs, per FUSE API.
 */
static int bru_statfs(const char *path, struct statvfs *buf)
{
	SET_CALLER_UID();
	MAKE_RELATIVE(path);
	CHDIR_REF(path);
	
	int ret = statvfs(path, buf);

	SET_RET_ERRNO();
	return ret;
}

/*
 * FUSE uses the word "release" rather than "close".
 */
static int bru_release(const char *path, struct fuse_file_info *fi)
{
	SET_CALLER_UID();

	int ret = close(fi->fh);

	SET_RET_ERRNO();
	return ret;
}

/*
 * The FUSE API talks about a 'datasync parameter' being non-zero - presumably
 * that's the second parameter, since its the only number (int).
 */
static int bru_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
	SET_CALLER_UID();

	int ret;
	if(datasync)
		ret = fdatasync(fi->fh);
	else
		ret = fsync(fi->fh);

	SET_RET_ERRNO();
	return ret;
}

static int bru_setxattr(const char *path, const char *name, const char *value, size_t size, int flags)
{
	SET_CALLER_UID();
	MAKE_RELATIVE(path);
	CHDIR_REF(path);

	int ret = lsetxattr(path, name, value, size, flags);

	SET_RET_ERRNO();
	return ret;
}

static int bru_getxattr(const char *path, const char *name, char *value, size_t size)
{
	SET_CALLER_UID();
	MAKE_RELATIVE(path);
	CHDIR_REF(path);

	int ret = lgetxattr(path, name, value, size);

	SET_RET_ERRNO();
	return ret;
}

static int bru_listxattr(const char *path, char *list, size_t size)
{
	SET_CALLER_UID();
	MAKE_RELATIVE(path);
	CHDIR_REF(path);

	int ret = llistxattr(path, list, size);

	SET_RET_ERRNO();
	return ret;
}

static int bru_removexattr(const char *path, const char *name)
{
	SET_CALLER_UID();
	MAKE_RELATIVE(path);
	CHDIR_REF(path);

	int ret = lremovexattr(path, name);

	SET_RET_ERRNO();
	return ret;
}

/*
 * FUSE uses this primarily for a permissions check.  Actually returning a
 * file handler is optional.  Unlike POSIX, this does not directly return
 * the file handler, but rather returns indictation of whether or not the user
 * may use opendir().
 */
static int bru_opendir(const char *path, struct fuse_file_info *fi)
{
	SET_CALLER_UID();
	MAKE_RELATIVE(path);
	CHDIR_REF(path);

	int ret;
	DIR *d = opendir(path);
	/*
	 * It seems FUSE wants an int pointer, not a directory stream pointer.
	 */
	fi->fh = (intptr_t) d;
	if (d)
		ret = 0;
	else
		ret = -errno;

	return ret;
}

/*
 * This function returns the files in a given directory.  We want to
 * actually return three groups:
 * - "." and ".."
 * - Files that match redir_files and are in the same place on redir_dir.
 * - Files that do not match redir_files and are in the same place in
 *   default_dir.
 */
static int bru_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
	SET_CALLER_UID();
	MAKE_RELATIVE(path);

	int i;
	/*
	 * Store a directory.
	 */
	DIR *d;
	struct dirent *dir;
	/*
	 * We'll have combine the provided path and its contents to get strings to
	 * compare against alt_files.
	 */
	char *full_path;
	/*
	 * alt point is populated if there is a match, mount point is populated if
	 * not.  For mount point we have to iterate over all alt_files.  Track if
	 * any match.
	 */
	int match;
	/*
	 * If neither mount point nor alt point have specified directory , ENOENT.
	 * However, we have to check both.  Track here.
	 */
	int exists = 0;
	/* This will be referenced repeatedly, pre-calculate once here */
	int path_len = strlen(path);

	/*
	 * Every directory has these.
	 */
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	/*
	 * Populate items from alt point
	 */
	i = fchdir(alt_fd);
	d = opendir(path);
	if (i >= 0 && d) {
		while ((dir = readdir(d)) != NULL) {
			/*
			 * If the file is "." or "..", we can skip the rest of this iteration.
			 */
			if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) {
				continue;
			}
			/*
			 * The path items we're getting are relative to a directory
			 * that is relative to the mount/alt point, which makes it
			 * awkward to compare against the alt list.  Get a full string
			 * we can compare.
			 */
			full_path = malloc(path_len + strlen(dir->d_name));
			if (path[1] == '\0') {
				strcpy(full_path, dir->d_name);
			} else {
				strcpy(full_path, path);
				strcat(full_path, "/");
				strcat(full_path, dir->d_name);
			}
			/*
			 * Iterate over items in alt list and add if we find a match.
			 */
			for (i=0; i < alt_file_count; i++) {
				if (strncmp(full_path, alt_files[i], alt_file_lens[i]) == 0
						&& (full_path[alt_file_lens[i]] == '\0'
							|| full_path[alt_file_lens[i]] == '/')) {
					filler(buf, dir->d_name, NULL, 0);
					break;
				}
			}
			free(full_path);
		}
		closedir(d);
		exists = 1;
	}

	/*
	 * Populate with items from mount point
	 */
	i = fchdir(mount_fd);
	d = opendir(path);
	if (i >= 0 && d) {
		while ((dir = readdir(d)) != NULL) {
			/*
			 * If the file is "." or "..", we can skip the rest of this iteration.
			 */
			if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) {
				continue;
			}
			/*
			 * The path items we're getting are relative to a directory
			 * that is relative to the mount/alt point, which makes it
			 * awkward to compare against the alt list.  Get a full string
			 * we can compare.
			 */
			full_path = malloc(path_len + strlen(dir->d_name));
			if (path[1] == '\0') {
				strcpy(full_path, dir->d_name);
			} else {
				strcpy(full_path, path);
				strcat(full_path, "/");
				strcat(full_path, dir->d_name);
			}
			/*
			 * Iterate over items in alt list and add if we don't find a match.
			 */
			match = 0;
			for (i=0; i < alt_file_count; i++) {
				if (strncmp(full_path, alt_files[i], alt_file_lens[i]) == 0
						&& (full_path[alt_file_lens[i]] == '\0'
							|| full_path[alt_file_lens[i]] == '/')) {
					match = 1;
					break;
				}
			}
			if (match == 0) {
				filler(buf, dir->d_name, NULL, 0);
			}
		}
		closedir(d);
		exists = 1;
	}

	if (!exists) {
		return -ENOENT;
	}
	return 0;
}

/*
 * FUSE uses the word "release" rather than "close".
 */
static int bru_releasedir(const char *path, struct fuse_file_info *fi)
{
	SET_CALLER_UID();

	/*
	 * FUSE provides an "uint64_t" rather than DIR*
	 */
	int ret = closedir((DIR *) fi->fh);

	SET_RET_ERRNO();
	return ret;
}

/*
 * There is no POSIX fsyncdir - presumably this is just fsync when called on a
 * directory.  Mimicking code from (non-dir) fsync.
 */
static int bru_fsyncdir(const char *path, int datasync, struct fuse_file_info *fi)
{
	SET_CALLER_UID();

	int ret;
	if (datasync) {
		ret = fdatasync(fi->fh);
	} else {
		ret = fsync(fi->fh);
	}

	SET_RET_ERRNO();
	return ret;
}

/*
 * We cannot use POSIX access() for two reasons:
 * 1. It uses real uid, rather than effective or filesystem uid.
 * 2. It dereferences symlinks.
 * Instead, we're using faccessat().
 * TODO: POSIX faccessat() doesn't support AT_SYMLINK_NOFOLLOW, and neither
 * does musl.  See if we can upstream support into musl.  Utilizing
 * AT_SYMLINK_NOFOLLOW is disabled for now so it will compile against musl.
 */
static int bru_access(const char *path, int mask)
{
	SET_CALLER_UID();
	MAKE_RELATIVE(path);

	int i;
	int ref_fd;
	GET_FD_REF(path, ref_fd, i);

	/*
	 * Disabling AT_SYMLINK_NOFOLLOW since musl does not (yet?) support it.
	 * int ret = faccessat(ref_fd, path, mask, AT_EACCESS | AT_SYMLINK_NOFOLLOW);
	 */
    int ret = faccessat(ref_fd, path, mask, AT_EACCESS);

	SET_RET_ERRNO();
	return ret;
}

/*
 * Yes, FUSE uses creat*e* and POSIX uses creat.
 */
static int bru_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	SET_CALLER_UID();
	MAKE_RELATIVE(path);
	CHDIR_REF(path);

	int ret = 0;
	if ((fi->fh = creat(path, mode)) < 0)
		ret = -errno;

	return ret;
}

static int bru_ftruncate(const char *path, off_t length, struct fuse_file_info *fi)
{
	SET_CALLER_UID();

	int ret = ftruncate(fi->fh, length);

	SET_RET_ERRNO();
	return ret;
}

static int bru_fgetattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
	SET_CALLER_UID();

	int ret = fstat(fi->fh, stbuf);

	SET_RET_ERRNO();
	return ret;
}

static int bru_utimens(const char *path, const struct timespec *times)
{
	SET_CALLER_UID();
	MAKE_RELATIVE(path);

	int i;
	int ref_fd;
	GET_FD_REF(path, ref_fd, i);

	int ret = utimensat(ref_fd, path, times, AT_SYMLINK_NOFOLLOW);

	SET_RET_ERRNO();
	return ret;
}

/*
 * TODO: implement
 * static int bru_ioctl(const char *path, int request, void *arg, struct
 * 		fuse_file_info *fi, unsigned int flags, void *data)
 * {
 * 	SET_CALLER_UID();
 * 
 * 	int ret = ioctl(fi->fh, request, data);
 * 
 * 	SET_RET_ERRNO();
 * 	return ret;
 * }
 */


/*
 * This struct is a list of implemented fuse functions which is provided to
 * FUSE in main().
 */
static struct fuse_operations bru_oper = {
	.getattr = bru_getattr,
	.readlink = bru_readlink,
	.mknod = bru_mknod,
	.mkdir = bru_mkdir,
	.unlink = bru_unlink,
	.rmdir = bru_rmdir,
	.symlink = bru_symlink,
	.rename = bru_rename,
	.link = bru_link,
	.chmod = bru_chmod,
	.chown = bru_chown,
	.truncate = bru_truncate,
	.open = bru_open,
	.read = bru_read,
	.write = bru_write,
	.statfs = bru_statfs,
	/*
	 * I *think* we can skip implementing this, as the underlying filesystem
	 * will take care of it. TODO: Confirm.
	 * .flush = bru_flush,
	 */
	.release = bru_release,
	.fsync = bru_fsync,
	.setxattr = bru_setxattr,
	.getxattr = bru_getxattr,
	.listxattr = bru_listxattr,
	.removexattr = bru_removexattr,
	.opendir = bru_opendir,
	.readdir = bru_readdir,
	.releasedir = bru_releasedir,
	.fsyncdir = bru_fsyncdir,
	/*
	 * These seem to be hooks at mount/unmount time of which bru does not need
	 * to take advantage.
	 * .init = bru_init,
	 * .destroy = bru_destroy,
	 */
	.access = bru_access,
	.create = bru_create,
	.ftruncate = bru_ftruncate,
	.fgetattr = bru_fgetattr,
	/*
	 * TODO: Apparently this is unimplemented in the Linux kernel?
	 * .lock = bru_lock,
	 */
	.utimens = bru_utimens,
	/*
	 * This only makes sense for block devices.
	 * .bmap = bru_bmap,
	 */
	/*
	 * TODO: implement these:
	 * .ioctl = bru_ioctl,
	 * .poll = bru_poll,
	 * .write_buf = bru_write_buf,
	 * .read_buf = bru_read_buf,
	 * .flock = bru_flock,
	 * .fallocate = bru_fallocate,
	 */
};

int main(int argc, char* argv[])
{
	/*
	 * Print help.  If there are less than two arguments the user probably
	 * doesn't know how to use this, and will also cover things like --help and
	 * -h.
	 */
	if (argc < 2) {
		printf(
"bru - BedRock linux Union filesystem\n"
"\n"
"Usage: bru [mount-point] [alt directory] [paths]\n"
"\n"
"Example: bru /tmp /dev/shm file1 file2 file3\n"
"\n"
"[mount-point]       is the directory where the filesystem will be mounted\n"
"                    as well as where filesystem calls which aren't to [paths]\n"
"                    will be directed.  This must be a directory.\n"
"[redir directory]   is where filesystem calls which are in [paths] will be\n"
"                    redirected.  This must be a directory.\n"
"[paths]             is the list of file paths relative to [mount-point]\n"
"                    which will be redirected to [redir directory].\n"
"                    Everything else will be redirected to\n"
"                    [mount-point].  [paths] items must not start or end with\n"
"                    a slash.\n");
		return 1;
	}

	/*
	 * Ensure we are running as root so that any requests by root to this
	 * filesystem can be provided.
	 */
	if(getuid() != 0){
		fprintf(stderr, "ERROR: not running as root, aborting.\n");
		return 1;
	}

	/*
	 * Ensure sufficient arguments are provided
	 */
	if (argc < 3) {
		fprintf(stderr, "ERROR: Insufficient arguments.\n");
		return 1;
	}

	/*
	 * argv[1] is the desired mount point.  Get the directory's file descriptor
	 * *before* mounting so we can access files under the mount point by
	 * referencing the file descriptor.
	 */
	mount_fd = open(argv[1], O_DIRECTORY);
	if (mount_fd == -1) {
		fprintf(stderr, "ERROR: Could not open mount point \"%s\", aborting.\n", argv[1]);
		return 1;
	}

	/*
	 * argv[2] is the alternate location to reference for file access.  Similar
	 * to the mount point, get the file descriptor here.  We can then easily
	 * set which file descriptor to use when attempting to access another file.
	 */
	alt_fd = open(argv[2], O_DIRECTORY);
	if (alt_fd == -1) {
		fprintf(stderr, "ERROR: Could not open alt point \"%s\", aborting.\n", argv[2]);
		return 1;
	}

	/*
	 * All of the arguments except the first two constitute the alt point list.
	 * Don't forget about bru being argv[0].
	 *
	 *
	 * +- argv
	 * |     +- argv + 1
	 * |     |    +- argv + 2
	 * |     |    |        +- argv + 3
	 * |     |    |        |
	 * ./bru /tmp /dev/shm foo bar baz
	 * |     |    |        |       |
	 * |     |    |        +-------+ argc - 3
	 * |     |    +----------------+ argc - 2
	 * |     +---------------------+ argc - 1
	 * +---------------------------+ argc
	 *
	 */
	alt_file_count = argc - 3;
	alt_files = argv + 3;

	/*
	 * All of the alt_files should start with a slash and should not end with
	 * a slash.
	 */
	int i;
	for (i=0; i < alt_file_count; i++) {
		if (alt_files[i][0] == '/' || alt_files[i][strlen(alt_files[i])-1] == '/') {
			fprintf(stderr, "The alternate location files should not start or "
					"end with a '/'.  This one is problematic: "
					"\"%s\"\n", alt_files[i]);
			return 1;
		}
	}
	
	/*
	 * Pre-calculate lengths to use at runtime.
	 */
	alt_file_lens = malloc(alt_file_count * sizeof(int));
	for (i=0; i < alt_file_count; i++) {
		alt_file_lens[i] = strlen(alt_files[i]);
	}

	/* Generate arguments for fuse:
	 * - start with no arguments
	 * - add argv[0] (which I think is just ignored)
	 * - add mount point
	 * - disable multithreading, as with the UID/GID switching it will result
	 *   in abusable race conditions.
	 * - add argument to:
	 *   - let all users access filesystem
	 *   - allow mounting over non-empty directories
	 * - stay in the fore ground, user can "&" if the prefer backgrounding.
	 */
	struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
	fuse_opt_add_arg(&args, argv[0]);
	fuse_opt_add_arg(&args, argv[1]);
	fuse_opt_add_arg(&args, "-s");
	fuse_opt_add_arg(&args, "-oallow_other,nonempty");
	/* stay in foreground, useful for debugging */
	fuse_opt_add_arg(&args, "-f");

	/* start fuse */
	return fuse_main(args.argc, args.argv, &bru_oper, NULL);
}
