/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/fs.h>

#define DFS_SUPER_MAGIC 0x00444653
#define DFS_SUPER_VERSION 1
#define DFS_DEFAULT_BLOCK_SIZE 4096

struct dfs_super_block {
	__le32 magic;
	__le32 version;
	__le32 block_size;
	__le32 flags;
	__le64 created_ns;
	__le64 reserved[5];
};

extern const struct file_operations dfs_file_operations;
extern const struct inode_operations dfs_file_inode_operations;
