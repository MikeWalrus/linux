// SPDX-License-Identifier: GPL-2.0-only

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/ramfs.h>
#include <linux/slab.h>
#include <linux/dirent.h>
#include <linux/iomap.h>
#include <linux/blkdev.h>

#include "internal.h"

static const struct inode_operations dfs_dir_inode_operations;
static const struct file_operations dfs_dir_operations;

static unsigned int dfs_dir_rec_len(unsigned int name_len)
{
	return ALIGN(DFS_DIR_REC_LEN(name_len), 4);
}

static unsigned int dfs_last_byte(struct inode *inode, unsigned long index)
{
	loff_t size = inode->i_size;
	loff_t offset = (loff_t)index << PAGE_SHIFT;

	if (offset >= size)
		return 0;
	if (size - offset > PAGE_SIZE)
		return PAGE_SIZE;
	return size - offset;
}

static umode_t dfs_dtype_to_mode(u8 dtype)
{
	switch (dtype) {
	case DT_DIR:
		return S_IFDIR;
	case DT_REG:
		return S_IFREG;
	case DT_LNK:
		return S_IFLNK;
	case DT_CHR:
		return S_IFCHR;
	case DT_BLK:
		return S_IFBLK;
	case DT_FIFO:
		return S_IFIFO;
	case DT_SOCK:
		return S_IFSOCK;
	default:
		return S_IFREG;
	}
}

static bool dfs_dirent_valid(struct dfs_dir_entry *de, unsigned int remaining)
{
	unsigned int rec_len = le16_to_cpu(de->rec_len);
	unsigned int min_len;

	if (!rec_len || rec_len > remaining)
		return false;
	if (de->name_len > DFS_DIRENT_NAME_MAX)
		return false;
	min_len = dfs_dir_rec_len(de->name_len);
	if (rec_len < min_len)
		return false;
	return true;
}

static int dfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	loff_t pos = ctx->pos;

	if (pos >= inode->i_size)
		return 0;

	while (pos < inode->i_size) {
		unsigned long index = pos >> PAGE_SHIFT;
		unsigned int offset = pos & (PAGE_SIZE - 1);
		unsigned int limit = dfs_last_byte(inode, index);
		struct folio *folio;
		void *kaddr;

		if (!limit)
			break;

		folio = read_mapping_folio(inode->i_mapping, index, NULL);
		if (IS_ERR(folio))
			return PTR_ERR(folio);

		kaddr = kmap_local_folio(folio, 0);

		while (offset + sizeof(struct dfs_dir_entry) <= limit) {
			struct dfs_dir_entry *de = (struct dfs_dir_entry *)(kaddr + offset);
			unsigned int remaining = limit - offset;
			unsigned int rec_len;
			unsigned int ino;
			unsigned char d_type;

			if (!dfs_dirent_valid(de, remaining)) {
				dfs_info("find_entry invalid dir=%lu pos=%lld rem=%u rec_len=%u name_len=%u\n",
					 inode->i_ino, pos, remaining,
					 le16_to_cpu(de->rec_len), de->name_len);
				kunmap_local(kaddr);
				folio_put(folio);
				return -EIO;
			}

			rec_len = le16_to_cpu(de->rec_len);
			ino = le32_to_cpu(de->ino);
			if (ino) {
				d_type = fs_ftype_to_dtype(de->file_type);
				if (!dir_emit(ctx, de->name, de->name_len, ino,
					      d_type)) {
					kunmap_local(kaddr);
					folio_put(folio);
					return 0;
				}
			}

			pos += rec_len;
			offset += rec_len;
			ctx->pos = pos;
			if (pos >= inode->i_size)
				break;
		}

		kunmap_local(kaddr);
		folio_put(folio);
		if (offset >= PAGE_SIZE)
			continue;
		if (pos < inode->i_size)
			pos = (loff_t)(index + 1) << PAGE_SHIFT;
		ctx->pos = pos;
	}

	return 0;
}

static int dfs_find_entry(struct inode *dir, const struct qstr *name,
			  loff_t *pos_out, u16 *rec_len_out,
			  u32 *ino_out, u8 *type_out)
{
	loff_t pos = 0;

	while (pos < dir->i_size) {
		unsigned long index = pos >> PAGE_SHIFT;
		unsigned int offset = pos & (PAGE_SIZE - 1);
		unsigned int limit = dfs_last_byte(dir, index);
		struct folio *folio;
		void *kaddr;

		if (!limit)
			break;

		folio = read_mapping_folio(dir->i_mapping, index, NULL);
		if (IS_ERR(folio))
			return PTR_ERR(folio);

		kaddr = kmap_local_folio(folio, 0);
		while (offset + sizeof(struct dfs_dir_entry) <= limit) {
			struct dfs_dir_entry *de = (struct dfs_dir_entry *)(kaddr + offset);
			unsigned int remaining = limit - offset;
			unsigned int rec_len;

			if (!dfs_dirent_valid(de, remaining)) {
				dfs_info("find_entry invalid dir=%lu pos=%lld rem=%u rec_len=%u name_len=%u\n",
					 dir->i_ino, pos, remaining,
					 le16_to_cpu(de->rec_len), de->name_len);
				kunmap_local(kaddr);
				folio_put(folio);
				return -EIO;
			}

			rec_len = le16_to_cpu(de->rec_len);
			if (le32_to_cpu(de->ino) &&
			    de->name_len == name->len &&
			    !memcmp(de->name, name->name, name->len)) {
				*pos_out = pos;
				*rec_len_out = rec_len;
				*ino_out = le32_to_cpu(de->ino);
				*type_out = de->file_type;
				dfs_info("lookup hit dir=%lu name=%.*s ino=%u type=%u\n",
					 dir->i_ino, name->len, name->name, *ino_out, *type_out);
				kunmap_local(kaddr);
				folio_put(folio);
				return 0;
			}

			pos += rec_len;
			offset += rec_len;
			if (pos >= dir->i_size)
				break;
		}
		kunmap_local(kaddr);
		folio_put(folio);
		if (offset >= PAGE_SIZE)
			continue;
		pos = (loff_t)(index + 1) << PAGE_SHIFT;
	}

	return -ENOENT;
}

static int dfs_write_dirent(struct inode *dir, loff_t pos, u16 rec_len,
			    const struct qstr *name, struct inode *inode)
{
	unsigned long index = pos >> PAGE_SHIFT;
	unsigned int offset = pos & (PAGE_SIZE - 1);
	struct folio *folio;
	void *kaddr;
	struct dfs_dir_entry *de;

	folio = filemap_grab_folio(dir->i_mapping, index);
	if (IS_ERR(folio))
		return PTR_ERR(folio);

	kaddr = kmap_local_folio(folio, 0);
	de = (struct dfs_dir_entry *)(kaddr + offset);
	memset(de, 0, rec_len);
	if (inode) {
		de->ino = cpu_to_le32(inode->i_ino);
		de->name_len = name->len;
		de->file_type = fs_umode_to_ftype(inode->i_mode);
		memcpy(de->name, name->name, name->len);
	}
	de->rec_len = cpu_to_le16(rec_len);
	flush_dcache_folio(folio);
	folio_mark_uptodate(folio);
	kunmap_local(kaddr);
	iomap_dirty_folio(dir->i_mapping, folio);
	folio_unlock(folio);
	folio_put(folio);

	if (pos + rec_len > dir->i_size)
		i_size_write(dir, pos + rec_len);
	mark_inode_dirty(dir);
	filemap_fdatawrite_range(dir->i_mapping, pos, pos + rec_len - 1);
	filemap_fdatawait_range(dir->i_mapping, pos, pos + rec_len - 1);
	dfs_info("write dirent dir=%lu pos=%lld rec_len=%u ino=%lu size=%lld\n",
		 dir->i_ino, pos, rec_len, inode ? inode->i_ino : 0,
		 dir->i_size);
	return 0;
}

static int dfs_add_entry(struct inode *dir, const struct qstr *name,
			 struct inode *inode)
{
	loff_t pos = 0;
	unsigned int rec_len = dfs_dir_rec_len(name->len);

	while (pos < dir->i_size) {
		unsigned long index = pos >> PAGE_SHIFT;
		unsigned int offset = pos & (PAGE_SIZE - 1);
		unsigned int limit = dfs_last_byte(dir, index);
		struct folio *folio;
		void *kaddr;

		if (!limit)
			break;

		folio = read_mapping_folio(dir->i_mapping, index, NULL);
		if (IS_ERR(folio))
			return PTR_ERR(folio);

		kaddr = kmap_local_folio(folio, 0);
		while (offset + sizeof(struct dfs_dir_entry) <= limit) {
			struct dfs_dir_entry *de = (struct dfs_dir_entry *)(kaddr + offset);
			unsigned int remaining = limit - offset;
			unsigned int slot_len;
			unsigned int min_len;

			if (!dfs_dirent_valid(de, remaining)) {
				kunmap_local(kaddr);
				folio_put(folio);
				return -EIO;
			}

			slot_len = le16_to_cpu(de->rec_len);
			min_len = dfs_dir_rec_len(de->name_len);
			if (le32_to_cpu(de->ino) && slot_len >= min_len + rec_len) {
				loff_t new_pos = pos + min_len;
				u16 new_len = slot_len - min_len;

				dfs_info("split dirent dir=%lu pos=%lld slot=%u min=%u new_len=%u\n",
					 dir->i_ino, pos, slot_len, min_len, new_len);
				de->rec_len = cpu_to_le16(min_len);
				flush_dcache_folio(folio);
				folio_mark_uptodate(folio);
				iomap_dirty_folio(dir->i_mapping, folio);
				kunmap_local(kaddr);
				folio_put(folio);
				filemap_fdatawrite_range(dir->i_mapping, pos,
							pos + min_len - 1);
				filemap_fdatawait_range(dir->i_mapping, pos,
							pos + min_len - 1);
				return dfs_write_dirent(dir, new_pos, new_len, name, inode);
			}
			if (!de->ino && slot_len >= rec_len) {
				dfs_info("reuse empty dirent dir=%lu pos=%lld slot=%u\n",
					 dir->i_ino, pos, slot_len);
				kunmap_local(kaddr);
				folio_put(folio);
				return dfs_write_dirent(dir, pos, slot_len, name, inode);
			}

			pos += slot_len;
			offset += slot_len;
			if (pos >= dir->i_size)
				break;
		}
		kunmap_local(kaddr);
		folio_put(folio);
		if (offset >= PAGE_SIZE)
			continue;
		pos = (loff_t)(index + 1) << PAGE_SHIFT;
	}

	{
		loff_t end = dir->i_size;
		unsigned int block_off = end & (dir->i_sb->s_blocksize - 1);
		unsigned int rem = dir->i_sb->s_blocksize - block_off;
		unsigned int fill = rem ? rem : dir->i_sb->s_blocksize;

		dfs_info("append dirent dir=%lu pos=%lld len=%u fill=%u\n",
			 dir->i_ino, end, rec_len, fill);
		return dfs_write_dirent(dir, end, max_t(unsigned int, rec_len, fill),
					name, inode);
	}
}

static int dfs_delete_entry(struct inode *dir, const struct qstr *name)
{
	loff_t pos;
	u16 rec_len;
	u32 ino;
	u8 type;
	int ret;
	struct qstr empty = QSTR_INIT("", 0);

	ret = dfs_find_entry(dir, name, &pos, &rec_len, &ino, &type);
	if (ret)
		return ret;

	return dfs_write_dirent(dir, pos, rec_len, &empty, NULL);
}

int dfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		 struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	int ret;

	ret = simple_setattr(idmap, dentry, attr);
	if (ret)
		return ret;
	dfs_info("setattr ino=%lu size=%lld mode=0%o\n",
		 inode->i_ino, inode->i_size, inode->i_mode);

	mark_inode_dirty(inode);
	dfs_schedule_commit(inode->i_sb);
	return 0;
}

int dfs_make_empty(struct inode *inode, struct inode *parent)
{
	unsigned int rec_len1 = dfs_dir_rec_len(1);
	unsigned int rec_len2 = inode->i_sb->s_blocksize - rec_len1;
	struct qstr dot = QSTR_INIT(".", 1);
	struct qstr dotdot = QSTR_INIT("..", 2);
	int ret;

	i_size_write(inode, 0);
	ret = dfs_write_dirent(inode, 0, rec_len1, &dot, inode);
	if (ret)
		return ret;
	return dfs_write_dirent(inode, rec_len1, rec_len2, &dotdot, parent);
}

static int dfs_empty_dir(struct inode *inode)
{
	loff_t pos = 0;

	while (pos < inode->i_size) {
		unsigned long index = pos >> PAGE_SHIFT;
		unsigned int offset = pos & (PAGE_SIZE - 1);
		unsigned int limit = dfs_last_byte(inode, index);
		struct folio *folio;
		void *kaddr;

		if (!limit)
			break;
		folio = read_mapping_folio(inode->i_mapping, index, NULL);
		if (IS_ERR(folio))
			return 0;
		kaddr = kmap_local_folio(folio, 0);
		while (offset + sizeof(struct dfs_dir_entry) <= limit) {
			struct dfs_dir_entry *de = (struct dfs_dir_entry *)(kaddr + offset);
			unsigned int remaining = limit - offset;
			unsigned int rec_len;

			if (!dfs_dirent_valid(de, remaining)) {
				kunmap_local(kaddr);
				folio_put(folio);
				return 0;
			}

			rec_len = le16_to_cpu(de->rec_len);
			if (le32_to_cpu(de->ino)) {
				if (de->name_len > 2 ||
				    (de->name_len == 1 && de->name[0] != '.') ||
				    (de->name_len == 2 &&
				     (de->name[0] != '.' || de->name[1] != '.'))) {
					kunmap_local(kaddr);
					folio_put(folio);
					return 0;
				}
			}
			pos += rec_len;
			offset += rec_len;
			if (pos >= inode->i_size)
				break;
		}
		kunmap_local(kaddr);
		folio_put(folio);
		if (offset >= PAGE_SIZE)
			continue;
		pos = (loff_t)(index + 1) << PAGE_SHIFT;
	}

	return 1;
}

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
		u64 bdev_bytes = bdev_nr_bytes(inode->i_sb->s_bdev);
		u32 disk_ino = le32_to_cpu(disk.ino);
		u32 disk_mode = le32_to_cpu(disk.mode);
		u64 expected_base = di ? (u64)ino * di->chunk_bytes : 0;
		bool base_ok = di && disk_ino == ino && disk_mode != 0 &&
				  base != 0 && base < bdev_bytes &&
				  base % di->chunk_bytes == 0 &&
				  base == expected_base;

		dfs_info("iget ino=%u disk_ino=%u mode=0%o size=%llu base=%llu expected=%llu valid=%d\n",
			 ino, disk_ino, disk_mode,
			 (unsigned long long)le64_to_cpu(disk.size),
			 (unsigned long long)base,
			 (unsigned long long)expected_base, base_ok);

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

