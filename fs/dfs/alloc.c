// SPDX-License-Identifier: GPL-2.0-only

#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/slab.h>

#include "internal.h"

u64 dfs_chunk_bytes(struct super_block *sb)
{
	struct dfs_sb_info *sbi = sb->s_fs_info;

	return sbi->chunk_blocks * sb->s_blocksize;
}

int dfs_alloc_inode_info(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct dfs_inode_info *di;
	u64 chunk_bytes;
	u64 base;
	u64 bdev_bytes;

	if (!sb || !sb->s_bdev)
		return -EINVAL;

	bdev_bytes = bdev_nr_bytes(sb->s_bdev);
	chunk_bytes = dfs_chunk_bytes(sb);
	base = (u64)inode->i_ino * chunk_bytes;
	if (base + chunk_bytes > bdev_bytes)
		return -ENOSPC;

	di = kzalloc(sizeof(*di), GFP_KERNEL);
	if (!di)
		return -ENOMEM;

	di->base = base;
	di->chunk_bytes = chunk_bytes;
	inode->i_private = di;
	return 0;
}
