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
 * CSC369 Assignment 1 - a1fs formatting tool.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

#include "a1fs.h"
#include "map.h"


/** Command line options. */
typedef struct mkfs_opts {
	/** File system image file path. */
	const char *img_path;
	/** Number of inodes. */
	size_t n_inodes;

	/** Print help and exit. */
	bool help;
	/** Overwrite existing file system. */
	bool force;
	/** Sync memory-mapped image file contents to disk. */
	bool sync;
	/** Verbose output. If false, the program must only print errors. */
	bool verbose;
	/** Zero out image contents. */
	bool zero;

} mkfs_opts;

static const char *help_str = "\
Usage: %s options image\n\
\n\
Format the image file into a1fs file system. The file must exist and\n\
its size must be a multiple of a1fs block size - %zu bytes.\n\
\n\
Options:\n\
    -i num  number of inodes; required argument\n\
    -h      print help and exit\n\
    -f      force format - overwrite existing a1fs file system\n\
    -s      sync image file contents to disk\n\
    -v      verbose output\n\
    -z      zero out image contents\n\
";

static void print_help(FILE *f, const char *progname)
{
	fprintf(f, help_str, progname, A1FS_BLOCK_SIZE);
}


static bool parse_args(int argc, char *argv[], mkfs_opts *opts)
{
	char o;
	while ((o = getopt(argc, argv, "i:hfsvz")) != -1) {
		switch (o) {
			case 'i': opts->n_inodes = strtoul(optarg, NULL, 10); break;

			case 'h': opts->help    = true; return true;// skip other arguments
			case 'f': opts->force   = true; break;
			case 's': opts->sync    = true; break;
			case 'v': opts->verbose = true; break;
			case 'z': opts->zero    = true; break;

			case '?': return false;
			default : assert(false);
		}
	}

	if (optind >= argc) {
		fprintf(stderr, "Missing image path\n");
		return false;
	}
	opts->img_path = argv[optind];

	if (opts->n_inodes == 0) {
		fprintf(stderr, "Missing or invalid number of inodes\n");
		return false;
	}
	return true;
}


/** Determine if the image has already been formatted into a1fs. */
static bool a1fs_is_present(void *image)
{
	//TODO: check if the image already contains a valid a1fs superblock
	struct a1fs_superblock *sb = (struct a1fs_superblock*)(image);
	if (sb->magic == A1FS_MAGIC){
		return true;
	}
	return false;
}


/**
 * Format the image into a1fs.
 *
 * NOTE: Must update mtime of the root directory.
 *
 * @param image  pointer to the start of the image.
 * @param size   image size in bytes.
 * @param opts   command line options.
 * @return       true on success;
 *               false on error, e.g. options are invalid for given image size.
 */
static bool mkfs(void *image, size_t size, mkfs_opts *opts)
{
	// Check if given image size can support a superblock, block_bm, inode_bm, atleast one 
	// data block, and the provided number of inodes.
	if (size < (4*A1FS_BLOCK_SIZE + opts->n_inodes*sizeof(a1fs_inode))){
		return false;
	}

	//TODO: initialize the superblock and create an empty root directory
	struct a1fs_superblock *sb = (struct a1fs_superblock*)(image);
	sb->magic = A1FS_MAGIC;
	sb->size = size;
	sb->inodes_count = opts->n_inodes;
	sb->free_inodes_count = opts->n_inodes;

	// Calculate the block of the inodes table based on # of inodes.
	int num_table_blocks = (opts->n_inodes * sizeof(a1fs_inode)) / A1FS_BLOCK_SIZE;
	if ((opts->n_inodes * sizeof(a1fs_inode)) % A1FS_BLOCK_SIZE != 0){
		num_table_blocks++;
	}

	sb->block_bitmap = 1;
	sb->inode_bitmap = 2;
	sb->inode_table = 3;
	sb->data_region = sb->inode_table + num_table_blocks;

	// Create an empty root directory
	struct a1fs_inode *inodes = (struct a1fs_inode*)(image + A1FS_BLOCK_SIZE * sb->inode_table);
	struct a1fs_inode *root_inode = inodes + A1FS_ROOT_INO;
	root_inode->mode = A1FS_S_IFDIR;
	root_inode->links = 2;
	root_inode->extents = 1;
	root_inode->dentry = 1;

	// struct a1fs_dentry *root_entry = (struct a1fs_dentry*)(image + A1FS_BLOCK_SIZE*sb->data_region);
	// root_entry->ino = A1FS_ROOT_INO;
	// strncpy(root_entry->name, ".", A1FS_NAME_MAX);
	struct a1fs_dentry *test_entry = (struct a1fs_dentry*)(image + A1FS_BLOCK_SIZE*sb->data_region);
	strncpy(test_entry->name, ".Trash", A1FS_NAME_MAX); 
	struct a1fs_inode *test_inode = inodes + 1;
	test_inode->mode = A1FS_S_IFREG;
	test_inode->links = 0;
	test_inode->extents = 0;
	test_inode->dentry = 0;
	test_inode->size = 55;


	test_entry->ino = 1;
	(root_inode->extent)[0].start = 0;
	(root_inode->extent)[0].count = 1;


	root_inode->size = sizeof(a1fs_dentry)*2;

	clock_gettime(CLOCK_REALTIME, &root_inode->mtime);

	return true;
}


int main(int argc, char *argv[])
{
	mkfs_opts opts = {0};// defaults are all 0
	if (!parse_args(argc, argv, &opts)) {
		// Invalid arguments, print help to stderr
		print_help(stderr, argv[0]);
		return 1;
	}
	if (opts.help) {
		// Help requested, print it to stdout
		print_help(stdout, argv[0]);
		return 0;
	}

	// Map image file into memory
	size_t size;
	void *image = map_file(opts.img_path, A1FS_BLOCK_SIZE, &size);
	if (image == NULL) return 1;

	// Check if overwriting existing file system
	int ret = 1;
	if (!opts.force && a1fs_is_present(image)) {
		fprintf(stderr, "Image already contains a1fs; use -f to overwrite\n");
		goto end;
	}

	if (opts.zero) memset(image, 0, size);
	if (!mkfs(image, size, &opts)) {
		fprintf(stderr, "Failed to format the image\n");
		goto end;
	}

	// Sync to disk if requested
	if (opts.sync && (msync(image, size, MS_SYNC) < 0)) {
		perror("msync");
		goto end;
	}

	ret = 0;
end:
	munmap(image, size);
	return ret;
}
