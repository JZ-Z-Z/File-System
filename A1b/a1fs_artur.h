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
 * CSC369 Assignment 1 - a1fs types, constants, and data structures header file.
 */

#pragma once

#include <assert.h>
#include <stdint.h>
#include <limits.h>
#include <sys/stat.h>


/**
 * a1fs block size in bytes. You are not allowed to change this value.
 *
 * The block size is the unit of space allocation. Each file (and directory)
 * must occupy an integral number of blocks. Each of the file systems metadata
 * partitions, e.g. superblock, inode/block bitmaps, inode table (but not an
 * individual inode) must also occupy an integral number of blocks.
 */
#define A1FS_BLOCK_SIZE 4096

/*
 * Creation modes for inode.
 */
#define A1FS_S_IFLNK 0xA000 /* symbolic link */
#define A1FS_S_IFREG 0x8000 /* regular file */
#define A1FS_S_IFDIR 0x4000 /* directory */

/* Block number (block pointer) type. */
typedef uint32_t a1fs_blk_t;

/* Inode number type. */
typedef uint32_t a1fs_ino_t;

/* The index of the first data block that is not reserved
   blocks 0-31 are reserved for the inodes. */
#define A1FS_FIRST_BLOCK 32

/** Magic value that can be used to identify an a1fs image. */
#define A1FS_MAGIC 0xC5C369A1C5C369A1ul

/** a1fs superblock. */
typedef struct a1fs_superblock {
	/** Must match A1FS_MAGIC. */
	uint64_t magic;
	/** File system size in bytes. */
	uint64_t size;

	//TODO
	unsigned int inodes_count;       /* Total number of inodes */
	unsigned int blocks_count;       /* Total number of data blocks */
	unsigned int free_inodes_count;  /* Number of free inodes */
	unsigned int free_blocks_count;  /* Number of free data blocks */

	// Block indexes starting from 0. Superblock is block index 0.
	unsigned int block_bitmap;      /* Blocks bitmap block */
	unsigned int inode_bitmap;      /* Inodes bitmap block */
	unsigned int inode_table;       /* Inodes table block */
	unsigned int data_region;       /* Data region starting block */  

} a1fs_superblock;

// Superblock must fit into a single block
static_assert(sizeof(a1fs_superblock) <= A1FS_BLOCK_SIZE,
              "superblock is too large");


/** Extent - a contiguous range of blocks. */
typedef struct a1fs_extent {
	/** Starting block of the extent. */
	a1fs_blk_t start;
	/** Number of blocks in the extent. */
	a1fs_blk_t count;

} a1fs_extent;

/* The index of the single indirect pointer in the extent array of an inode */
#define A1FS_IND_BLOCK 10

/* The index of the reserved root inode */
#define A1FS_ROOT_INO 0

/* The length of the inode extents array */
#define A1FS_EXTENTS_LENGTH 11

#define A1FS_NUM_EXTENTS 512


/** a1fs inode. */
typedef struct a1fs_inode {
	/** File mode. */
	mode_t mode;

	/**
	 * Reference count (number of hard links).
	 *
	 * Each file is referenced by its parent directory. Each directory is
	 * referenced by its parent directory, itself (via "."), and each
	 * subdirectory (via ".."). The "parent directory" of the root directory is
	 * the root directory itself.
	 */
	uint32_t links;

	/** File size in bytes. */
	uint64_t size;

	/**
	 * Last modification timestamp.
	 *
	 * Use the CLOCK_REALTIME clock; see "man 3 clock_gettime". Must be updated
	 * when the file (or directory) is created, written to, or its size changes.
	 */
	struct timespec mtime;

	//TODO
	int extents;      /* Extents count */
	int dentry; /* Dir entry count (only if inode is a directory) */
	a1fs_extent extent[A1FS_EXTENTS_LENGTH]; /* Pointers to extents */
	// extent[0-9] are direct, extent[10] is Single Indirect

	//NOTE: You might have to add padding (e.g. a dummy char array field) at the
	// end of the struct in order to satisfy the assertion below. Try to keep
	// the size of this struct minimal, but don't worry about the "wasted space"
	// introduced by the required padding.

} a1fs_inode;

// A single block must fit an integral number of inodes
static_assert(A1FS_BLOCK_SIZE % sizeof(a1fs_inode) == 0, "invalid inode size");


/** Maximum file name (path component) length. Includes the null terminator. */
#define A1FS_NAME_MAX 252

/** Maximum file path length. Includes the null terminator. */
#define A1FS_PATH_MAX PATH_MAX

/** Fixed size directory entry structure. */
typedef struct a1fs_dentry {
	/** Inode number. */
	a1fs_ino_t ino;
	/** File name. A null-terminated string. */
	char name[A1FS_NAME_MAX];

} a1fs_dentry;

static_assert(sizeof(a1fs_dentry) == 256, "invalid dentry size");
