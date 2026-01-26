// SPDX-License-Identifier: GPL-2.0-only

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/math64.h>
#include <linux/statfs.h>
#include <linux/fs_context.h>
#include <linux/magic.h>
#include <linux/ramfs.h>
#include <linux/slab.h>

#include "internal.h"

static const struct super_operations dfs_ops;
static const struct inode_operations dfs_dir_inode_operations;

static struct inode *dfs_get_inode(struct super_block *sb,
				   const struct inode *dir,
				   umode_t mode, dev_t dev)
{
	struct inode *inode = new_inode(sb);

	if (!inode)
		return NULL;

	inode->i_ino = get_next_ino();
	inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
	inode->i_mapping->a_ops = &ram_aops;
	mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
	mapping_set_unevictable(inode->i_mapping);
	simple_inode_init_ts(inode);

	switch (mode & S_IFMT) {
	default:
		init_special_inode(inode, mode, dev);
		break;
	case S_IFREG:
		inode->i_op = &dfs_file_inode_operations;
		inode->i_fop = &dfs_file_operations;
		break;
	case S_IFDIR:
		inode->i_op = &dfs_dir_inode_operations;
		inode->i_fop = &simple_dir_operations;
		inc_nlink(inode);
		break;
	case S_IFLNK:
		inode->i_op = &page_symlink_inode_operations;
		inode_nohighmem(inode);
		break;
	}

	return inode;
}

static int dfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
		     struct dentry *dentry, umode_t mode, dev_t dev)
{
	struct inode *inode = dfs_get_inode(dir->i_sb, dir, mode, dev);
	int error = -ENOSPC;

	if (!inode)
		return error;

	error = security_inode_init_security(inode, dir,
					     &dentry->d_name, NULL,
					     NULL);
	if (error) {
		iput(inode);
		return error;
	}

	d_make_persistent(dentry, inode);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	return 0;
}

static struct dentry *dfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				 struct dentry *dentry, umode_t mode)
{
	int error = dfs_mknod(&nop_mnt_idmap, dir, dentry, mode | S_IFDIR, 0);

	if (!error)
		inc_nlink(dir);
	return ERR_PTR(error);
}

static int dfs_create(struct mnt_idmap *idmap, struct inode *dir,
		      struct dentry *dentry, umode_t mode, bool excl)
{
	return dfs_mknod(&nop_mnt_idmap, dir, dentry, mode | S_IFREG, 0);
}

static int dfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, const char *symname)
{
	struct inode *inode;
	int error = -ENOSPC;

	inode = dfs_get_inode(dir->i_sb, dir, S_IFLNK | 0777, 0);
	if (!inode)
		return error;

	error = security_inode_init_security(inode, dir,
					     &dentry->d_name, NULL,
					     NULL);
	if (error) {
		iput(inode);
		return error;
	}

	error = page_symlink(inode, symname, strlen(symname) + 1);
	if (error) {
		iput(inode);
		return error;
	}

	d_make_persistent(dentry, inode);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	return 0;
}

static int dfs_tmpfile(struct mnt_idmap *idmap, struct inode *dir,
		       struct file *file, umode_t mode)
{
	struct inode *inode;
	int error;

	inode = dfs_get_inode(dir->i_sb, dir, mode, 0);
	if (!inode)
		return -ENOSPC;

	error = security_inode_init_security(inode, dir,
					     &file_dentry(file)->d_name, NULL,
					     NULL);
	if (error) {
		iput(inode);
		return error;
	}

	d_tmpfile(file, inode);
	return finish_open_simple(file, 0);
}

static const struct inode_operations dfs_dir_inode_operations = {
	.create		= dfs_create,
	.lookup		= simple_lookup,
	.link		= simple_link,
	.unlink		= simple_unlink,
	.symlink	= dfs_symlink,
	.mkdir		= dfs_mkdir,
	.rmdir		= simple_rmdir,
	.mknod		= dfs_mknod,
	.rename		= simple_rename,
	.tmpfile	= dfs_tmpfile,
};

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
};

static int dfs_validate_super(struct super_block *sb)
{
	struct buffer_head *bh;
	struct dfs_super_block *disk;
	int ret = 0;

	bh = sb_bread(sb, 0);
	if (!bh)
		return -EIO;

	disk = (struct dfs_super_block *)bh->b_data;
	if (le32_to_cpu(disk->magic) != DFS_SUPER_MAGIC)
		ret = -EINVAL;

	brelse(bh);
	return ret;
}

static int dfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct inode *inode;

	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_magic = DFS_SUPER_MAGIC;
	if (!sb_set_blocksize(sb, DFS_DEFAULT_BLOCK_SIZE))
		return -EINVAL;

	if (dfs_validate_super(sb))
		return -EINVAL;
	sb->s_op = &dfs_ops;
	sb->s_d_flags = DCACHE_DONTCACHE;
	sb->s_time_gran = 1;

	inode = dfs_get_inode(sb, NULL, S_IFDIR | 0755, 0);
	if (!inode)
		return -ENOMEM;

	sb->s_root = d_make_root(inode);
	if (!sb->s_root)
		return -ENOMEM;

	return 0;
}

static int dfs_get_tree(struct fs_context *fc)
{
	return get_tree_bdev(fc, dfs_fill_super);
}

static void dfs_kill_sb(struct super_block *sb)
{
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
