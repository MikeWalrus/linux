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

loff_t dfs_inode_offset(struct super_block *sb, u32 ino)
{
	loff_t table_start = (loff_t)DFS_INODE_TABLE_BLOCK * sb->s_blocksize;
	loff_t offset = (loff_t)ino * DFS_INODE_SIZE;

	return table_start + offset;
}

int dfs_inode_disk_location(struct super_block *sb, u32 ino,
				 loff_t *offset, pgoff_t *index,
				 unsigned int *page_off)
{
	u64 bdev_bytes;
	loff_t off;
	unsigned int page_offset;

	if (!sb || !sb->s_bdev || !offset || !index || !page_off)
		return -EINVAL;
	if (sb->s_blocksize < DFS_INODE_SIZE)
		return -EOPNOTSUPP;

	off = dfs_inode_offset(sb, ino);
	bdev_bytes = bdev_nr_bytes(sb->s_bdev);
	if (off + DFS_INODE_SIZE > bdev_bytes)
		return -ENOSPC;

	page_offset = off & (PAGE_SIZE - 1);
	if (page_offset + DFS_INODE_SIZE > PAGE_SIZE)
		return -EOPNOTSUPP;

	*offset = off;
	*index = off >> PAGE_SHIFT;
	*page_off = page_offset;
	return 0;
}

int dfs_inode_base(struct super_block *sb, u32 ino, u64 *base, u64 *chunk_bytes)
{
	u64 bdev_bytes;

	if (!sb || !sb->s_bdev || !base || !chunk_bytes)
		return -EINVAL;

	*chunk_bytes = dfs_chunk_bytes(sb);
	*base = (u64)ino * (*chunk_bytes);
	bdev_bytes = bdev_nr_bytes(sb->s_bdev);
	if (*base + *chunk_bytes > bdev_bytes)
		return -ENOSPC;

	return 0;
}

bool dfs_inode_base_valid(struct super_block *sb, u32 ino, u64 base,
			  u64 chunk_bytes)
{
	u64 bdev_bytes;

	if (!sb || !sb->s_bdev)
		return false;
	if (!chunk_bytes)
		return false;
	bdev_bytes = bdev_nr_bytes(sb->s_bdev);
	if (base >= bdev_bytes)
		return false;
	if (base + chunk_bytes > bdev_bytes)
		return false;
	if (base % chunk_bytes != 0)
		return false;
	if (base != (u64)ino * chunk_bytes)
		return false;
	return true;
}

int dfs_map_file_range(struct inode *inode, loff_t offset, loff_t length,
			 loff_t *start_out, loff_t *len_out, u64 *addr_out)
{
	struct dfs_inode_info *di = inode->i_private;
	struct super_block *sb = inode->i_sb;
	loff_t start;
	loff_t map_len;
	u64 bdev_bytes;
	u64 addr;

	if (!di || !sb || !sb->s_bdev)
		return -EIO;
	if (length <= 0)
		return -EINVAL;

	start = ALIGN_DOWN(offset, sb->s_blocksize);
	map_len = di->chunk_bytes - start;
	if (map_len <= 0)
		return -ENOSPC;

	map_len = min_t(loff_t, map_len, length + (offset - start));
	addr = di->base + start;
	bdev_bytes = bdev_nr_bytes(sb->s_bdev);
	if (addr >= bdev_bytes)
		return -ENOSPC;
	if (addr + map_len > bdev_bytes)
		map_len = bdev_bytes - addr;
	if (map_len == 0)
		return -ENOSPC;

	*start_out = start;
	*len_out = map_len;
	*addr_out = addr;
	return 0;
}

int dfs_alloc_inode_info(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct dfs_inode_info *di;
	u64 chunk_bytes;
	u64 base;
	int ret;

	ret = dfs_inode_base(sb, inode->i_ino, &base, &chunk_bytes);
	if (ret)
		return ret;

	di = kzalloc(sizeof(*di), GFP_KERNEL);
	if (!di)
		return -ENOMEM;

	di->base = base;
	di->chunk_bytes = chunk_bytes;
	inode->i_private = di;
	return 0;
}
