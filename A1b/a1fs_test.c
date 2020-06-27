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
	struct a1fs_superblock *sb = (struct a1fs_superblock*)(fs->image);
	st->f_namemax = A1FS_NAME_MAX;
	st->f_blocks = sb->size / A1FS_BLOCK_SIZE;
	st->f_bfree = sb->free_blocks_count;
	st->f_bavail = st->f_bfree;
	st->f_files = sb->inodes_count;
	st->f_ffree = sb->free_inodes_count;
	st->f_favail = st->f_ffree;

	return 0;

	return -ENOSYS;
}

int inode_by_name(struct a1fs_inode *dir, struct a1fs_inode **file, char *name, void *image){
	struct a1fs_superblock *sb = (struct a1fs_superblock*)(image);
	struct a1fs_extent *curr_extent;
	int entry_count = 0;

	int extents_count = 0;
	int i = 0;
	while (extents_count < dir->extents){
		if (i >= A1FS_IND_BLOCK){
			// We are now looking in the indirect block.
			int extent_location = A1FS_BLOCK_SIZE*(sb->data_region + ((dir->extent)[A1FS_IND_BLOCK]).start) + (i-A1FS_IND_BLOCK)*sizeof(a1fs_extent);
			curr_extent = (struct a1fs_extent*)(image + extent_location);
		}
		else{
			// We are looking at the extent at index i in the extents array.
			curr_extent = &(dir->extent)[i];
		}
		if (curr_extent->count > 0){
			// Loop through this entire extent (depending on extent length).
			for (size_t j = 0; j < curr_extent->count; j++){
				int curr_entry_block = sb->data_region + curr_extent->start + j;

				// Loop through all the entries in this extent (while keeping track of entry count).
				for (size_t k = 0; k < A1FS_BLOCK_SIZE/sizeof(a1fs_dentry); k++){
					if (entry_count < dir->dentry){
						struct a1fs_dentry *curr_entry = (struct a1fs_dentry*)(image + A1FS_BLOCK_SIZE*curr_entry_block + k*sizeof(a1fs_dentry));

						// Check if this entry is not in use.
						if (curr_entry->ino == 0 && (curr_entry->name)[0] == '\0'){
							continue;
						}

						// Check if this is the entry we are looking for.
						printf("Entry: %s\n", curr_entry->name);
						if (strcmp(curr_entry->name, name) == 0){
							int inode_location = sb->inode_table*A1FS_BLOCK_SIZE + curr_entry->ino*sizeof(a1fs_inode);
							*file = (struct a1fs_inode*)(image + inode_location);
							return 0;
						}
					}
					entry_count++;
				}
			}
			extents_count++;
		}
		i ++;
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
					// This was the last file/dir in the path string so it must be the target.
					if (i == strlen(path) - 1){
						*file = target;
						return 0;
					// This is just an intermediate directory in the path.
					} else {
						// Check if this is a directory.
						if (S_ISDIR(target->mode)){
							printf("Intermediate dir: %s\n", currFile);
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

// Initialize a new inode to the default parameters and provided mode.
// TODO: links???
void init_inode(struct a1fs_inode *inode, mode_t mode){
	inode->mode = mode;
	inode->size = 0; 									
	inode->links = 1;									
	inode->extents = 0;	

	// Initialize the extents to be empty (count = 0).
	for (int i = 0; i < A1FS_EXTENTS_LENGTH; i ++){
		(inode->extent)[0].count = 0;
	}

	clock_gettime(CLOCK_REALTIME, &inode->mtime);
}

void init_extent(struct a1fs_extent *extent, int start, int count, void *image){
	struct a1fs_superblock *superblock = (struct a1fs_superblock*)(image);
	unsigned char *block_bitmap = (unsigned char*)(image + (A1FS_BLOCK_SIZE * superblock->block_bitmap));

	extent->start = start;
	extent->count = count;
	block_bitmap[start] = 1;
	superblock->free_blocks_count -= 1;
}


// Return -1 on error and return index of extent on success;
int append_new_block(struct a1fs_inode *inode, void *image){

	struct a1fs_superblock *sb = (struct a1fs_superblock*)(image);
	unsigned char *block_bitmap = (unsigned char*)(image + (A1FS_BLOCK_SIZE * sb->block_bitmap));


	int block_index = find_available_space(image, 0);
	if (block_index == -1){
		return -1;
	}

	int extents_count = 0;
	int i = 0;
	struct a1fs_extent *curr_extent;
	while (extents_count < inode->extents){
		if (i >= A1FS_IND_BLOCK){
			// We are now looking in the indirect block.
			int extent_location = A1FS_BLOCK_SIZE*(sb->data_region + ((inode->extent)[A1FS_IND_BLOCK]).start) + (i-A1FS_IND_BLOCK)*sizeof(a1fs_extent);
			curr_extent = (struct a1fs_extent*)(image + extent_location);
		}
		else{
			// We are looking at the extent at index i in the extents array.
			curr_extent = inode->extent + i;
		}
		if (curr_extent->count > 0){
			// Check if we can extend the last extent.
			if (extents_count == inode->extents - 1){
				if (block_bitmap[curr_extent->start + curr_extent->count] == 0) {				//found a free block and can extend the extent
					block_bitmap[curr_extent->start + curr_extent->count] = 1;
					curr_extent->count += 1;
					sb->free_blocks_count -= 1;
					return i;
				}
			}
			extents_count++;
		}
		i++;
	}

	// Two possible cases: 1. Could not extend last extent. 2. Inode has no extents.
	// In both scenarios we need to create a new extent at the end. 

	if (i >= A1FS_IND_BLOCK){
		int extent_location = A1FS_BLOCK_SIZE*(sb->data_region + ((inode->extent)[A1FS_IND_BLOCK]).start) + (i-A1FS_IND_BLOCK)*sizeof(a1fs_extent);
		curr_extent = (struct a1fs_extent*)(image + extent_location);
	}
	else{
		// We are looking at the extent at index i in the extents array.
		curr_extent = inode->extent + i;
	}

	init_extent(curr_extent, block_index, 1, image);
	inode->extents += 1;
	return i;
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
	//unsigned char *blocks = (unsigned char*)(image + (A1FS_BLOCK_SIZE * superblock->data_region));

	int block_index = find_available_space(image, 0);
	if (block_index == -1) {											//since there are no available blocks left
		return -1;
	}

	// Loop over all the extents.
	for (int i = 0; (unsigned int)i < A1FS_EXTENTS_LENGTH; i++) {
		
		if (i != A1FS_IND_BLOCK) {
			
			if (modify->extent[i].count != 0) {
				if (block_bitmap[modify->extent[i].start + modify->extent[i].count] == 0) {				//found a free block and can extend the extent
					block_bitmap[modify->extent[i].start + modify->extent[i].count] = 1;
					modify->extent[i].count += 1;
					superblock->free_blocks_count -= 1;
					return i;
				}
			}
			else {												//case where this extent is empty and was never assigned any blocks
				init_extent(modify->extent + i, block_index, 1, image);
				modify->extents += 1;
				return i;
			}
		}
		else {	
			// Need to check if the indirect block has been initialized.
			if ((modify->extent[A1FS_IND_BLOCK]).count == 0){
				// We need to initialize the independent block.
				int indirect_block_index = find_available_space(image, 0); 
				init_extent(modify->extent + A1FS_IND_BLOCK, indirect_block_index, 1, image);
			}

			int extent_num = A1FS_IND_BLOCK;								//variable keeping track of how many extents have been iterated over in indirect blocks
																//initialized to 10 to indicate indirect block
			//loop through indirect blocks
			for (int j = 0; (unsigned int)j < modify->extent[i].count; j++) {
				//unsigned char *indirect_blocks = (unsigned char*)(blocks + (A1FS_BLOCK_SIZE * (modify->extent[i].start + j)));
				int indirect_block = superblock->data_region + modify->extent[i].start + j;

				//loop through extents inside indirect blocks
				for (int k = 0; (unsigned int)k < A1FS_BLOCK_SIZE / sizeof(a1fs_extent); k++) {
					//struct a1fs_extent *extent = (struct a1fs_extent*)(indirect_blocks + (sizeof(a1fs_extent) * k));
					struct a1fs_extent *extent = (struct a1fs_extent*)(image + A1FS_BLOCK_SIZE*indirect_block + (sizeof(a1fs_extent)*k));

					if (extent->count == 0) {					//case where this extent is empty and was never assigned any blocks
						init_extent(extent, block_index, 1, image);
						modify->extents += 1;
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
					extent_num += 1;
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

	int extents_count = 0;
	int i = 0;
	while (extents_count < target->extents){
		if (i >= A1FS_IND_BLOCK){
			// We are now looking in the indirect block.
			int extent_location = A1FS_BLOCK_SIZE*(sb->data_region + ((target->extent)[A1FS_IND_BLOCK]).start) + (i-A1FS_IND_BLOCK)*sizeof(a1fs_extent);
			curr_extent = (struct a1fs_extent*)(fs->image + extent_location);
		}
		else{
			// We are looking at the extent at index i in the extents array.
			curr_extent = &(target->extent)[i];
		}
		if (curr_extent->count > 0){
			// Loop through this entire extent (depending on extent length).
			for (size_t j = 0; j < curr_extent->count; j++){
				int curr_entry_block = sb->data_region + curr_extent->start + j;

				// Loop through all the entries in this extent (while keeping track of entry count).
				for (size_t k = 0; k < A1FS_BLOCK_SIZE/sizeof(a1fs_dentry); k++){
					if (entry_count < target->dentry){
						struct a1fs_dentry *curr_entry = (struct a1fs_dentry*)(fs->image + A1FS_BLOCK_SIZE*curr_entry_block + k*sizeof(a1fs_dentry));

						// Check if this entry is not in use.
						if (curr_entry->ino == 0 && (curr_entry->name)[0] == '\0'){
							continue;
						}

						// Check if this is the entry we are looking for.
						if (filler(buf, curr_entry->name, NULL, 0)){
							return -ENOMEM;
						}
					}
					entry_count++;
				}
			}
			extents_count++;
		}
		i++;
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

	struct a1fs_superblock *superblock = (struct a1fs_superblock*)(fs->image);
	struct a1fs_inode *inodes = (struct a1fs_inode*)(fs->image + A1FS_BLOCK_SIZE * superblock->inode_table);
	struct a1fs_inode *root_inode = inodes + A1FS_ROOT_INO;
	//unsigned char *block_start = (unsigned char*)(fs->image + (A1FS_BLOCK_SIZE * superblock->data_region));
	//unsigned char *inode_start = (unsigned char*)(fs->image + (A1FS_BLOCK_SIZE * superblock->inode_table));
	unsigned char *inode_bitmap = (unsigned char*)(fs->image + (A1FS_BLOCK_SIZE * superblock->inode_bitmap)); 
	//unsigned char *block_bitmap = (unsigned char*)(fs->image + (A1FS_BLOCK_SIZE * superblock->block_bitmap)); 

	//check to see if there is space for an additional inode
	if (superblock->free_inodes_count == 0) {
		return -ENOSPC;
	}

	//find parent directory to insert new dentry in
	struct a1fs_inode *parent_directory = (void *)0;
	char path_copy[A1FS_NAME_MAX];
	strcpy(path_copy, path);
	char *final_slash = strrchr(path_copy, '/');
	*final_slash = '\0';
	char file_name[A1FS_NAME_MAX];
	strcpy(file_name, (final_slash + 1));
	printf("Path to parent: %s File name: %s\n", path_copy, file_name);

	// Base case for when we are creating a new dir in the root.
	if (strlen(path_copy) == 0){
		parent_directory = root_inode;
	}
	else {
		inode_from_path(root_inode, &parent_directory, path_copy, fs->image);
	}

	//find place in parent directory to insert new dentry into
	int created_dentry = 0;
	int inode_index = find_available_space(fs->image, 1);

	//FOR TESTING PURPOSES
	// block_bitmap[10] = 1;
	// parent_directory->extent[1].start = 10;
	// parent_directory->extent[1].count = 1;
	// parent_directory->extents++;
	// // TODO: When we assign a new block we need to memset that block to all 0.
	// unsigned char *extent_start = (unsigned char*)(fs->image + (A1FS_BLOCK_SIZE*(superblock->data_region + 10)));
	// memset(extent_start, 0, A1FS_BLOCK_SIZE);


	// Look through the parent's existing extents for free space.
	struct a1fs_extent *curr_extent;
	int extents_count = 0;
	int i = 0;
	while (extents_count < parent_directory->extents){
		if (i >= A1FS_IND_BLOCK){
			// We are now looking in the indirect block.
			int extent_location = A1FS_BLOCK_SIZE*(superblock->data_region + ((parent_directory->extent)[A1FS_IND_BLOCK]).start) + (i-A1FS_IND_BLOCK)*sizeof(a1fs_extent);
			curr_extent = (struct a1fs_extent*)(fs->image + extent_location);
		}
		else{
			// We are looking at the extent at index i in the extents array.
			curr_extent = parent_directory->extent + i;
		}
		if (curr_extent->count > 0){
	        // Loop through this entire extent (depending on extent length).
	        printf("Extent[%d] has size: %d\n", i, curr_extent->count);
			for (size_t j = 0; j < curr_extent->count; j++){
				int curr_entry_block = superblock->data_region + curr_extent->start + j;

				// Loop through all the entries in this extent.
				for (size_t k = 0; k < A1FS_BLOCK_SIZE/sizeof(a1fs_dentry); k++){
						struct a1fs_dentry *curr_entry = (struct a1fs_dentry*)(fs->image + A1FS_BLOCK_SIZE*curr_entry_block + k*sizeof(a1fs_dentry));

						// Check if this entry is not in use.
						printf("Extent[%d] dentry[%ld] has ino: %d\n", i, k, curr_entry->ino);
						if ((curr_entry->ino == 0 && (curr_entry->name)[0] == '\0') || curr_entry == NULL){
				            strcpy(curr_entry->name, file_name);
				            curr_entry->ino = inode_index;
				            created_dentry = 1;
				            break;
						}
				}
				if (created_dentry){
					break;
				}
			}
			if (created_dentry) {
				break;
			}
			extents_count++;
		}
		i++;
	}

	// TODO: The existing extents had no space available, need to assign more space to the parent dir.
	if (!created_dentry) {	
		int extent_index = allocate_new_block(&parent_directory, fs->image);
		if (extent_index == -1){
			return -ENOSPC;
		}

		struct a1fs_dentry *new_entry;

		// Check if the new block is part of an extent stored in the indirect block.
		if (extent_index >= A1FS_IND_BLOCK){
			int extent_block = (superblock->data_region + ((parent_directory->extent)[A1FS_IND_BLOCK]).start)*A1FS_BLOCK_SIZE;
			struct a1fs_extent *extent = (struct a1fs_extent*)(fs->image + extent_block + sizeof(a1fs_extent)*(extent_index % A1FS_IND_BLOCK));

			int entry_byte = (superblock->data_region + extent->start)*A1FS_BLOCK_SIZE;
			new_entry = (struct a1fs_dentry*)(fs->image + entry_byte + (extent->count - 1)*A1FS_BLOCK_SIZE);
		}
		else {
			int entry_byte = (superblock->data_region + ((parent_directory->extent)[extent_index]).start)*A1FS_BLOCK_SIZE;
			new_entry = (struct a1fs_dentry*)(fs->image + entry_byte + (((parent_directory->extent)[extent_index]).count - 1)*A1FS_BLOCK_SIZE);
		}

		strcpy(new_entry->name, file_name);
        new_entry->ino = inode_index;
	}
	
	//create the inode for the new directory and save it to the inode table
	struct a1fs_inode *inode = (struct a1fs_inode*)(fs->image + A1FS_BLOCK_SIZE*superblock->inode_table + inode_index*sizeof(a1fs_inode));
	init_inode(inode, mode | S_IFDIR);
	// inode->mode = mode | S_IFDIR;
	// inode->size = 0; 									//currently no data inside the directory
	// inode->links = 1;									//AGAIN IDK HOW LINKS WORK NEED TO EDIT
	// inode->extents = 0;									//will not allocate any blocks until directory actually gets written into
	// clock_gettime(CLOCK_REALTIME, &inode->mtime);

	// Update
	inode_bitmap[inode_index] = 1;
	superblock->free_inodes_count -= 1;
	parent_directory->dentry++;

	return 0;
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
static int a1fs_rmdir(const char *path)						//TODO issue when maximum # of directories are made then removing the final one
{
	fs_ctx *fs = get_fs();

	//TODO: remove the directory at given path (only if it's empty)
	(void)path;
	(void)fs;

	struct a1fs_superblock *superblock = (struct a1fs_superblock*)(fs->image);
	struct a1fs_inode *inodes = (struct a1fs_inode*)(fs->image + A1FS_BLOCK_SIZE * superblock->inode_table);
	struct a1fs_inode *root_inode = inodes + A1FS_ROOT_INO;
	unsigned char *inode_bitmap = (unsigned char*)(fs->image + (A1FS_BLOCK_SIZE * superblock->inode_bitmap));
	unsigned char *block_bitmap = (unsigned char*)(fs->image + (A1FS_BLOCK_SIZE * superblock->block_bitmap));

	//find the directory to be removed
	struct a1fs_inode *directory = (void *)0;
	char path_copy[A1FS_NAME_MAX];
	strcpy(path_copy, path);
	inode_from_path(root_inode, &directory, path, fs->image);

	//check if directory is empty or not
	if (directory->size != 0 || directory->dentry != 0) {
		return -ENOTEMPTY;
	}

	//find the parent directory
	struct a1fs_inode *parent_directory = (void *)0;
	char *final_slash = strrchr(path_copy, '/');
	*final_slash = '\0';
	char file_name[A1FS_NAME_MAX];
	strcpy(file_name, (final_slash + 1));

	// Base case for when we are removing a directory in the root.
	if (strlen(path_copy) == 0){
		parent_directory = root_inode;
	}
	else {
		inode_from_path(root_inode, &parent_directory, path_copy, fs->image);
	}

	// Look through the directory's existing extents to free space
	struct a1fs_extent *curr_extent;
	int extents_count = 0;
	int i = 0;
	while (extents_count < directory->extents){
		if (i >= A1FS_IND_BLOCK){
			// We are now looking in the indirect block.
			int extent_location = A1FS_BLOCK_SIZE*(superblock->data_region + ((directory->extent)[A1FS_IND_BLOCK]).start) + (i-A1FS_IND_BLOCK)*sizeof(a1fs_extent);
			curr_extent = (struct a1fs_extent*)(fs->image + extent_location);
		}
		else{
			// We are looking at the extent at index i in the extents array.
			curr_extent = directory->extent + i;
		}
		if (curr_extent->count > 0){
	        // Loop through this entire extent (depending on extent length).
			for (size_t j = 0; j < curr_extent->count; j++){
				int curr_entry_block = superblock->data_region + curr_extent->start + j;
				block_bitmap[curr_entry_block] = 0;
				memset((fs->image + (A1FS_BLOCK_SIZE * curr_entry_block)), 0, A1FS_BLOCK_SIZE);			// TODO change what data we set erased blocks to
				superblock->free_blocks_count += 1;
			}
			extents_count++;
		}
		i++;
	}

	// Look through the parent directory's existing extents to locate the dentry for the removed directory
	extents_count = 0;
	i = 0;
	int removed = 0;
	int inode_num = -1;
	while (extents_count < parent_directory->extents){
		if (i >= A1FS_IND_BLOCK){
			// We are now looking in the indirect block.
			int extent_location = A1FS_BLOCK_SIZE*(superblock->data_region + ((parent_directory->extent)[A1FS_IND_BLOCK]).start) + (i-A1FS_IND_BLOCK)*sizeof(a1fs_extent);
			curr_extent = (struct a1fs_extent*)(fs->image + extent_location);
		}
		else{
			// We are looking at the extent at index i in the extents array.
			curr_extent = parent_directory->extent + i;
		}
		if (curr_extent->count > 0){
	        // Loop through this entire extent (depending on extent length).
			for (size_t j = 0; j < curr_extent->count; j++){
				int curr_entry_block = superblock->data_region + curr_extent->start + j;

				// Loop through all the entries in this extent.
				for (size_t k = 0; k < A1FS_BLOCK_SIZE/sizeof(a1fs_dentry); k++){
						struct a1fs_dentry *curr_entry = (struct a1fs_dentry*)(fs->image + A1FS_BLOCK_SIZE*curr_entry_block + k*sizeof(a1fs_dentry));

						// Check if this entry is the removed directory's
						if (curr_entry != NULL) {
							if (!strcmp(curr_entry->name, file_name)) {
								inode_num = curr_entry->ino;
								inode_bitmap[curr_entry->ino] = 0;
								memset(curr_entry, 0, sizeof(a1fs_dentry));				//TODO change what data we set erased blocks to
								superblock->free_inodes_count += 1;
								removed = 1;
								parent_directory->dentry -= 1;
								break;
							}
						}
				}
				if (removed){
					break;
				}
			}
			if (removed) {
				break;
			}
			extents_count++;
		}
		i++;
	}

	//remove the directory inode from the inode table		
	if (inode_num > 0) {
		memset((fs->image + (A1FS_BLOCK_SIZE * superblock->inode_table) + (sizeof(a1fs_inode) * inode_num)), 0, sizeof(a1fs_inode));
	}

	return 0;
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
	unsigned char *inode_bitmap = (unsigned char*)(fs->image + (A1FS_BLOCK_SIZE * superblock->inode_bitmap)); 
	
	//check to see if there is space for an additional inode
	if (superblock->free_inodes_count == 0) {
		return -ENOSPC;
	}

	//find parent directory to insert new dentry in
	struct a1fs_inode *parent_directory = (void *)0;
	char path_copy[A1FS_NAME_MAX];
	strcpy(path_copy, path);
	char *final_slash = strrchr(path_copy, '/');
	*final_slash = '\0';
	char file_name[A1FS_NAME_MAX];
	strcpy(file_name, (final_slash + 1));
	printf("Path to parent: %s File name: %s\n", path_copy, file_name);

	// Base case for when we are creating a new dir in the root.
	if (strlen(path_copy) == 0){
		parent_directory = root_inode;
	}
	else {
		inode_from_path(root_inode, &parent_directory, path_copy, fs->image);
	}

	//find place in parent directory to insert new dentry into
	int created_dentry = 0;
	int inode_index = find_available_space(fs->image, 1);

	// Look through the parent's existing extents for free space.
	struct a1fs_extent *curr_extent;
	int extents_count = 0;
	int i = 0;
	while (extents_count < parent_directory->extents){
		if (i >= A1FS_IND_BLOCK){
			// We are now looking in the indirect block.
			int extent_location = A1FS_BLOCK_SIZE*(superblock->data_region + ((parent_directory->extent)[A1FS_IND_BLOCK]).start) + (i-A1FS_IND_BLOCK)*sizeof(a1fs_extent);
			curr_extent = (struct a1fs_extent*)(fs->image + extent_location);
		}
		else{
			// We are looking at the extent at index i in the extents array.
			curr_extent = parent_directory->extent + i;
		}
		if (curr_extent->count > 0){
	        // Loop through this entire extent (depending on extent length).
			for (size_t j = 0; j < curr_extent->count; j++){
				int curr_entry_block = superblock->data_region + curr_extent->start + j;

				// Loop through all the entries in this extent.
				for (size_t k = 0; k < A1FS_BLOCK_SIZE/sizeof(a1fs_dentry); k++){
						struct a1fs_dentry *curr_entry = (struct a1fs_dentry*)(fs->image + A1FS_BLOCK_SIZE*curr_entry_block + k*sizeof(a1fs_dentry));

						// Check if this entry is not in use.
						if ((curr_entry->ino == 0 && (curr_entry->name)[0] == '\0') || curr_entry == NULL){
				            strcpy(curr_entry->name, file_name);
				            curr_entry->ino = inode_index;
				            created_dentry = 1;
				            break;
						}
				}
				if (created_dentry){
					break;
				}
			}
			if (created_dentry) {
				break;
			}
			extents_count++;
		}
		i++;
	}

	// TODO: The existing extents had no space available, need to assign more space to the parent dir.
	if (!created_dentry) {	
		int extent_index = allocate_new_block(&parent_directory, fs->image);
		if (extent_index == -1){
			return -ENOSPC;
		}

		struct a1fs_dentry *new_entry;

		// Check if the new block is part of an extent stored in the indirect block.
		if (extent_index >= A1FS_IND_BLOCK){
			int extent_block = (superblock->data_region + ((parent_directory->extent)[A1FS_IND_BLOCK]).start)*A1FS_BLOCK_SIZE;
			struct a1fs_extent *extent = (struct a1fs_extent*)(fs->image + extent_block + sizeof(a1fs_extent)*(extent_index % A1FS_IND_BLOCK));

			int entry_byte = (superblock->data_region + extent->start)*A1FS_BLOCK_SIZE;
			new_entry = (struct a1fs_dentry*)(fs->image + entry_byte + (extent->count - 1)*A1FS_BLOCK_SIZE);
		}
		else {
			int entry_byte = (superblock->data_region + ((parent_directory->extent)[extent_index]).start)*A1FS_BLOCK_SIZE;
			new_entry = (struct a1fs_dentry*)(fs->image + entry_byte + (((parent_directory->extent)[extent_index]).count - 1)*A1FS_BLOCK_SIZE);
		}

		strcpy(new_entry->name, file_name);
        new_entry->ino = inode_index;
	}
	
	//create the inode for the new directory and save it to the inode table
	struct a1fs_inode *inode = (struct a1fs_inode*)(fs->image + A1FS_BLOCK_SIZE*superblock->inode_table + inode_index*sizeof(a1fs_inode));
	init_inode(inode, mode);

	// Update
	inode_bitmap[inode_index] = 1;
	superblock->free_inodes_count -= 1;
	parent_directory->dentry++;

	return 0;
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
static int a1fs_unlink(const char *path)								//TODO same issue as rmdir
{
	fs_ctx *fs = get_fs();

	//TODO: remove the file at given path
	(void)path;
	(void)fs;
	
	struct a1fs_superblock *superblock = (struct a1fs_superblock*)(fs->image);
	struct a1fs_inode *inodes = (struct a1fs_inode*)(fs->image + A1FS_BLOCK_SIZE * superblock->inode_table);
	struct a1fs_inode *root_inode = inodes + A1FS_ROOT_INO;
	unsigned char *inode_bitmap = (unsigned char*)(fs->image + (A1FS_BLOCK_SIZE * superblock->inode_bitmap));
	unsigned char *block_bitmap = (unsigned char*)(fs->image + (A1FS_BLOCK_SIZE * superblock->block_bitmap));

	//find the file to be removed
	struct a1fs_inode *file = (void *)0;
	char path_copy[A1FS_NAME_MAX];
	strcpy(path_copy, path);
	inode_from_path(root_inode, &file, path, fs->image);

	//find the parent directory
	struct a1fs_inode *parent_directory = (void *)0;
	char *final_slash = strrchr(path_copy, '/');
	*final_slash = '\0';
	char file_name[A1FS_NAME_MAX];
	strcpy(file_name, (final_slash + 1));

	// Base case for when we are removing a file in the root.
	if (strlen(path_copy) == 0){
		parent_directory = root_inode;
	}
	else {
		inode_from_path(root_inode, &parent_directory, path_copy, fs->image);
	}

	// Look through the directory's existing extents to free space
	struct a1fs_extent *curr_extent;
	int extents_count = 0;
	int i = 0;
	while (extents_count < file->extents){
		if (i >= A1FS_IND_BLOCK){
			// We are now looking in the indirect block.
			int extent_location = A1FS_BLOCK_SIZE*(superblock->data_region + ((file->extent)[A1FS_IND_BLOCK]).start) + (i-A1FS_IND_BLOCK)*sizeof(a1fs_extent);
			curr_extent = (struct a1fs_extent*)(fs->image + extent_location);
		}
		else{
			// We are looking at the extent at index i in the extents array.
			curr_extent = file->extent + i;
		}
		if (curr_extent->count > 0){
	        // Loop through this entire extent (depending on extent length).
			for (size_t j = 0; j < curr_extent->count; j++){
				int curr_entry_block = superblock->data_region + curr_extent->start + j;
				block_bitmap[curr_entry_block] = 0;
				memset((fs->image + (A1FS_BLOCK_SIZE * curr_entry_block)), 0, A1FS_BLOCK_SIZE);			// TODO change what data we set erased blocks to
				superblock->free_blocks_count += 1;
			}
			extents_count++;
		}
		i++;
	}

	// Look through the parent directory's existing extents to locate the dentry for the removed file
	extents_count = 0;
	i = 0;
	int removed = 0;
	int inode_num = -1;
	while (extents_count < parent_directory->extents){
		if (i >= A1FS_IND_BLOCK){
			// We are now looking in the indirect block.
			int extent_location = A1FS_BLOCK_SIZE*(superblock->data_region + ((parent_directory->extent)[A1FS_IND_BLOCK]).start) + (i-A1FS_IND_BLOCK)*sizeof(a1fs_extent);
			curr_extent = (struct a1fs_extent*)(fs->image + extent_location);
		}
		else{
			// We are looking at the extent at index i in the extents array.
			curr_extent = parent_directory->extent + i;
		}
		if (curr_extent->count > 0){
	        // Loop through this entire extent (depending on extent length).
			for (size_t j = 0; j < curr_extent->count; j++){
				int curr_entry_block = superblock->data_region + curr_extent->start + j;

				// Loop through all the entries in this extent.
				for (size_t k = 0; k < A1FS_BLOCK_SIZE/sizeof(a1fs_dentry); k++){
						struct a1fs_dentry *curr_entry = (struct a1fs_dentry*)(fs->image + A1FS_BLOCK_SIZE*curr_entry_block + k*sizeof(a1fs_dentry));

						// Check if this entry is the removed directory's
						if (curr_entry != NULL) {
							if (!strcmp(curr_entry->name, file_name)) {
								inode_num = curr_entry->ino;
								inode_bitmap[curr_entry->ino] = 0;
								memset(curr_entry, 0, sizeof(a1fs_dentry));				//TODO change what data we set erased blocks to
								superblock->free_inodes_count += 1;
								removed = 1;
								parent_directory->dentry -= 1;
								break;
							}
						}
				}
				if (removed){
					break;
				}
			}
			if (removed) {
				break;
			}
			extents_count++;
		}
		i++;
	}

	//remove the directory inode from the inode table		
	if (inode_num > 0) {
		memset((fs->image + (A1FS_BLOCK_SIZE * superblock->inode_table) + (sizeof(a1fs_inode) * inode_num)), 0, sizeof(a1fs_inode));
	}

	return 0;
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

	struct a1fs_superblock *superblock = (struct a1fs_superblock*)(fs->image);
	struct a1fs_inode *inodes = (struct a1fs_inode*)(fs->image + A1FS_BLOCK_SIZE * superblock->inode_table);
	struct a1fs_inode *root_inode = inodes + A1FS_ROOT_INO;
	struct a1fs_inode *target = (void *)0;
	inode_from_path(root_inode, &target, path, fs->image);
	
	if (tv->tv_nsec == UTIME_NOW || tv == NULL) {				//change to current time
		clock_gettime(CLOCK_REALTIME, &target->mtime);
	}
	else if (tv->tv_nsec != UTIME_OMIT) {						//change to time stored in struct
		target->mtime.tv_sec = tv->tv_sec;
		target->mtime.tv_nsec = tv->tv_nsec;
	}

	return 0;
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

	// Setup all of the usefull variables we will need.
	struct a1fs_superblock *sb = (struct a1fs_superblock*)(fs->image);
	struct a1fs_inode *inodes = (struct a1fs_inode*)(fs->image + A1FS_BLOCK_SIZE * sb->inode_table);
	struct a1fs_inode *root_inode = inodes + A1FS_ROOT_INO;

	// Get the target file that we will be reading, we do not need to check the return value of 'inode_from_path'
	// because we are assuming it has already beed checked by a1fs_getattr().
	struct a1fs_inode *target = (void *)0;
	inode_from_path(root_inode, &target, path, fs->image);

	// Loop through the file's extents
	size_t byte_count = 0;
	int offset_count = 0;
	a1fs_extent *curr_extent;
	int extents_count = 0;
	int i = 0;
	while (extents_count < target->extents){
		if (i >= A1FS_IND_BLOCK){
			// We are now looking in the indirect block.
			int extent_location = A1FS_BLOCK_SIZE*(sb->data_region + ((target->extent)[A1FS_IND_BLOCK]).start) + (i-A1FS_IND_BLOCK)*sizeof(a1fs_extent);
			curr_extent = (struct a1fs_extent*)(fs->image + extent_location);
		}
		else{
			// We are looking at the extent at index i in the extents array.
			curr_extent = target->extent + i;
		}
		if (curr_extent->count > 0){
			// Loop through this entire extent (depending on extent length).
			for (size_t j = 0; j < curr_extent->count; j++){
				int curr_block = sb->data_region + curr_extent->start + j;

				// Loop through all the bytes in this block.
				for (size_t k = 0; k < A1FS_BLOCK_SIZE; k++){
					// First we must get to the start of the read range.
					if (offset_count < offset){
						offset_count++;
					}
					// Check if we still need to read more bytes.
					else if (byte_count < size){
						buf[byte_count] = *((unsigned char*)(fs->image + A1FS_BLOCK_SIZE*curr_block + k));
						byte_count++;
					}
					// We have read the number of bytes requested, return success.
					else {
						return byte_count;
					}
				}
			}
			extents_count++;
		}
		i++;
	}

	// We reached EOF.

	// Check if the offset was beyong EOF.
	if (byte_count == 0){
		return 0;
	}

	// Fill the rest of the buffer with 0's.
	for (size_t i = 0; i < size - byte_count; i ++){
		buf[byte_count + i] = 0;
	}
	return byte_count;
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
	(void)buf;
	(void)size;
	(void)offset;

	// Setup all of the usefull variables we will need.
	struct a1fs_superblock *sb = (struct a1fs_superblock*)(fs->image);
	struct a1fs_inode *inodes = (struct a1fs_inode*)(fs->image + A1FS_BLOCK_SIZE * sb->inode_table);
	struct a1fs_inode *root_inode = inodes + A1FS_ROOT_INO;

	// Get the target file that we will be reading, we do not need to check the return value of 'inode_from_path'
	// because we are assuming it has already beed checked by a1fs_getattr().
	struct a1fs_inode *target = (void *)0;
	inode_from_path(root_inode, &target, path, fs->image);

	// Loop through the file's extents
	size_t byte_count = 0;
	int offset_count = 0;
	a1fs_extent *curr_extent;
	int extents_count = 0;
	int i = 0;
	while (extents_count < target->extents){
		if (i >= A1FS_IND_BLOCK){
			// We are now looking in the indirect block.
			int extent_location = A1FS_BLOCK_SIZE*(sb->data_region + ((target->extent)[A1FS_IND_BLOCK]).start) + (i-A1FS_IND_BLOCK)*sizeof(a1fs_extent);
			curr_extent = (struct a1fs_extent*)(fs->image + extent_location);
		}
		else{
			// We are looking at the extent at index i in the extents array.
			curr_extent = target->extent + i;
		}
		if (curr_extent->count > 0){
			// Loop through this entire extent (depending on extent length).
			for (size_t j = 0; j < curr_extent->count; j++){
				int curr_block = sb->data_region + curr_extent->start + j;
				unsigned char *byte = (unsigned char*)(fs->image + A1FS_BLOCK_SIZE*curr_block);

				// Loop through all the bytes in this block.
				for (size_t k = 0; k < A1FS_BLOCK_SIZE; k++){
					// First we must get to the start of the read range.
					if (offset_count < offset){
						offset_count++;
					}
					// Check if we still need to write more bytes.
					else if (byte_count < size){
						byte[k] = buf[byte_count];
						byte_count++;
					}
					// We have read the number of bytes requested, return success.
					else {
						return byte_count;
					}
				}
			}
			extents_count++;
		}
		i++;
	}

	// We reached EOF.

	// Check if the offset was beyong EOF.
	if (byte_count == 0){
		// The file must be extended.
		 if (allocate_new_block(&target, fs->image) == -1){
		 	return -ENOSPC;
		 }
		 return a1fs_write(path, buf, size, offset, fi);
	}

	// Check if we still have bytes to write.
	while (byte_count < size){
		int extent_index = append_new_block(target, fs->image);
		if (extent_index== -1){
			return -ENOSPC;
		}
		unsigned char *byte;

		// Check if the new block is part of an extent stored in the indirect block.
		if (extent_index >= A1FS_IND_BLOCK){
			int extent_block = (sb->data_region + ((target->extent)[A1FS_IND_BLOCK]).start)*A1FS_BLOCK_SIZE;
			struct a1fs_extent *extent = (struct a1fs_extent*)(fs->image + extent_block + sizeof(a1fs_extent)*(extent_index % A1FS_IND_BLOCK));

			int new_block = (sb->data_region + extent->start)*A1FS_BLOCK_SIZE;
			byte = (unsigned char*)(fs->image + new_block + (extent->count - 1)*A1FS_BLOCK_SIZE);
		}
		// The new block is in the direct extents array.
		else {
			int new_block = (sb->data_region + ((target->extent)[extent_index]).start)*A1FS_BLOCK_SIZE;
			byte = (unsigned char*)(fs->image + new_block + (((target->extent)[extent_index]).count - 1)*A1FS_BLOCK_SIZE);
		}

		// Fill this new block with the remaining data.
		for (int i = 0; i < A1FS_BLOCK_SIZE; i++){
			byte[i] = buf[byte_count];
			byte_count++;
		}
	}

	return byte_count;
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