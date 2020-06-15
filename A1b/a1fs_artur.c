/*
 * This code is provided solely for the personal and private use of students
 * taking the CSC369H course at the University of Toronto. Copying for purposes
 * other than this use is expressly prohibited. All forms of distribution of
 * this code, including but not limited to public repositories on GitHub,
 * GitLab, Bitbucket, or any other online platform, whether as given or with
 * any changes, are expressly prohibited.
 *
 * Authors: Alexey Khrabrov, Karen Reid
 *
 * All of the files in this directory and all subdirectories are:
 * Copyright (c) 2019 Karen Reid
 */

/**
 * CSC369 Assignment 1 - a1fs driver implementation.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

// Using 2.9.x FUSE API
#define FUSE_USE_VERSION 29
#include <fuse.h>

#include "a1fs.h"
#include "fs_ctx.h"
#include "options.h"
#include "map.h"

//NOTE: All path arguments are absolute paths within the a1fs file system and
// start with a '/' that corresponds to the a1fs root directory.
//
// For example, if a1fs is mounted at "~/my_csc369_repo/a1b/mnt/", the path to a
// file at "~/my_csc369_repo/a1b/mnt/dir/file" (as seen by the OS) will be
// passed to FUSE callbacks as "/dir/file".
//
// Paths to directories (except for the root directory - "/") do not end in a
// trailing '/'. For example, "~/my_csc369_repo/a1b/mnt/dir/" will be passed to
// FUSE callbacks as "/dir".


/**
 * Initialize the file system.
 *
 * Called when the file system is mounted. NOTE: we are not using the FUSE
 * init() callback since it doesn't support returning errors. This function must
 * be called explicitly before fuse_main().
 *
 * @param fs    file system context to initialize.
 * @param opts  command line options.
 * @return      true on success; false on failure.
 */
static bool a1fs_init(fs_ctx *fs, a1fs_opts *opts)
{
	// Nothing to initialize if only printing help or version
	if (opts->help || opts->version) return true;

	size_t size;
	void *image = map_file(opts->img_path, A1FS_BLOCK_SIZE, &size);
	if (!image) return false;

	return fs_ctx_init(fs, image, size, opts);
}

/**
 * Cleanup the file system.
 *
 * Called when the file system is unmounted. Must cleanup all the resources
 * created in a1fs_init().
 */
static void a1fs_destroy(void *ctx)
{
	fs_ctx *fs = (fs_ctx*)ctx;
	if (fs->image) {
		if (fs->opts->sync && (msync(fs->image, fs->size, MS_SYNC) < 0)) {
			perror("msync");
		}
		munmap(fs->image, fs->size);
		fs_ctx_destroy(fs);
	}
}

/** Get file system context. */
static fs_ctx *get_fs(void)
{
	return (fs_ctx*)fuse_get_context()->private_data;
}


/**
 * Get file system statistics.
 *
 * Implements the statvfs() system call. See "man 2 statvfs" for details.
 * The f_bfree and f_bavail fields should be set to the same value.
 * The f_ffree and f_favail fields should be set to the same value.
 * The following fields can be ignored: f_fsid, f_flag.
 * All remaining fields are required.
 *
 * @param path  path to any file in the file system. Can be ignored.
 * @param st    pointer to the struct statvfs that receives the result.
 * @return      0 on success; -errno on error.
 */
static int a1fs_statfs(const char *path, struct statvfs *st)
{
	(void)path;// unused
	fs_ctx *fs = get_fs();

	memset(st, 0, sizeof(*st));
	st->f_bsize   = A1FS_BLOCK_SIZE;
	st->f_frsize  = A1FS_BLOCK_SIZE;
	//TODO: fill in the rest of required fields based on the information stored
	// in the superblock
	(void)fs;
	st->f_namemax = A1FS_NAME_MAX;

	return -ENOSYS;
}

int inode_by_name(struct a1fs_inode *dir, struct a1fs_inode **file, char *name, void *image){
	struct a1fs_superblock *sb = (struct a1fs_superblock*)(image);
	struct a1fs_extent *curr_extent;
	int entry_count = 0;

	// Go through all of this directory's extents
	for (int i = 0; i < dir->extents; i++){
		if (i >= A1FS_IND_BLOCK){
			// We are now looking in the indirect block.
			int extent_location = A1FS_BLOCK_SIZE*(sb->data_region + ((dir->extent)[A1FS_IND_BLOCK]).start) + (i-A1FS_IND_BLOCK)*sizeof(a1fs_extent);
			curr_extent = (struct a1fs_extent*)(image + extent_location);
		}
		else{
			// We are looking at the extent at index i in the extents array.
			curr_extent = &(dir->extent)[i];
		}
		// Loop through this entire extent (depending on extent length).
		for (size_t j = 0; j < curr_extent->count; j++){
			int curr_entry_block = sb->data_region + curr_extent->start + j;

			// Loop through all the entries in this extent (while keeping track of entry count).
			for (size_t k = 0; k < A1FS_BLOCK_SIZE/sizeof(a1fs_dentry); k++){
				if (entry_count < dir->dentry){
					struct a1fs_dentry *curr_entry = (struct a1fs_dentry*)(image + A1FS_BLOCK_SIZE*curr_entry_block + k*sizeof(a1fs_dentry));

					// Check if this is the entry we are looking for.
					if (strcmp(curr_entry->name, name) == 0){
						int inode_location = sb->inode_table*A1FS_BLOCK_SIZE + curr_entry->ino*sizeof(a1fs_inode);
						*file = (struct a1fs_inode*)(image + inode_location);
						return 0;
					}
				}
				entry_count++;
			}
		}
	}
	return 1;
}

// Returns 0 if success or errors on fail.
int inode_from_path(struct a1fs_inode *dir, struct a1fs_inode **file, const char *path, void *image){
	// Extract dir/file names from path one by one.
	int start = 0;
	int end = 0;
	int started = 0;
	char currFile[A1FS_NAME_MAX];
	struct a1fs_inode *curr_dir = dir;
	for (size_t i = 0; i < strlen(path); i++){
		// Either we reached the end of the path string or we reached a '/' char.
		if (i == strlen(path) - 1 || path[i] == '/'){
			// Check if the '/' char is the ending of a file/dir name.
			if (started){
				// Is the last char part of the file/dir name or is it a '/'?
				if (path[i] == '/'){
					end = i;
				} else {
					end = i + 1;
				}
				strncpy(currFile, path+start+1, end-start-1);
				currFile[end-start-1] = '\0';
				printf("currFile: %s\n", currFile);
				struct a1fs_inode* target = ((void*)0);
				// We found the file in the current directory.
				if (inode_by_name(curr_dir, &target, currFile, image) == 0){
					// We got the file/dir from the path.
					if (i == strlen(path) - 1){
						*file = target;
						return 0;
					}
					else {
						// Check if this is a dir
						if (target->mode == A1FS_S_IFDIR){
							curr_dir = target;
							start = i;
						}
						else {
							return -ENOTDIR;
						}
					}
				// We didn't find the file from the path.
				} else {
					return -ENOENT;
				}
			} else {
				started = 1;
				start = i;
			}
		} 
	}	
	return -ENOSYS;
}

/**
 * Get file or directory attributes.
 *
 * Implements the stat() system call. See "man 2 stat" for details.
 * The following fields can be ignored: st_dev, st_ino, st_uid, st_gid, st_rdev,
 *                                      st_blksize, st_atim, st_ctim.
 * All remaining fields are required.
 *
 * NOTE: the st_blocks field is measured in 512-byte units (disk sectors).
 *
 * Errors:
 *   ENAMETOOLONG  the path or one of its components is too long.
 *   ENOENT        a component of the path does not exist.
 *   ENOTDIR       a component of the path prefix is not a directory.
 *
 * @param path  path to a file or directory.
 * @param st    pointer to the struct stat that receives the result.
 * @return      0 on success; -errno on error;
 */
static int a1fs_getattr(const char *path, struct stat *st)
{
	printf("Path: %s\n", path);
	printf("Path len: %ld\n", strlen(path));
	if (strlen(path) >= A1FS_PATH_MAX) return -ENAMETOOLONG;
	fs_ctx *fs = get_fs();

	memset(st, 0, sizeof(*st));
	//NOTE: This is just a placeholder that allows the file system to be mounted
	// without errors. You should remove this from your implementation.
	if (strcmp(path, "/") == 0) {
		//NOTE: all the fields set below are required and must be set according
		// to the information stored in the corresponding inode
		st->st_mode = S_IFDIR | 0777;
		st->st_nlink = 22555;
		st->st_size = 0;
		st->st_blocks = 0 * A1FS_BLOCK_SIZE / 512;
		st->st_mtim = (struct timespec){0};
		return 0;
	}

	//TODO: lookup the inode for given path and, if it exists, fill in the
	// required fields based on the information stored in the inode
	struct a1fs_superblock *sb = (struct a1fs_superblock*)(fs->image);
	struct a1fs_inode *inodes = (struct a1fs_inode*)(fs->image + A1FS_BLOCK_SIZE * sb->inode_table);
	struct a1fs_inode *root_inode = inodes + A1FS_ROOT_INO;

	// Extract dir/file names from path one by one.
	struct a1fs_inode *target = (void *)0;
	int ret = inode_from_path(root_inode, &target, path,fs->image);
	if (ret == 0){
		st->st_mode = target->mode;
		st->st_nlink = 2255525;
		st->st_size = target->size;
		st->st_blocks = target->size / 512;
		st->st_mtim = target->mtime;
		return 0;
	}
	else {
		return ret;
	}
	// int start = 0;
	// int end = 0;
	// int started = 0;
	// char currFile[A1FS_NAME_MAX];
	// struct a1fs_inode *curr_dir = root_inode;
	// for (size_t i = 0; i < strlen(path); i++){
	// 	// Either we reached the end of the path string or we reached a '/' char.
	// 	if (i == strlen(path) - 1 || path[i] == '/'){
	// 		// Check if the '/' char is the ending of a file/dir name.
	// 		if (started){
	// 			// Is the last char part of the file/dir name or is it a '/'?
	// 			if (path[i] == '/'){
	// 				end = i;
	// 			} else {
	// 				end = i + 1;
	// 			}
	// 			strncpy(currFile, path+start+1, end-start-1);
	// 			currFile[end-start-1] = '\0';
	// 			printf("currFile: %s\n", currFile);
	// 			struct a1fs_inode* target = ((void*)0);
	// 			// We found the file in the current directory.
	// 			if (inode_by_name(curr_dir, &target, currFile, fs->image)){
	// 				if (i == strlen(path) - 1){
	// 					st->st_mode = target->mode;
	// 					st->st_nlink = 2225;
	// 					st->st_size = target->size;
	// 					st->st_blocks = target->size / 512;
	// 					st->st_mtim = target->mtime;
	// 					return 0;
	// 				}
	// 				else {
	// 					// Check if this is a dir
	// 					if (target->mode == A1FS_S_IFDIR){
	// 						curr_dir = target;
	// 						start = i;
	// 					}
	// 					else {
	// 						return -ENOTDIR;
	// 					}
	// 				}
	// 			// We didn't find the file from the path.
	// 			} else {
	// 				return -ENOENT;
	// 			}
	// 		} else {
	// 			started = 1;
	// 			start = i;
	// 		}
	// 	} 
	// }	
	
	return -ENOSYS;
}

/**
 * Read a directory.
 *
 * Implements the readdir() system call. Should call filler() for each directory
 * entry. See fuse.h in libfuse source code for details.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a directory.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a filler() call failed).
 *
 * @param path    path to the directory.
 * @param buf     buffer that receives the result.
 * @param filler  function that needs to be called for each directory entry.
 *                Pass 0 as offset (4th argument). 3rd argument can be NULL.
 * @param offset  unused.
 * @param fi      unused.
 * @return        0 on success; -errno on error.
 */
static int a1fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi)
{
	(void)offset;// unused
	(void)fi;// unused
	fs_ctx *fs = get_fs();

	//NOTE: This is just a placeholder that allows the file system to be mounted
	// without errors. You should remove this from your implementation.
	// if (strcmp(path, "/") == 0) {
	// 	filler(buf, "." , NULL, 0);
	// 	filler(buf, "..", NULL, 0);
	// 	return 0;
	// }

	//TODO: lookup the directory inode for given path and iterate through its
	// directory entries
	struct a1fs_superblock *sb = (struct a1fs_superblock*)(fs->image);
	struct a1fs_inode *inodes = (struct a1fs_inode*)(fs->image + A1FS_BLOCK_SIZE * sb->inode_table);
	struct a1fs_inode *root_inode = inodes + A1FS_ROOT_INO;

	struct a1fs_inode *target = (void *)0;
	if (strcmp(path, "/") == 0){
		target = root_inode;
	} else{
		inode_from_path(root_inode, &target, path, fs->image);
	}
	struct a1fs_extent *curr_extent;
	int entry_count = 0;
	
	for (int i = 0; i < target->extents; i++){
		if (i >= A1FS_IND_BLOCK){
			// We are now looking in the indirect block.
			int extent_location = A1FS_BLOCK_SIZE*(sb->data_region + ((target->extent)[A1FS_IND_BLOCK]).start) + (i-A1FS_IND_BLOCK)*sizeof(a1fs_extent);
			curr_extent = (struct a1fs_extent*)(fs->image + extent_location);
		}
		else{
			// We are looking at the extent at index i in the extents array.
			curr_extent = &(target->extent)[i];
		}
		// Loop through this entire extent (depending on extent length).
		for (size_t j = 0; j < curr_extent->count; j++){
			int curr_entry_block = sb->data_region + curr_extent->start + j;

			// Loop through all the entries in this extent (while keeping track of entry count).
			for (size_t k = 0; k < A1FS_BLOCK_SIZE/sizeof(a1fs_dentry); k++){
				if (entry_count < target->dentry){
					struct a1fs_dentry *curr_entry = (struct a1fs_dentry*)(fs->image + A1FS_BLOCK_SIZE*curr_entry_block + k*sizeof(a1fs_dentry));

					// Check if this is the entry we are looking for.
					if (filler(buf, curr_entry->name, NULL, 0)){
						return -ENOMEM;
					}
				}
				entry_count++;
			}
		}
	}
	return 0;


	return -ENOSYS;
}


/**
 * Create a directory.
 *
 * Implements the mkdir() system call.
 *
 * NOTE: the mode argument may not have the type specification bits set, i.e.
 * S_ISDIR(mode) can be false. To obtain the correct directory type bits use
 * "mode | S_IFDIR".
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" doesn't exist.
 *   The parent directory of "path" exists and is a directory.
 *   "path" and its components are not too long.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a malloc() call failed).
 *   ENOSPC  not enough free space in the file system.
 *
 * @param path  path to the directory to create.
 * @param mode  file mode bits.
 * @return      0 on success; -errno on error.
 */
static int a1fs_mkdir(const char *path, mode_t mode)
{
	fs_ctx *fs = get_fs();

	//TODO: create a directory at given path with given mode
	(void)path;
	(void)mode;
	(void)fs;
	return -ENOSYS;
}

/**
 * Remove a directory.
 *
 * Implements the rmdir() system call.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a directory.
 *
 * Errors:
 *   ENOTEMPTY  the directory is not empty.
 *
 * @param path  path to the directory to remove.
 * @return      0 on success; -errno on error.
 */
static int a1fs_rmdir(const char *path)
{
	fs_ctx *fs = get_fs();

	//TODO: remove the directory at given path (only if it's empty)
	(void)path;
	(void)fs;
	return -ENOSYS;
}

/**
 * Create a file.
 *
 * Implements the open()/creat() system call.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" doesn't exist.
 *   The parent directory of "path" exists and is a directory.
 *   "path" and its components are not too long.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a malloc() call failed).
 *   ENOSPC  not enough free space in the file system.
 *
 * @param path  path to the file to create.
 * @param mode  file mode bits.
 * @param fi    unused.
 * @return      0 on success; -errno on error.
 */
static int a1fs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	(void)fi;// unused
	assert(S_ISREG(mode));
	fs_ctx *fs = get_fs();

	//TODO: create a file at given path with given mode
	(void)path;
	(void)mode;
	(void)fs;
	return -ENOSYS;
}

/**
 * Remove a file.
 *
 * Implements the unlink() system call.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a file.
 *
 * @param path  path to the file to remove.
 * @return      0 on success; -errno on error.
 */
static int a1fs_unlink(const char *path)
{
	fs_ctx *fs = get_fs();

	//TODO: remove the file at given path
	(void)path;
	(void)fs;
	return -ENOSYS;
}

/**
 * Rename a file or directory.
 *
 * Implements the rename() system call. See "man 2 rename" for details.
 * If the destination file (directory) already exists, it must be replaced with
 * the source file (directory). Existing destination can be replaced if it's a
 * file or an empty directory.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "from" exists.
 *   The parent directory of "to" exists and is a directory.
 *   If "from" is a file and "to" exists, then "to" is also a file.
 *   If "from" is a directory and "to" exists, then "to" is also a directory.
 *
 * Errors:
 *   ENOMEM     not enough memory (e.g. a malloc() call failed).
 *   ENOTEMPTY  destination is a non-empty directory.
 *   ENOSPC     not enough free space in the file system.
 *
 * @param from  original file path.
 * @param to    new file path.
 * @return      0 on success; -errno on error.
 */
static int a1fs_rename(const char *from, const char *to)
{
	fs_ctx *fs = get_fs();

	//TODO: move the inode (file or directory) at given source path to the
	// destination path, according to the description above
	(void)from;
	(void)to;
	(void)fs;
	return -ENOSYS;
}


/**
 * Change the access and modification times of a file or directory.
 *
 * Implements the utimensat() system call. See "man 2 utimensat" for details.
 *
 * NOTE: You only have to implement the setting of modification time (mtime).
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists.
 *
 * @param path  path to the file or directory.
 * @param tv    timestamps array. See "man 2 utimensat" for details.
 * @return      0 on success; -errno on failure.
 */
static int a1fs_utimens(const char *path, const struct timespec tv[2])
{
	fs_ctx *fs = get_fs();

	//TODO: update the modification timestamp (mtime) in the inode for given
	// path with either the time passed as argument or the current time,
	// according to the utimensat man page
	(void)path;
	(void)tv;
	(void)fs;
	return -ENOSYS;
}

/**
 * Change the size of a file.
 *
 * Implements the truncate() system call. Supports both extending and shrinking.
 * If the file is extended, future reads from the new uninitialized range must
 * return ranges filled with zeros.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a file.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a malloc() call failed).
 *   ENOSPC  not enough free space in the file system.
 *
 * @param path  path to the file to set the size.
 * @param size  new file size in bytes.
 * @return      0 on success; -errno on error.
 */
static int a1fs_truncate(const char *path, off_t size)
{
	fs_ctx *fs = get_fs();

	//TODO: set new file size, possibly "zeroing out" the uninitialized range
	(void)path;
	(void)size;
	(void)fs;
	return -ENOSYS;
}


/**
 * Read data from a file.
 *
 * Implements the pread() system call. Should return exactly the number of bytes
 * requested except on EOF (end of file) or error, otherwise the rest of the
 * data will be substituted with zeros. Reads from file ranges that have not
 * been written to must return ranges filled with zeros.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a file.
 *
 * @param path    path to the file to read from.
 * @param buf     pointer to the buffer that receives the data.
 * @param size    buffer size (number of bytes requested).
 * @param offset  offset from the beginning of the file to read from.
 * @param fi      unused.
 * @return        number of bytes read on success; 0 if offset is beyond EOF;
 *                -errno on error.
 */
static int a1fs_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi)
{
	(void)fi;// unused
	fs_ctx *fs = get_fs();

	//TODO: read data from the file at given offset into the buffer
	(void)path;
	(void)buf;
	(void)size;
	(void)offset;
	(void)fs;
	return -ENOSYS;
}

/**
 * Write data to a file.
 *
 * Implements the pwrite() system call. Should return exactly the number of
 * bytes requested except on error. If the offset is beyond EOF (end of file),
 * the file must be extended. If the write creates a "hole" of uninitialized
 * data, future reads from the "hole" must return ranges filled with zeros.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a file.
 *
 * @param path    path to the file to write to.
 * @param buf     pointer to the buffer containing the data.
 * @param size    buffer size (number of bytes requested).
 * @param offset  offset from the beginning of the file to write to.
 * @param fi      unused.
 * @return        number of bytes written on success; -errno on error.
 */
static int a1fs_write(const char *path, const char *buf, size_t size,
                      off_t offset, struct fuse_file_info *fi)
{
	(void)fi;// unused
	fs_ctx *fs = get_fs();

	//TODO: write data from the buffer into the file at given offset, possibly
	// "zeroing out" the uninitialized range
	(void)path;
	(void)buf;
	(void)size;
	(void)offset;
	(void)fs;
	return -ENOSYS;
}


static struct fuse_operations a1fs_ops = {
	.destroy  = a1fs_destroy,
	.statfs   = a1fs_statfs,
	.getattr  = a1fs_getattr,
	.readdir  = a1fs_readdir,
	.mkdir    = a1fs_mkdir,
	.rmdir    = a1fs_rmdir,
	.create   = a1fs_create,
	.unlink   = a1fs_unlink,
	.rename   = a1fs_rename,
	.utimens  = a1fs_utimens,
	.truncate = a1fs_truncate,
	.read     = a1fs_read,
	.write    = a1fs_write,
};

int main(int argc, char *argv[])
{
	a1fs_opts opts = {0};// defaults are all 0
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	if (!a1fs_opt_parse(&args, &opts)) return 1;

	fs_ctx fs = {0};
	if (!a1fs_init(&fs, &opts)) {
		fprintf(stderr, "Failed to mount the file system\n");
		return 1;
	}

	return fuse_main(args.argc, args.argv, &a1fs_ops, &fs);
}
