// SPDX-License-Identifier: GPL-2.0-only

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/statfs.h>
#include <linux/fs_context.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/writeback.h>
#include <linux/string.h>

#include "internal.h"

static const struct super_operations dfs_ops;
static int dfs_sync_fs(struct super_block *sb, int wait);
static void dfs_dirty_inode(struct inode *inode, int flags);
static int dfs_read_validate_super(struct super_block *sb,
				    struct dfs_super_block *out);
static int dfs_write_super(struct super_block *sb, u32 next_ino, bool wait);

#define DFS_COMMIT_INTERVAL (5 * HZ)

static void dfs_commit_work(struct work_struct *work)
{
	struct dfs_sb_info *sbi = container_of(work, struct dfs_sb_info,
					    commit_work.work);
	struct super_block *sb = sbi->sb;

	atomic_set(&sbi->commit_pending, 0);
	if (!sb)
		return;
	down_read(&sb->s_umount);
	sync_filesystem(sb);
	up_read(&sb->s_umount);
}

void dfs_schedule_commit(struct super_block *sb)
{
	struct dfs_sb_info *sbi = sb->s_fs_info;

	if (!sbi)
		return;

	if (atomic_xchg(&sbi->commit_pending, 1) == 0) {
		dfs_info("schedule commit sb=%pg\n", sb->s_bdev);
		schedule_delayed_work(&sbi->commit_work, DFS_COMMIT_INTERVAL);
	}
}

static int dfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	u64 id = huge_encode_dev(sb->s_dev);
	u64 blocks = 0;

	if (sb->s_bdev)
		blocks = div_u64(bdev_nr_bytes(sb->s_bdev), sb->s_blocksize);

	buf->f_type = DFS_SUPER_MAGIC;
	buf->f_bsize = sb->s_blocksize;
	buf->f_blocks = blocks;
	buf->f_bfree = blocks;
	buf->f_bavail = blocks;
	buf->f_files = 0;
	buf->f_ffree = 0;
	buf->f_fsid = u64_to_fsid(id);
	buf->f_namelen = NAME_MAX;
	return 0;
}

static const struct super_operations dfs_ops = {
	.statfs		= dfs_statfs,
	.drop_inode	= inode_just_drop,
	.free_inode	= dfs_free_inode,
	.write_inode	= dfs_write_inode,
	.dirty_inode	= dfs_dirty_inode,
	.sync_fs	= dfs_sync_fs,
};

static void dfs_dirty_inode(struct inode *inode, int flags)
{
	dfs_info("dirty_inode ino=%lu size=%lld mode=0%o flags=0x%x\n",
		 inode->i_ino, inode->i_size, inode->i_mode, flags);
	if (inode->i_sb)
		dfs_schedule_commit(inode->i_sb);
}

static int dfs_sync_fs(struct super_block *sb, int wait)
{
	struct dfs_sb_info *sbi = sb->s_fs_info;

	dfs_info("sync_fs wait=%d sb=%pg\n", wait, sb->s_bdev);
	if (sbi && atomic_xchg(&sbi->super_dirty, 0))
		dfs_write_super(sb, (u32)atomic_read(&sbi->next_ino), wait != 0);
	return 0;
}

static int dfs_read_validate_super(struct super_block *sb,
				    struct dfs_super_block *out)
{
	struct folio *folio;
	struct dfs_super_block *disk;
	struct address_space *mapping;
	int ret = 0;

	if (!sb->s_bdev)
		return -EINVAL;

	mapping = sb->s_bdev->bd_mapping;
	folio = read_mapping_folio(mapping, 0, NULL);
	if (IS_ERR(folio))
		return PTR_ERR(folio);

	disk = (struct dfs_super_block *)folio_address(folio);
	if (le32_to_cpu(disk->magic) != DFS_SUPER_MAGIC)
		ret = -EINVAL;
	else if (le32_to_cpu(disk->version) != DFS_SUPER_VERSION)
		ret = -EINVAL;
	else if (le32_to_cpu(disk->block_size) != DFS_DEFAULT_BLOCK_SIZE)
		ret = -EINVAL;
	else if (le32_to_cpu(disk->flags) != 0)
		ret = -EOPNOTSUPP;
	else
		memcpy(out, disk, sizeof(*out));

	folio_put(folio);
	return ret;
}

static int dfs_write_super(struct super_block *sb, u32 next_ino, bool wait)
{
	struct folio *folio;
	struct dfs_super_block *disk;
	struct address_space *mapping;
	int ret = 0;

	if (!sb->s_bdev)
		return -EINVAL;

	mapping = sb->s_bdev->bd_mapping;
	folio = read_mapping_folio(mapping, 0, NULL);
	if (IS_ERR(folio))
		return PTR_ERR(folio);

	disk = (struct dfs_super_block *)folio_address(folio);
	disk->next_ino = cpu_to_le32(next_ino);
	flush_dcache_folio(folio);
	folio_mark_dirty(folio);
	folio_put(folio);

	if (wait) {
		loff_t end = sb->s_blocksize - 1;

		ret = filemap_fdatawrite_range(mapping, 0, end);
		if (!ret)
			ret = filemap_fdatawait_range(mapping, 0, end);
	}
	return ret;
}

static int dfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct inode *inode;
	struct dfs_sb_info *sbi;
	struct dfs_disk_inode root_disk;
	struct dfs_super_block on_disk;
	int error;
	int ret = 0;
	bool create_root = false;
	u64 blocks;

	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_magic = DFS_SUPER_MAGIC;
	if (!sb_set_blocksize(sb, DFS_DEFAULT_BLOCK_SIZE))
		return -EINVAL;
	if (sb->s_bdev)
		invalidate_bdev(sb->s_bdev);

	if (dfs_read_validate_super(sb, &on_disk))
		return -EINVAL;

	dfs_info("mount: dev=%pg blocksize=%lu\n", sb->s_bdev,
		 sb->s_blocksize);

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sbi->sb = sb;
	atomic_set(&sbi->commit_pending, 0);
	atomic_set(&sbi->super_dirty, 0);
	INIT_DELAYED_WORK(&sbi->commit_work, dfs_commit_work);
	atomic_set(&sbi->next_ino, 1);
	blocks = div_u64(bdev_nr_bytes(sb->s_bdev), sb->s_blocksize);
	sbi->chunk_blocks = div_u64(DFS_CHUNK_BYTES, sb->s_blocksize);
	if (sbi->chunk_blocks == 0)
		sbi->chunk_blocks = 1;
	sb->s_fs_info = sbi;
	sb->s_op = &dfs_ops;
	sb->s_d_flags = 0;
	sb->s_time_gran = 1;

	{
		u32 disk_next_ino = le32_to_cpu(on_disk.next_ino);

		if (disk_next_ino < 1)
			disk_next_ino = 1;
		atomic_set(&sbi->next_ino, disk_next_ino);
		create_root = (disk_next_ino == 1);
	}

	if (!create_root) {
		error = dfs_read_inode_disk(sb, 1, &root_disk);
		if (error) {
			ret = error;
			goto out_free_sbi;
		}
		inode = dfs_iget(sb, 1, le32_to_cpu(root_disk.mode));
		if (IS_ERR(inode)) {
			error = PTR_ERR(inode);
			ret = error;
			goto out_free_sbi;
		}
		if (!dfs_root_dir_valid(inode)) {
			error = dfs_make_empty(inode, inode);
			if (error)
				goto out_iput;
		}
	} else {
		inode = dfs_get_inode(sb, NULL, S_IFDIR | 0755, 0);
		if (!inode) {
			error = -ENOMEM;
			ret = error;
			goto out_free_sbi;
		}
		error = dfs_make_empty(inode, inode);
		if (error)
			goto out_iput;
		atomic_set(&sbi->next_ino, 2);
		atomic_set(&sbi->super_dirty, 1);
		dfs_write_super(sb, 2, true);
	}

	dfs_info("root inode=%lu\n", inode->i_ino);

	sb->s_root = d_make_root(inode);
	if (!sb->s_root) {
		error = -ENOMEM;
		goto out_iput;
	}

	return 0;

out_iput:
	ret = error ? error : -ENOMEM;
	iput(inode);
out_free_sbi:
	if (sbi) {
		cancel_delayed_work_sync(&sbi->commit_work);
		kfree(sbi);
		sb->s_fs_info = NULL;
	}
	return ret;
}

static int dfs_get_tree(struct fs_context *fc)
{
	return get_tree_bdev(fc, dfs_fill_super);
}

static void dfs_kill_sb(struct super_block *sb)
{
	struct dfs_sb_info *sbi = sb->s_fs_info;

	if (sbi) {
		dfs_info("kill_sb sync start sb=%pg\n", sb->s_bdev);
		sync_filesystem(sb);
		dfs_info("kill_sb sync done sb=%pg\n", sb->s_bdev);
		cancel_delayed_work_sync(&sbi->commit_work);
		kfree(sbi);
		sb->s_fs_info = NULL;
	}
	kill_block_super(sb);
}

static const struct fs_context_operations dfs_context_ops = {
	.get_tree	= dfs_get_tree,
};

static int dfs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &dfs_context_ops;
	return 0;
}

static struct file_system_type dfs_fs_type = {
	.name		= "dfs",
	.init_fs_context = dfs_init_fs_context,
	.kill_sb	= dfs_kill_sb,
	.fs_flags	= FS_USERNS_MOUNT,
};

static int __init dfs_init_fs(void)
{
	return register_filesystem(&dfs_fs_type);
}
fs_initcall(dfs_init_fs);
