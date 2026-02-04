// SPDX-License-Identifier: GPL-2.0-only

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/ramfs.h>
#include <linux/slab.h>
#include <linux/iomap.h>
#include <linux/blkdev.h>
#include <linux/writeback.h>
#include <linux/time64.h>
#include <linux/mm.h>
#include <linux/sched.h>

#include "internal.h"

static const struct inode_operations dfs_dir_inode_operations;
static const struct file_operations dfs_dir_operations;

static struct dfs_inode_info *dfs_inode_info(struct inode *inode)
{
	return inode->i_private;
}

int dfs_read_inode_disk(struct super_block *sb, u32 ino,
			 struct dfs_disk_inode *out)
{
	struct address_space *mapping;
	struct folio *folio;
	loff_t offset;
	pgoff_t index;
	unsigned int page_off;
	void *kaddr;
	int ret;

	if (!sb || !sb->s_bdev)
		return -EINVAL;
	if (!out)
		return -EINVAL;

	ret = dfs_inode_disk_location(sb, ino, &offset, &index, &page_off);
	if (ret)
		return ret;

	mapping = sb->s_bdev->bd_mapping;
	folio = read_mapping_folio(mapping, index, NULL);
	if (IS_ERR(folio))
		return PTR_ERR(folio);

	kaddr = kmap_local_folio(folio, 0);
	memcpy(out, kaddr + page_off, sizeof(*out));
	kunmap_local(kaddr);
	folio_put(folio);
	dfs_info("read inode=%u offset=%lld mode=0%o size=%llu base=%llu disk_ino=%u\n",
		 ino, offset, le32_to_cpu(out->mode),
		 (unsigned long long)le64_to_cpu(out->size),
		 (unsigned long long)le64_to_cpu(out->base),
		 le32_to_cpu(out->ino));
	return 0;
}

int dfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	struct super_block *sb = inode->i_sb;
	struct dfs_inode_info *di = dfs_inode_info(inode);
	int sync_mode = wbc ? wbc->sync_mode : -1;
	struct address_space *mapping;
	struct folio *folio;
	loff_t offset;
	pgoff_t index;
	unsigned int page_off;
	void *kaddr;
	struct dfs_disk_inode *disk;
	int ret;

	dfs_info("write_inode enter ino=%lu sync_mode=%d di=%p\n",
		 inode->i_ino, sync_mode, di);

	if (!sb || !sb->s_bdev || !di) {
		dfs_info("write_inode skip ino=%lu sb=%p bdev=%p di=%p\n",
			 inode->i_ino, sb, sb ? sb->s_bdev : NULL, di);
		return -EIO;
	}

	if (inode->i_ino == 0)
		return 0;

	ret = dfs_inode_disk_location(sb, inode->i_ino, &offset, &index, &page_off);
	if (ret)
		return ret;

	mapping = sb->s_bdev->bd_mapping;
	folio = filemap_grab_folio(mapping, index);
	if (IS_ERR(folio))
		return PTR_ERR(folio);
	if (!folio_test_uptodate(folio)) {
		folio_unlock(folio);
		folio_put(folio);
		folio = read_mapping_folio(mapping, index, NULL);
		if (IS_ERR(folio))
			return PTR_ERR(folio);
		folio_lock(folio);
	}

	kaddr = kmap_local_folio(folio, 0);
	disk = (struct dfs_disk_inode *)(kaddr + page_off);
	memset(disk, 0, DFS_INODE_SIZE);
	disk->ino = cpu_to_le32(inode->i_ino);
	disk->base = cpu_to_le64(di->base);
	disk->mode = cpu_to_le32(inode->i_mode);
	disk->nlink = cpu_to_le32(inode->i_nlink);
	disk->size = cpu_to_le64(i_size_read(inode));
	disk->uid = cpu_to_le32(i_uid_read(inode));
	disk->gid = cpu_to_le32(i_gid_read(inode));
	disk->flags = cpu_to_le32(0);
	disk->generation = cpu_to_le32(inode->i_generation);
	{
		struct timespec64 ts;

		ts = inode_get_atime(inode);
		disk->atime_ns = cpu_to_le64(timespec64_to_ns(&ts));
		ts = inode_get_ctime(inode);
		disk->ctime_ns = cpu_to_le64(timespec64_to_ns(&ts));
		disk->btime_ns = cpu_to_le64(0);
		ts = inode_get_mtime(inode);
		disk->mtime_ns = cpu_to_le64(timespec64_to_ns(&ts));
	}

	flush_dcache_folio(folio);
	folio_mark_uptodate(folio);
	kunmap_local(kaddr);
	folio_mark_dirty(folio);
	folio_unlock(folio);
	folio_put(folio);

	if (wbc && wbc->sync_mode == WB_SYNC_ALL)
		filemap_fdatawrite_range(mapping, offset,
					 offset + DFS_INODE_SIZE - 1);
	if (wbc && wbc->sync_mode == WB_SYNC_ALL)
		filemap_fdatawait_range(mapping, offset,
					 offset + DFS_INODE_SIZE - 1);

	dfs_info("write inode=%lu offset=%lld size=%llu mode=0%o nlink=%u base=%llu\n",
		 inode->i_ino, offset, i_size_read(inode), inode->i_mode,
		 inode->i_nlink, (unsigned long long)di->base);

	return 0;
}

static int dfs_iomap_begin(struct inode *inode, loff_t offset, loff_t length,
			   unsigned int flags, struct iomap *iomap,
			   struct iomap *srcmap)
{
	struct dfs_inode_info *di = dfs_inode_info(inode);
	struct super_block *sb = inode->i_sb;
	loff_t isize = i_size_read(inode);
	loff_t start;
	loff_t map_len;
	u64 addr;
	int ret;

	ret = dfs_map_file_range(inode, offset, length, &start, &map_len, &addr);
	if (ret)
		return ret;

	iomap->bdev = sb->s_bdev;
	iomap->offset = start;
	iomap->addr = addr;
	iomap->length = map_len;

	if (!(flags & IOMAP_WRITE)) {
		if (offset >= isize) {
			iomap->type = IOMAP_HOLE;
			iomap->offset = offset;
			iomap->addr = IOMAP_NULL_ADDR;
			iomap->length = length;
			return 0;
		}
		if (start >= isize) {
			iomap->type = IOMAP_HOLE;
			iomap->addr = IOMAP_NULL_ADDR;
			return 0;
		}
		iomap->type = IOMAP_MAPPED;
		iomap->length = min_t(loff_t, iomap->length, isize - start);
		return 0;
	}

	if (!di || offset >= di->chunk_bytes)
		return -ENOSPC;

	iomap->type = IOMAP_MAPPED;
	iomap->flags |= IOMAP_F_DIRTY;
	dfs_info("iomap ino=%lu off=%lld len=%lld\n",
		 inode->i_ino, offset, iomap->length);
	return 0;
}

const struct iomap_ops dfs_iomap_ops = {
	.iomap_begin = dfs_iomap_begin,
};

static int dfs_read_folio(struct file *file, struct folio *folio)
{
	iomap_bio_read_folio(folio, &dfs_iomap_ops);
	return 0;
}

static void dfs_readahead(struct readahead_control *rac)
{
	iomap_bio_readahead(rac, &dfs_iomap_ops);
}

static ssize_t dfs_writeback_range(struct iomap_writepage_ctx *wpc,
				  struct folio *folio, u64 offset,
				  unsigned int len, u64 end_pos)
{
	if (end_pos <= offset)
		return len;

	if (offset + len > end_pos)
		len = end_pos - offset;

	if (offset < wpc->iomap.offset ||
	    offset >= wpc->iomap.offset + wpc->iomap.length) {
		int error;

		error = dfs_iomap_begin(wpc->inode, offset, end_pos - offset,
					 IOMAP_WRITE, &wpc->iomap, NULL);
		if (error)
			return error;
	}

	return iomap_add_to_ioend(wpc, folio, offset, end_pos, len);
}

static const struct iomap_writeback_ops dfs_writeback_ops = {
	.writeback_range	= dfs_writeback_range,
	.writeback_submit	= iomap_ioend_writeback_submit,
};

static int dfs_writepages(struct address_space *mapping,
			   struct writeback_control *wbc)
{
	struct iomap_writepage_ctx wpc = {
		.inode	= mapping->host,
		.wbc	= wbc,
		.ops	= &dfs_writeback_ops,
	};

	return iomap_writepages(&wpc);
}

const struct address_space_operations dfs_aops = {
	.dirty_folio		= iomap_dirty_folio,
	.release_folio		= iomap_release_folio,
	.invalidate_folio	= iomap_invalidate_folio,
	.read_folio		= dfs_read_folio,
	.readahead		= dfs_readahead,
	.writepages		= dfs_writepages,
	.is_partially_uptodate	= iomap_is_partially_uptodate,
	.error_remove_folio	= generic_error_remove_folio,
	.migrate_folio		= filemap_migrate_folio,
};

void dfs_free_inode(struct inode *inode)
{
	kfree(inode->i_private);
	inode->i_private = NULL;
}

static unsigned long dfs_get_unmapped_area(struct file *file,
				 unsigned long addr,
				 unsigned long len,
				 unsigned long pgoff,
				 unsigned long flags)
{
	return mm_get_unmapped_area(file, addr, len, pgoff, flags);
}

static ssize_t dfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret;

	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		goto out_unlock;

	ret = iomap_file_buffered_write(iocb, from, &dfs_iomap_ops, NULL, NULL);
out_unlock:
	inode_unlock(inode);
	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	if (ret > 0) {
		mark_inode_dirty(inode);
		dfs_schedule_commit(inode->i_sb);
	}
	dfs_info("write_iter ino=%lu ret=%zd\n", inode->i_ino, ret);
	return ret;
}

const struct file_operations dfs_file_operations = {
	.read_iter	= generic_file_read_iter,
	.write_iter	= dfs_file_write_iter,
	.mmap_prepare	= generic_file_mmap_prepare,
	.fsync		= generic_file_fsync,
	.splice_read	= filemap_splice_read,
	.splice_write	= iter_file_splice_write,
	.llseek		= generic_file_llseek,
	.get_unmapped_area	= dfs_get_unmapped_area,
};

const struct inode_operations dfs_file_inode_operations = {
	.setattr	= dfs_setattr,
	.getattr	= simple_getattr,
};

struct inode *dfs_get_inode(struct super_block *sb,
			    const struct inode *dir,
			    umode_t mode, dev_t dev)
{
	struct inode *inode = new_inode(sb);
	int error;
	struct dfs_sb_info *sbi = sb->s_fs_info;

	if (!inode)
		return NULL;

	if (!sbi) {
		iput(inode);
		return NULL;
	}

	if (!dir)
		inode->i_ino = 1;
	else
		inode->i_ino = (u32)atomic_fetch_inc(&sbi->next_ino);
	insert_inode_hash(inode);
	inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
	inode->i_mapping->a_ops = &dfs_aops;
	mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
	mapping_set_unevictable(inode->i_mapping);
	simple_inode_init_ts(inode);

	error = dfs_alloc_inode_info(inode);
	if (error) {
		iput(inode);
		return NULL;
	}

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
		inode->i_fop = &dfs_dir_operations;
		inc_nlink(inode);
		break;
	case S_IFLNK:
		inode->i_op = &page_symlink_inode_operations;
		inode->i_mapping->a_ops = &ram_aops;
		inode_nohighmem(inode);
		break;
	}
	return inode;
}

struct inode *dfs_iget(struct super_block *sb, u32 ino, umode_t mode)
{
	struct inode *inode = iget_locked(sb, ino);
	int error;
	struct dfs_disk_inode disk;
	struct timespec64 ts;

	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode_state_read_once(inode) & I_NEW))
		return inode;

	inode_init_owner(&nop_mnt_idmap, inode, NULL, mode);
	inode->i_mapping->a_ops = &dfs_aops;
	mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
	mapping_set_unevictable(inode->i_mapping);
	simple_inode_init_ts(inode);

	error = dfs_read_inode_disk(sb, ino, &disk);
	if (!error) {
		inode->i_mode = le32_to_cpu(disk.mode);
		set_nlink(inode, le32_to_cpu(disk.nlink));
		i_size_write(inode, le64_to_cpu(disk.size));
		inode->i_uid = make_kuid(&init_user_ns, le32_to_cpu(disk.uid));
		inode->i_gid = make_kgid(&init_user_ns, le32_to_cpu(disk.gid));
		inode->i_generation = le32_to_cpu(disk.generation);
		ts = ns_to_timespec64(le64_to_cpu(disk.atime_ns));
		inode_set_atime_to_ts(inode, ts);
		ts = ns_to_timespec64(le64_to_cpu(disk.ctime_ns));
		inode_set_ctime_to_ts(inode, ts);
		ts = ns_to_timespec64(le64_to_cpu(disk.mtime_ns));
		inode_set_mtime_to_ts(inode, ts);
	}

	error = dfs_alloc_inode_info(inode);
	if (error) {
		iget_failed(inode);
		return ERR_PTR(error);
	}
	if (!error) {
		struct dfs_inode_info *di = inode->i_private;
		u64 base = le64_to_cpu(disk.base);
		u32 disk_ino = le32_to_cpu(disk.ino);
		u32 disk_mode = le32_to_cpu(disk.mode);
		bool base_ok = di && disk_ino == ino && disk_mode != 0 &&
				  base != 0 &&
				  dfs_inode_base_valid(inode->i_sb, ino,
						      base, di->chunk_bytes);

		dfs_info("iget ino=%u disk_ino=%u mode=0%o size=%llu base=%llu valid=%d\n",
			 ino, disk_ino, disk_mode,
			 (unsigned long long)le64_to_cpu(disk.size),
			 (unsigned long long)base, base_ok);

		if (base_ok)
			di->base = base;
	}

	switch (inode->i_mode & S_IFMT) {
	default:
		init_special_inode(inode, mode, 0);
		break;
	case S_IFREG:
		inode->i_op = &dfs_file_inode_operations;
		inode->i_fop = &dfs_file_operations;
		break;
	case S_IFDIR:
		inode->i_op = &dfs_dir_inode_operations;
		inode->i_fop = &dfs_dir_operations;
		inc_nlink(inode);
		break;
	case S_IFLNK:
		inode->i_op = &page_symlink_inode_operations;
		inode->i_mapping->a_ops = &ram_aops;
		inode_nohighmem(inode);
		break;
	}

	unlock_new_inode(inode);
	return inode;
}

static struct inode *dfs_lookup_inode(struct inode *dir,
				      const struct qstr *name)
{
	loff_t pos;
	u16 rec_len;
	u32 ino;
	u8 type;
	umode_t mode;
	int ret;

	ret = dfs_find_entry(dir, name, &pos, &rec_len, &ino, &type);
	if (ret) {
		dfs_info("lookup miss dir=%lu name=%.*s ret=%d\n",
			 dir->i_ino, name->len, name->name, ret);
		return ERR_PTR(ret);
	}

	mode = dfs_dtype_to_mode(type) | 0644;

	return dfs_iget(dir->i_sb, ino, mode);
}

static struct dentry *dfs_lookup(struct inode *dir, struct dentry *dentry,
			       unsigned int flags)
{
	struct inode *inode;

	if (dentry->d_name.len > DFS_DIRENT_NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);

	inode = dfs_lookup_inode(dir, &dentry->d_name);
	if (inode && !IS_ERR(inode)) {
		d_add(dentry, inode);
		return NULL;
	}
	if (IS_ERR(inode)) {
		if (PTR_ERR(inode) == -ENOENT) {
			d_add(dentry, NULL);
			return NULL;
		}
		return ERR_PTR(PTR_ERR(inode));
	}
	return NULL;
}

static int dfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
		     struct dentry *dentry, umode_t mode, dev_t dev)
{
	struct inode *inode = dfs_get_inode(dir->i_sb, dir, mode, dev);
	int error = -ENOSPC;

	if (dentry->d_name.len > DFS_DIRENT_NAME_MAX)
		return -ENAMETOOLONG;

	if (!inode)
		return error;

	error = security_inode_init_security(inode, dir,
					     &dentry->d_name, NULL,
					     NULL);
	if (error) {
		iput(inode);
		return error;
	}

	mark_inode_dirty(inode);

	if (S_ISDIR(mode)) {
		error = dfs_make_empty(inode, dir);
		if (error) {
			iput(inode);
			return error;
		}
	}

	error = dfs_add_entry(dir, &dentry->d_name, inode);
	if (error) {
		iput(inode);
		return error;
	}

	mark_inode_dirty(inode);
	dfs_info("mark dirty ino=%lu mode=0%o size=%lld\n",
		 inode->i_ino, inode->i_mode, inode->i_size);

	dfs_info("add entry name=%s ino=%lu dir=%lu\n",
		 dentry->d_name.name, inode->i_ino, dir->i_ino);

	if (S_ISDIR(mode))
		inc_nlink(dir);

	mark_inode_dirty(inode);

	d_make_persistent(dentry, inode);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	dfs_schedule_commit(dir->i_sb);
	return 0;
}

static struct dentry *dfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				 struct dentry *dentry, umode_t mode)
{
	int error = dfs_mknod(&nop_mnt_idmap, dir, dentry, mode | S_IFDIR, 0);

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

	if (dentry->d_name.len > DFS_DIRENT_NAME_MAX)
		return -ENAMETOOLONG;

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

	error = dfs_add_entry(dir, &dentry->d_name, inode);
	if (error) {
		iput(inode);
		return error;
	}

	mark_inode_dirty(inode);
	dfs_info("mark dirty ino=%lu mode=0%o size=%lld (symlink)\n",
		 inode->i_ino, inode->i_mode, inode->i_size);

	dfs_info("symlink name=%s ino=%lu dir=%lu\n",
		 dentry->d_name.name, inode->i_ino, dir->i_ino);

	d_make_persistent(dentry, inode);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	dfs_schedule_commit(dir->i_sb);
	return 0;
}

static int dfs_link(struct dentry *old_dentry, struct inode *dir,
		     struct dentry *dentry)
{
	struct inode *inode = d_inode(old_dentry);
	int error;

	error = dfs_add_entry(dir, &dentry->d_name, inode);
	if (error)
		return error;

	ihold(inode);
	inc_nlink(inode);
	inode_set_ctime_current(inode);
	d_make_persistent(dentry, inode);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	dfs_schedule_commit(dir->i_sb);
	return 0;
}

static int dfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	int error;

	error = dfs_delete_entry(dir, &dentry->d_name);
	if (error)
		return error;

	dfs_info("unlink name=%s ino=%lu dir=%lu\n",
		 dentry->d_name.name, inode->i_ino, dir->i_ino);

	drop_nlink(inode);
	inode_set_ctime_current(inode);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	dfs_schedule_commit(dir->i_sb);
	return 0;
}
static int dfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	int error;

	if (!dfs_empty_dir(inode))
		return -ENOTEMPTY;

	error = dfs_delete_entry(dir, &dentry->d_name);
	if (error)
		return error;

	dfs_info("rmdir name=%s ino=%lu dir=%lu\n",
		 dentry->d_name.name, inode->i_ino, dir->i_ino);

	drop_nlink(inode);
	drop_nlink(inode);
	drop_nlink(dir);
	inode_set_ctime_current(inode);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	dfs_schedule_commit(dir->i_sb);
	return 0;
}

static int dfs_rename(struct mnt_idmap *idmap, struct inode *old_dir,
		      struct dentry *old_dentry, struct inode *new_dir,
		      struct dentry *new_dentry, unsigned int flags)
{
	struct inode *inode = d_inode(old_dentry);
	int error;
	bool they_are_dirs = d_is_dir(old_dentry);

	if (flags)
		return -EINVAL;

	if (d_really_is_positive(new_dentry) && they_are_dirs &&
	    !dfs_empty_dir(d_inode(new_dentry)))
		return -ENOTEMPTY;

	if (new_dentry->d_inode) {
		error = dfs_delete_entry(new_dir, &new_dentry->d_name);
		if (error)
			return error;
	}

	error = dfs_delete_entry(old_dir, &old_dentry->d_name);
	if (error)
		return error;

	error = dfs_add_entry(new_dir, &new_dentry->d_name, inode);
	if (error)
		return error;

	if (d_really_is_positive(new_dentry)) {
		drop_nlink(d_inode(new_dentry));
		if (they_are_dirs) {
			drop_nlink(d_inode(new_dentry));
			drop_nlink(old_dir);
		}
	} else if (they_are_dirs && old_dir != new_dir) {
		drop_nlink(old_dir);
		inc_nlink(new_dir);
	}

	inode_set_ctime_current(inode);
	inode_set_mtime_to_ts(old_dir, inode_set_ctime_current(old_dir));
	if (old_dir != new_dir)
		inode_set_mtime_to_ts(new_dir, inode_set_ctime_current(new_dir));
	dfs_schedule_commit(old_dir->i_sb);
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
	.lookup		= dfs_lookup,
	.link		= dfs_link,
	.unlink		= dfs_unlink,
	.symlink	= dfs_symlink,
	.mkdir		= dfs_mkdir,
	.rmdir		= dfs_rmdir,
	.mknod		= dfs_mknod,
	.rename		= dfs_rename,
	.tmpfile	= dfs_tmpfile,
	.setattr	= dfs_setattr,
	.getattr	= simple_getattr,
};

static const struct file_operations dfs_dir_operations = {
	.iterate_shared	= dfs_readdir,
	.fsync		= generic_file_fsync,
	.llseek		= generic_file_llseek,
};

