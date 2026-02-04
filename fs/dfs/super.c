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

#include "internal.h"

static const struct super_operations dfs_ops;
static int dfs_sync_fs(struct super_block *sb, int wait);
static void dfs_dirty_inode(struct inode *inode, int flags);

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
	dfs_info("sync_fs wait=%d sb=%pg\n", wait, sb->s_bdev);
	return 0;
}

static bool dfs_root_dir_valid(struct inode *inode)
{
	struct folio *folio;
	void *kaddr;
	struct dfs_dir_entry *de1;
	struct dfs_dir_entry *de2;
	unsigned int rec_len1;
	unsigned int rec_len2;

	if (inode->i_size < 2 * DFS_DIR_REC_LEN(1))
		return false;

	folio = read_mapping_folio(inode->i_mapping, 0, NULL);
	if (IS_ERR(folio))
		return false;

	kaddr = kmap_local_folio(folio, 0);
	de1 = (struct dfs_dir_entry *)kaddr;
	rec_len1 = le16_to_cpu(de1->rec_len);
	if (!rec_len1 || rec_len1 > inode->i_sb->s_blocksize)
		goto invalid;
	de2 = (struct dfs_dir_entry *)(kaddr + rec_len1);
	rec_len2 = le16_to_cpu(de2->rec_len);
	if (!rec_len2 || rec_len1 + rec_len2 > inode->i_sb->s_blocksize)
		goto invalid;
	if (de1->name_len != 1 || de1->name[0] != '.')
		goto invalid;
	if (de2->name_len != 2 || de2->name[0] != '.' || de2->name[1] != '.')
		goto invalid;

	kunmap_local(kaddr);
	folio_put(folio);
	return true;

invalid:
	kunmap_local(kaddr);
	folio_put(folio);
	return false;
}

static int dfs_validate_super(struct super_block *sb)
{
	struct folio *folio;
	struct dfs_super_block *disk;
	int ret = 0;
	struct address_space *mapping;

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

	folio_put(folio);
	return ret;
}

static int dfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct inode *inode;
	struct dfs_sb_info *sbi;
	struct dfs_disk_inode disk;
	int error;
	int ret = 0;
	bool have_root = false;
	u64 blocks;

	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_magic = DFS_SUPER_MAGIC;
	if (!sb_set_blocksize(sb, DFS_DEFAULT_BLOCK_SIZE))
		return -EINVAL;
	if (sb->s_bdev)
		invalidate_bdev(sb->s_bdev);

	if (dfs_validate_super(sb))
		return -EINVAL;

	dfs_info("mount: dev=%pg blocksize=%lu\n", sb->s_bdev,
		 sb->s_blocksize);

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sbi->sb = sb;
	atomic_set(&sbi->commit_pending, 0);
	INIT_DELAYED_WORK(&sbi->commit_work, dfs_commit_work);
	atomic_set(&sbi->next_ino, 2);
	blocks = div_u64(bdev_nr_bytes(sb->s_bdev), sb->s_blocksize);
	sbi->chunk_blocks = min_t(u64, 1024, max_t(u64, 1, blocks));
	sb->s_fs_info = sbi;
	sb->s_op = &dfs_ops;
	sb->s_d_flags = DCACHE_DONTCACHE;
	sb->s_time_gran = 1;

	if (!dfs_read_inode_disk(sb, 1, &disk)) {
		u32 mode = le32_to_cpu(disk.mode);
		u64 size = le64_to_cpu(disk.size);

		if (le32_to_cpu(disk.ino) == 1 && mode != 0 && S_ISDIR(mode) &&
		    size % sb->s_blocksize == 0)
			have_root = true;
	}

	if (have_root) {
		inode = dfs_iget(sb, 1, le32_to_cpu(disk.mode));
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
