// SPDX-License-Identifier: GPL-2.0-only

#include <linux/fs.h>
#include <linux/dirent.h>
#include <linux/pagemap.h>
#include <linux/iomap.h>
#include <linux/blkdev.h>

#include "internal.h"

unsigned int dfs_dir_rec_len(unsigned int name_len)
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

umode_t dfs_dtype_to_mode(u8 dtype)
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

umode_t dfs_dirent_mode(u8 file_type)
{
	return dfs_dtype_to_mode(fs_ftype_to_dtype(file_type));
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

int dfs_readdir(struct file *file, struct dir_context *ctx)
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

int dfs_find_entry(struct inode *dir, const struct qstr *name,
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
		if (pos < dir->i_size)
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
	dir->i_blkbits = dir->i_sb->s_blocksize_bits;
	if (dir->i_size >= dir->i_sb->s_blocksize) {
		loff_t block_start = ALIGN_DOWN(pos, dir->i_sb->s_blocksize);
		loff_t block_end = block_start + dir->i_sb->s_blocksize - 1;

		dfs_info("dirent writeback dir=%lu pos=%lld rec_len=%u range=%lld..%lld blocksize=%lu\n",
			 dir->i_ino, pos, rec_len,
			 block_start, block_end,
			 dir->i_sb->s_blocksize);
		/* filemap_fdatawrite_range(dir->i_mapping, block_start, block_end);
		filemap_fdatawait_range(dir->i_mapping, block_start, block_end); */
	}
	return 0;
}

int dfs_add_entry(struct inode *dir, const struct qstr *name,
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
				return dfs_write_dirent(dir, new_pos, new_len,
							 name, inode);
			}
			if (!de->ino && slot_len >= rec_len) {
				dfs_info("reuse empty dirent dir=%lu pos=%lld slot=%u\n",
					 dir->i_ino, pos, slot_len);
				kunmap_local(kaddr);
				folio_put(folio);
				return dfs_write_dirent(dir, pos, slot_len,
							 name, inode);
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
		return dfs_write_dirent(dir, end,
					 max_t(unsigned int, rec_len, fill),
					 name, inode);
	}
}

int dfs_delete_entry(struct inode *dir, const struct qstr *name)
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

int dfs_empty_dir(struct inode *inode)
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

bool dfs_root_dir_valid(struct inode *inode)
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
