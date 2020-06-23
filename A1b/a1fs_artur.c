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
 * Find the index of an available spot on the block or inode bitmap
 * 
 * @param image	the disk image
 * @param type	the desired spot; 0 for block 1 for inode
 * @return		the index of the available spot; -1 if no available space
 */
int find_available_space(void *image, int type) {

	struct a1fs_superblock *superblock = (a1fs_superblock*)(image);
	int index = -1;
	if (type) {
		unsigned char* inode_bitmap;
		for (int i = 0; (unsigned int)i < superblock->inode_bitmap_span; i++) {
			inode_bitmap = (unsigned char*)(image + (A1FS_BLOCK_SIZE * (superblock->inode_bitmap + i)));

			if ((unsigned int)i == superblock->inode_bitmap_span - 1) {
				for (int j = 0; (unsigned int)j < superblock->inodes_count - (i * A1FS_BLOCK_SIZE); j++) {
					if (!inode_bitmap[j]) {
						index = i * A1FS_BLOCK_SIZE + j;
						return index;
					}
				}
			}
			else {
				for (int j = 0; (unsigned int)j < 4096; j++) {
					if (!inode_bitmap[j]) {
						index = i * A1FS_BLOCK_SIZE + j;
						return index;
					}
				}
			}
			
		}
	}
	else {
		unsigned char* block_bitmap;
		for (int i = 0; (unsigned int)i < superblock->block_bitmap_span; i++) {
			block_bitmap = (unsigned char*)(image + (A1FS_BLOCK_SIZE * (superblock->block_bitmap + i)));

			//alternate for loop for when we are on the final bitmap block which doesn't necessarily have all 4096 bits
			if ((unsigned int)i == superblock->block_bitmap_span - 1) {
				for (int j = 0; (unsigned int)j < superblock->blocks_count - (i * A1FS_BLOCK_SIZE); j++) {
					if (!block_bitmap[j]) {
						index = i * A1FS_BLOCK_SIZE + j;
						return index;
					}
				}
			}
			else {
				for (int j = 0; (unsigned int)j < 4096; j++) {
					if (!block_bitmap[j]) {
						index = i * A1FS_BLOCK_SIZE + j;
						return index;
					}
				}
			}

		}
	}
	return -1;
}


/**
 * Allocates a new block for an a1fs_inode
 * 
 * @param inode	the inode to give a new block in
 * @param image the disk image
 * @return 		the index of the edited extent; -1 on error (e.g. no space available)
 */
int allocate_new_block(struct a1fs_inode **inode, void *image) {

	struct a1fs_inode *modify = *inode;
	struct a1fs_superblock *superblock = (struct a1fs_superblock*)(image);
	unsigned char *block_bitmap = (unsigned char*)(image + (A1FS_BLOCK_SIZE * superblock->block_bitmap));
	unsigned char *blocks = (unsigned char*)(image + (A1FS_BLOCK_SIZE * superblock->data_region));

	int block_index = find_available_space(image, 0);
	if (block_index == -1) {											//since there are no available blocks left
		return -1;
	}

	//loop over extents in inode
	for (int i = 0; (unsigned int)i < A1FS_EXTENTS_LENGTH; i++) {
		
		if (i != A1FS_EXTENTS_LENGTH - 1) {
			
			if (modify->extent[i].count != 0) {
				if (block_bitmap[modify->extent[i].start + modify->extent[i].count] == 0) {				//found a free block and can extend the extent
					block_bitmap[modify->extent[i].start + modify->extent[i].count] = 1;
					modify->extent[i].count += 1;
					superblock->free_blocks_count -= 1;
					return i;
				}
			}
			else {												//case where this extent is empty and was never assigned any blocks
				struct a1fs_extent extent;
				extent.start = block_index;
				extent.count = 1;
				modify->extent[i] = extent;
				block_bitmap[block_index] = 1;
				superblock->free_blocks_count -= 1;
				return i;
			}
		}
		else {													//case where we need to allocate a new block in the indirect block

			int extent_num = 10;								//variable keeping track of how many extents have been iterated over in indirect blocks
																//initialized to 10 to indicate indirect block
			//loop through indirect blocks
			for (int j = 0; (unsigned int)j < modify->extent[i].count; j++) {
				unsigned char *indirect_blocks = (unsigned char*)(blocks + (A1FS_BLOCK_SIZE * (modify->extent[i].start + j)));

				//loop through extents inside indirect blocks
				for (int k = 0; (unsigned int)k < A1FS_BLOCK_SIZE / sizeof(a1fs_extent); k++) {
					struct a1fs_extent *extent = (struct a1fs_extent*)(indirect_blocks + (sizeof(a1fs_extent) * k));
					extent_num += 1;

					if (extent->count == 0) {					//case where this extent is empty and was never assigned any blocks
						struct a1fs_extent save_extent;
						struct a1fs_extent *save_extent_pointer = &save_extent;
						save_extent_pointer->count = 1;
						save_extent_pointer->start = block_index;
						block_bitmap[block_index] = 1;
						superblock->free_blocks_count -= 1;
						memcpy(extent, save_extent_pointer, sizeof(a1fs_extent));
						return extent_num;
					}
					else {
						if (block_bitmap[extent->start + extent->count] == 0) {
							block_bitmap[extent->start + extent->count] = 1;
							extent->count += 1;
							superblock->free_blocks_count -= 1;
							return extent_num;
						}
					}
				}
			}
			//if we reach this point without returning, should we even try to extend the indirect block extent?
		}
	}

	return -1;
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

	unsigned char *disk = fs->image;
	struct a1fs_superblock *superblock = (struct a1fs_superblock*)(disk);

	//check to see if there is available space for a new directory
	if (superblock->free_inodes_count == 0 || superblock->free_blocks_count <= 1) {
		return -ENOSPC;
	}

	unsigned char *block_bits = (unsigned char*)(disk + (A1FS_BLOCK_SIZE * superblock->block_bitmap));
	unsigned char *inode_bits = (unsigned char*)(disk + (A1FS_BLOCK_SIZE * superblock->inode_bitmap));
	int block_index = -1;
	int inode_index = -1;

	//determine which block index is free and mark it for the new directory's spot
	for (int i = 0; (unsigned int)i < superblock->block_bitmap_span; i++) {
		block_bits = (unsigned char*)(disk + (A1FS_BLOCK_SIZE * superblock->block_bitmap * i));

		//alternate for loop for when we are on the final bitmap block which doesn't necessarily have all 4096 bits
		if ((unsigned int)i == superblock->block_bitmap_span - 1) {			
			for (int j = 0; (unsigned int)j < superblock->blocks_count - (i * A1FS_BLOCK_SIZE); j++){
				if (!block_bits[j]) {
					block_bits[j] = 1;
					block_index = i * A1FS_BLOCK_SIZE + j;
				}
			}
		}
		else {
			for (int j = 0; j < 4096; j++) {
				if (!block_bits[j]) {
					block_bits[j] = 1;
					block_index = i * A1FS_BLOCK_SIZE + j;
				}
			}
		}
		if (block_index >= 0) {
			break;
		}
	}

	//determine which inode index is free and mark it for the new directory's spot
	for (int i = 0; (unsigned int)i < superblock->inode_bitmap_span; i++) {
		inode_bits = (unsigned char*)(disk + (A1FS_BLOCK_SIZE * superblock->inode_bitmap * i));

		if ((unsigned int)i == superblock->inode_bitmap_span - 1) {
			for (int j = 0; (unsigned int)j < superblock->inodes_count - (i * A1FS_BLOCK_SIZE); j++){
				if (!inode_bits[j]) {
					inode_bits[j] = 1;
					inode_index = i * A1FS_BLOCK_SIZE + j;
				}
			}
		}
		for (int j = 0; j < 4096; j++) {
			if (!inode_bits[j]) {
				inode_bits[j] = 1;
				inode_index = i * A1FS_BLOCK_SIZE + j;
				break;
			}
		}
		if (inode_index >= 0) {
			break;
		}
	}
	superblock->free_inodes_count -= 1;
	superblock->free_blocks_count -= 1;

	//create the new inode for this directory
	struct a1fs_inode inode;
	struct a1fs_inode *inode_pointer = &inode;
	inode_pointer->mode = mode | S_IFDIR; 														//NOT SURE WHAT THIS DOES BUT THE STARTER CODE COMMENTS SAY TO DO IT
	inode_pointer->links = 1;
	inode_pointer->size = 0;																	//MIGHT NEED TO CHANGE SIZE AND LINKS BC IDK WHAT THE INITIAL VALUES SHOULD BE
	clock_gettime(CLOCK_REALTIME, &inode_pointer->mtime);
	inode_pointer->extents = 1;
	inode_pointer->dentry = 0;
	(inode_pointer->extent)[0].start = block_index;
	(inode_pointer->extent)[0].count = 1;

	//add the new inode to the inode table
	unsigned char *inode_table = (unsigned char*)(disk + (A1FS_BLOCK_SIZE * superblock->inode_table));
	inode_table = (unsigned char*)(inode_table + (sizeof(a1fs_inode) * inode_index));
	memcpy(inode_table, inode_pointer, sizeof(a1fs_inode));

	//find the parent directory and add in a new dentry representing this new directory
	char *temp = malloc(sizeof(char) * strlen(path));													
	strcpy(temp, path);													//copy path to temporary variable
	char *final_slash = strrchr(temp, '/');								//get the pointer to the final "/"
	char *directory_name = malloc(sizeof(char) * strlen(final_slash));						
	strcpy(directory_name, (final_slash + sizeof(char)));				//copy the name of the directory to variable
	*final_slash = '\0';												//assign value of final slash to '\0'

	struct a1fs_inode *inodes = (struct a1fs_inode*)(fs->image + A1FS_BLOCK_SIZE * superblock->inode_table);
	struct a1fs_inode *root_inode = inodes + A1FS_ROOT_INO;
	struct a1fs_inode *target = (void *)0;
	inode_from_path(root_inode, &target, temp, fs->image);

	target->links += 1;																			//AGAIN, HOW DO LINKS WORK?
	clock_gettime(CLOCK_REALTIME, &target->mtime);
	target->dentry += 1;

	struct a1fs_dentry dentry;
	struct a1fs_dentry *dentry_pointer = &dentry;
	dentry_pointer->ino = inode_index;
	strcpy(dentry_pointer->name, directory_name);

	//determine which extent block has free space for writing into
	unsigned char *block_table = (unsigned char*)(disk + (A1FS_BLOCK_SIZE * superblock->data_region));
	int written = 0;
	//loop through array of extents in this inode
	for (int i = 0; i < A1FS_EXTENTS_LENGTH; i++) {
		if (i != A1FS_EXTENTS_LENGTH - 1) {
			
			//loop through the blocks of this one extent
			for (int j = 0; (unsigned int)j < target->extent[i].count; j++) {
				unsigned char *blocks = (unsigned char*)(block_table + (A1FS_BLOCK_SIZE * j));

				//loop through the dentrys of this block
				for (int k = 0; (unsigned int)k < A1FS_BLOCK_SIZE / sizeof(a1fs_dentry); k++) {
					struct a1fs_dentry *directory_dentry = (struct a1fs_dentry*)(blocks + (sizeof(a1fs_dentry) * k));
					if (directory_dentry->ino == 0) {
						memcpy(directory_dentry, dentry_pointer, sizeof(a1fs_dentry));
						written = 1;
						break;
					}
				}
				if (written) {
					break;
				}
			}
		}
		else {																					//CASE WHERE AVAILABLE SPACE IS IN THE INDIRECT BLOCK

			//loop through the indirect blocks
			for (int j = 0; (unsigned int)j < target->extent[i].count; j++) {
				unsigned char *blocks = (unsigned char*)(block_table + (A1FS_BLOCK_SIZE * j));

				//loop through the extents inside the indirect blocks
				for (int k = 0; (unsigned int)k < A1FS_BLOCK_SIZE / sizeof(a1fs_extent); k++) {
					struct a1fs_extent *extents = (struct a1fs_extent*)(blocks + (sizeof(a1fs_extent) * k));

					//loop through the blocks inside the extents
					for (int l = 0; (unsigned int)l < extents->count; l++) {
						unsigned char *extent_blocks = (unsigned char*)(block_table + (A1FS_BLOCK_SIZE * (extents->start + l)));

						//loop through the dentrys inside the blocks
						for (int m = 0; (unsigned int)m < A1FS_BLOCK_SIZE / sizeof(a1fs_dentry); m++) {
							struct a1fs_dentry *directory_dentry = (struct a1fs_dentry*)(extent_blocks + (sizeof(a1fs_dentry) * m));
							if (directory_dentry->ino == 0) {
								memcpy(directory_dentry, dentry_pointer, sizeof(a1fs_dentry));
								written = 1;
								break;
							}
						}
						if (written) {
							break;
						}
					}
					if (written) {
						break;
					}
				}
				if (written) {
					break;
				}
			}
		}
		if (written) {
			break;
		}
	}

	if (written == 0) {
		//int num = allocate_new_block(**inode);				//TODO CREATE A FUNCTION THAT ALLOCATES A NEW BLOCK TO AN INODE AND RETURNS THE INDEX OF THE EXTENT IT EDITED
	}


	//update_sizes(path, size);													//TODO CREATE A FUNCTION TO UPDATE SIZE OF DIRECTORIES
	free(temp);
	free(directory_name);
	return 0;
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

	unsigned char *disk = fs->image;
	struct a1fs_superblock *superblock = (struct a1fs_superblock*)(disk);
	struct a1fs_inode *inodes = (struct a1fs_inode*)(fs->image + A1FS_BLOCK_SIZE * superblock->inode_table);
	struct a1fs_inode *root_inode = inodes + A1FS_ROOT_INO;
	struct a1fs_inode *target = (void *)0;
	inode_from_path(root_inode, &target, path, fs->image);

	//check if directory is empty or not
	if (target->size != 0) {
		return -ENOTEMPTY;
	}

	//free the blocks this directory owns on the block bitmap
	unsigned char *block_bitmap = (unsigned char*)(disk + (A1FS_BLOCK_SIZE * superblock->block_bitmap));
	for (int i = 0; i < A1FS_EXTENTS_LENGTH; i++) {
		if (i != A1FS_EXTENTS_LENGTH - 1) {
			for (int j = 0; (unsigned int)j < target->extent[i].count; j++) {
				block_bitmap[target->extent[i].start + j] = 0;
				superblock->free_blocks_count += 1;
			}
		}
		else {																	//case where we need to free blocks in the indirect block
			unsigned char *indirect_block_start = block_bitmap + (A1FS_BLOCK_SIZE * target->extent[i].start);
			//loop through the indirect blocks
			for (int j = 0; (unsigned int)j < target->extent[i].count; j++) {
				unsigned char *indirect_block = indirect_block_start + (A1FS_BLOCK_SIZE * j);

				//loop through extents in the indirect blocks
				for (int k = 0; (unsigned int)k < A1FS_BLOCK_SIZE / sizeof(a1fs_extent); k++) {
					struct a1fs_extent *extent = (a1fs_extent*)(indirect_block + (sizeof(a1fs_extent) * k));

					//loop through the blocks in the extents
					for (int l = 0; (unsigned int)l < extent->count; l++) {
						block_bitmap[extent->start + l] = 0;
						superblock->free_blocks_count += 1;
					}
					
					memset(extent, 0, sizeof(a1fs_extent));						//reset the value to default since directory is being deleted
				}

			}
		}
	}

	//find the inode for the directory containing the directory to be removed and remove the dentry
	struct a1fs_inode *target_directory = (void *)0;
	char *temp = malloc(sizeof(char) * strlen(path));													
	strcpy(temp, path);															//copy path to temporary variable
	char *final_slash = strrchr(temp, '/');										//get the pointer to the final "/"
	char *directory_name = malloc(sizeof(char) * strlen(final_slash));						
	strcpy(directory_name, (final_slash + sizeof(char)));						//copy the name of the directory to variable
	*final_slash = '\0';														//assign value of final slash to '\0'
	inode_from_path(root_inode, &target_directory, temp, fs->image);

	unsigned char *block_table = (unsigned char*)(disk + (A1FS_BLOCK_SIZE * superblock->data_region));
	int inode_index = -1;

	//loop through the extents in the inode extent array
	for (int i = 0; i < A1FS_EXTENTS_LENGTH; i++) {
		if (i != A1FS_EXTENTS_LENGTH - 1) {

			//loop through the number of blocks contained in each extent
			for (int j = 0; (unsigned int)j < target->extent[i].count; j++) {
				unsigned char *block = block_table + ((target->extent[i].start + j) * A1FS_BLOCK_SIZE);

				//loop through all the dentrys contained in one block
				for (int k = 0; (unsigned int)k < A1FS_BLOCK_SIZE / sizeof(a1fs_dentry); k++) {
					//find the dentry with the name of the directory to be removed
					struct a1fs_dentry *dentry = (struct a1fs_dentry*)(block + (sizeof(a1fs_dentry) * k));
					if (!strcmp(dentry->name, directory_name)) {
						inode_index = dentry->ino;
						dentry->ino = 0;
						dentry->name[0] = '\0';
						//memset(dentry, 0, sizeof(a1fs_dentry));
						break;
					}
				}
				if (inode_index >= 0) {
					break;
				}
			}
			if (inode_index >= 0) {
				break;
			}
		}
		else {																	//case where dentry is stored in the indirect block

			unsigned char *indirect_block_start = block_bitmap + (A1FS_BLOCK_SIZE * target->extent[i].start);
			//loop through the indirect blocks
			for (int j = 0; (unsigned int)j < target->extent[i].count; j++) {
				unsigned char *indirect_block = indirect_block_start + (A1FS_BLOCK_SIZE * j);

				//loop through extents in the indirect blocks
				for (int k = 0; (unsigned int)k < A1FS_BLOCK_SIZE / sizeof(a1fs_extent); k++) {
					struct a1fs_extent *extent = (a1fs_extent*)(indirect_block + (sizeof(a1fs_extent) * k));

					//loop through the blocks in the extents
					for (int l = 0; (unsigned int)l < extent->count; l++) {
						unsigned char *extent_blocks = (unsigned char*)(block_table + (A1FS_BLOCK_SIZE * (extent->start + l)));

						//loop through the dentrys in the blocks
						for (int m = 0; (unsigned int)m < A1FS_BLOCK_SIZE / sizeof(a1fs_dentry); m++) {
							struct a1fs_dentry *dentry = (struct a1fs_dentry*)(extent_blocks + (sizeof(a1fs_dentry) * m));
							if (!strcmp(dentry->name, directory_name)) {
								inode_index = dentry->ino;
								dentry->ino = 0;
								dentry->name[0] = '\0';
								//memset(dentry, 0, sizeof(a1fs_dentry));
								break;
							}
						}
						if (inode_index >= 0) {
							break;
						}
					}
					if (inode_index >= 0) {
						break;
					}
				}
				if (inode_index >= 0) {
					break;
				}
			}
		}
		if (inode_index >= 0) {
			break;
		}
	}			

	//free up the space in the inode bitmap and inode table
	unsigned char *inode_bitmap = (unsigned char*)(disk + (A1FS_BLOCK_SIZE * superblock->inode_bitmap));
	inode_bitmap[inode_index] = 0;
	superblock->free_inodes_count += 1;	

	unsigned char *inode_table = (unsigned char*)(disk + (A1FS_BLOCK_SIZE * superblock->inode_table));
	memset(inode_table + (inode_index * sizeof(a1fs_inode)), 0, sizeof(a1fs_inode));					//remove inode by setting data to default 0s

	free(temp);
	free(directory_name);
	return 0;
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

	struct a1fs_superblock *superblock = (struct a1fs_superblock*)(fs->image);
	struct a1fs_inode *inodes = (struct a1fs_inode*)(fs->image + A1FS_BLOCK_SIZE * superblock->inode_table);
	struct a1fs_inode *root_inode = inodes + A1FS_ROOT_INO;
	unsigned char *block_start = (unsigned char*)(fs->image + (A1FS_BLOCK_SIZE * superblock->data_region));
	unsigned char *inode_start = (unsigned char*)(fs->image + (A1FS_BLOCK_SIZE * superblock->inode_table));
	unsigned char *inode_bitmap = (unsigned char*)(fs->image + (A1FS_BLOCK_SIZE * superblock->inode_bitmap)); 

	//check to see if there is available space for a new directory
	if (superblock->free_inodes_count == 0) {
		return -ENOSPC;
	}

	//find parent directory to create new file in
	struct a1fs_inode *parent_directory = (void *)0;
	char path_copy[A1FS_NAME_MAX];
	strcpy(path_copy, path);
	char *final_slash = strrchr(path_copy, '/');
	*final_slash = '\0';
	char file_name[A1FS_NAME_MAX];
	strcpy(file_name, (final_slash + 1));
	inode_from_path(root_inode, &parent_directory, path_copy, fs->image);

	//find a spot in one of the parent's directory blocks to create a new dentry in
	int created_dentry = 0;
	int inode_index = find_available_space(fs->image, 1);
	//loop through parent directory's extents
	for (int i = 0; (unsigned int)i < A1FS_EXTENTS_LENGTH; i++) {
		if ((unsigned int)i != A1FS_EXTENTS_LENGTH - 1) {

			//loop through dentrys in extent blocks
			for (int j = 0; (unsigned int)j < parent_directory->extent[i].count * A1FS_BLOCK_SIZE / sizeof(a1fs_dentry); j++) {
				struct a1fs_dentry *dentry = (struct a1fs_dentry*)(block_start + (A1FS_BLOCK_SIZE * parent_directory->extent[i].start) + (sizeof(a1fs_dentry) * j));

				if (dentry->ino == 0) {			//found spot to place new dentry; might need to also check name
					struct a1fs_dentry file;
					strcpy(file.name, file_name);
					file.ino = inode_index;
					memcpy(dentry, &file, sizeof(a1fs_dentry));
					created_dentry = 1;
					break;
				}
			}
		}
		else {		//case where no space available in direct blocks

			//loop through extents inside indirect blocks
			for (int j = 0; (unsigned int)j < (parent_directory->extent[i].count * A1FS_BLOCK_SIZE / sizeof(a1fs_extent)); j++) {
				struct a1fs_extent *indirect_extent = (struct a1fs_extent*)(block_start + (A1FS_BLOCK_SIZE * parent_directory->extent[i].start) + (sizeof(a1fs_extent) * j));

				//loop through dentrys inside extent
				for (int k = 0; (unsigned int)k < indirect_extent->count * A1FS_BLOCK_SIZE / sizeof(a1fs_dentry); k++) {
					struct a1fs_dentry *dentry = (struct a1fs_dentry*)(block_start + (A1FS_BLOCK_SIZE * indirect_extent->start) + (sizeof(a1fs_dentry) * k));

					if (dentry->ino == 0) {			//found spot to place new dentry
					struct a1fs_dentry file;
					strcpy(file.name, file_name);
					file.ino = inode_index;
					memcpy(dentry, &file, sizeof(a1fs_dentry));
					created_dentry = 1;
					break;
					}
				}
				if (created_dentry) {
					break;
				}
			}
		}
		if (created_dentry) {
			break;
		}
	}

	//failed to create a new dentry possibly because there was no more space in the parent directory
	if (!created_dentry) {
		if (superblock->free_blocks_count == 0) {	//no space to allocate new block to the parent directory
			return -ENOSPC;
		}

		//allocate a new block into the directory to put in a new dentry for the new file
		int status = allocate_new_block(&parent_directory, fs->image);
		if (status == -1) {
			return -ENOSPC;
		}

		if (status == 10) {			//allocated block is in the indirect extent

			//TODO

		}
		else {
			struct a1fs_dentry dentry;
			dentry.ino = inode_index;
			strcpy(dentry.name, file_name);
			memcpy((block_start + (A1FS_BLOCK_SIZE * (parent_directory->extent[status].start + parent_directory->extent[status].count - 1))), &dentry, sizeof(a1fs_dentry));
		}

	}

	//create the inode for the new file and save it to the inode table
	struct a1fs_inode inode;
	inode.mode = mode;
	inode.size = 0; 									//currently no data inside the file
	inode.links = 1;									//AGAIN IDK HOW LINKS WORK NEED TO EDIT
	inode.extents = 0;									//will not allocate any blocks until file actually gets written into
	clock_gettime(CLOCK_REALTIME, &inode.mtime);
	memcpy((inode_start + (sizeof(a1fs_inode) * inode_index)), &inode, sizeof(a1fs_inode));
	inode_bitmap[inode_index] = 1;
	superblock->free_inodes_count -= 1;
	
	return 0;
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
	
	//locate the path inode
	struct a1fs_superblock *superblock = (struct a1fs_superblock*)(fs->image);
	struct a1fs_inode *inodes = (struct a1fs_inode*)(fs->image + A1FS_BLOCK_SIZE * superblock->inode_table);
	struct a1fs_inode *root_inode = inodes + A1FS_ROOT_INO;
	struct a1fs_inode *target = (void *)0;
	inode_from_path(root_inode, &target, path, fs->image);

	unsigned char *block_bitmap = (unsigned char*)(fs->image + (A1FS_BLOCK_SIZE * superblock->block_bitmap)); 
	unsigned char *block_start = (unsigned char*)(fs->image + (A1FS_BLOCK_SIZE * superblock->data_region));
	//free the blocks that this inode owns
	//loop through extent array in inode
	for (int i = 0; (unsigned int)i < A1FS_EXTENTS_LENGTH; i++) {
		if ((unsigned int)i != A1FS_EXTENTS_LENGTH - 1) {					
			
			//loop through blocks in the extents
			for (int j = 0; (unsigned int)j < target->extent[i].count; j++) {
				block_bitmap[target->extent[i].start + j] = 0;
				superblock->free_blocks_count += 1;
				memset((block_start + (A1FS_BLOCK_SIZE * (target->extent[i].start + j))), 0, A1FS_BLOCK_SIZE);
			}
		}
		else {	//case where we need to free blocks in the indirect block

			//loop through extents inside indirect blocks
			for (int j = 0; (unsigned int)j < (target->extent[i].count * A1FS_BLOCK_SIZE / sizeof(a1fs_extent)); j++) {
				struct a1fs_extent *indirect_extent = (struct a1fs_extent*)(block_start + (A1FS_BLOCK_SIZE * target->extent[i].start) + (sizeof(a1fs_extent) * j));

				//loop through blocks inside indirect extent
				for (int k = 0; (unsigned int)k < indirect_extent->count; k++) {
					block_bitmap[indirect_extent->start + j] = 0;
					superblock->free_blocks_count += 1;
					memset((block_start + (A1FS_BLOCK_SIZE * (indirect_extent->start + j))), 0, A1FS_BLOCK_SIZE);
				}
			}
		}
	}

	//locate the parent directory inode and remove the dentry and inode for this file
	struct a1fs_inode *parent_directory = (void *)0;
	char path_copy[A1FS_NAME_MAX];
	strcpy(path_copy, path);
	char *final_slash = strrchr(path_copy, '/');
	*final_slash = '\0';
	char file_name[A1FS_NAME_MAX];
	strcpy(file_name, (final_slash + 1));
	inode_from_path(root_inode, &parent_directory, path_copy, fs->image);
	parent_directory->size -= target->size;														//MIGHT BE UNNECESSARY IF TRUNCATE GETS CALLED BY DEFAULT

	unsigned char* inode_bitmap = (unsigned char*)(fs->image + (A1FS_BLOCK_SIZE * superblock->inode_bitmap));
	unsigned char* inode_start = (unsigned char*)(fs->image + (A1FS_BLOCK_SIZE * superblock->inode_table));
	//loop through parent directory's extents
	for (int i = 0; (unsigned int)i < A1FS_EXTENTS_LENGTH; i++) {
		
		if ((unsigned int)i != A1FS_EXTENTS_LENGTH - 1) {					
			
			//loop through dentrys in the blocks of the extent
			for (int j = 0; (unsigned int)j < parent_directory->extent[i].count * A1FS_BLOCK_SIZE / sizeof(a1fs_dentry); j++) {
				struct a1fs_dentry *dentry = (struct a1fs_dentry*)(block_start + (A1FS_BLOCK_SIZE * parent_directory->extent[i].start) + (sizeof(a1fs_dentry) * j));

				if (!strcmp(dentry->name, file_name)) {
					memset(inode_start + (sizeof(a1fs_inode) * (dentry->ino)), 0, sizeof(a1fs_inode));
					inode_bitmap[dentry->ino] = 0;
					memset(dentry, 0, sizeof(a1fs_dentry));
					superblock->free_inodes_count += 1;
					return 0;
				}
			}
		}
		else {	//case where dentry is located in indirect block

			//loop through extents inside indirect blocks
			for (int j = 0; (unsigned int)j < (parent_directory->extent[i].count * A1FS_BLOCK_SIZE / sizeof(a1fs_extent)); j++) {
				struct a1fs_extent *indirect_extent = (struct a1fs_extent*)(block_start + (A1FS_BLOCK_SIZE * target->extent[i].start) + (sizeof(a1fs_extent) * j));

				//loop through dentrys inside indirect extent
				for (int k = 0; (unsigned int)k < indirect_extent->count * A1FS_BLOCK_SIZE / sizeof(a1fs_dentry); k++) {
					struct a1fs_dentry *dentry = (struct a1fs_dentry*)(block_start + (A1FS_BLOCK_SIZE * indirect_extent->start) + (sizeof(a1fs_dentry) * k));

					if (!strcmp(dentry->name, file_name)) {
						memset(inode_start + (sizeof(a1fs_inode) * (dentry->ino)), 0, sizeof(a1fs_inode));
						inode_bitmap[dentry->ino] = 0;
						memset(dentry, 0, sizeof(a1fs_dentry));
						superblock->free_inodes_count += 1;
						return 0;
					}
				}
			}
		}
	}

	return -ENOSYS;			//should never reach here from assumption
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
