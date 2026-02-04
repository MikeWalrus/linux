// SPDX-License-Identifier: GPL-2.0-only

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/iomap.h>
#include <linux/writeback.h>
#include <linux/time64.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/ramfs.h>

#include "internal.h"

static struct dfs_inode_info *dfs_inode_info(struct inode *inode)
{
	return inode->i_private;
}

static loff_t dfs_inode_offset(struct super_block *sb, u32 ino)
{
	loff_t table_start = (loff_t)DFS_INODE_TABLE_BLOCK * sb->s_blocksize;
	loff_t offset = (loff_t)ino * DFS_INODE_SIZE;

	return table_start + offset;
}

int dfs_read_inode_disk(struct super_block *sb, u32 ino,
			 struct dfs_disk_inode *out)
{
	struct address_space *mapping;
	struct folio *folio;
	loff_t offset;
	u64 bdev_bytes;
	pgoff_t index;
	unsigned int page_off;
	void *kaddr;

	if (!sb || !sb->s_bdev)
		return -EINVAL;
	if (!out)
		return -EINVAL;
	if (sb->s_blocksize < DFS_INODE_SIZE)
		return -EOPNOTSUPP;

	offset = dfs_inode_offset(sb, ino);
	bdev_bytes = bdev_nr_bytes(sb->s_bdev);
	if (offset + DFS_INODE_SIZE > bdev_bytes)
		return -ENOSPC;

	index = offset >> PAGE_SHIFT;
	page_off = offset & (PAGE_SIZE - 1);
	if (page_off + DFS_INODE_SIZE > PAGE_SIZE)
		return -EOPNOTSUPP;

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
	u64 bdev_bytes;
	pgoff_t index;
	unsigned int page_off;
	void *kaddr;
	struct dfs_disk_inode *disk;

	dfs_info("write_inode enter ino=%lu sync_mode=%d di=%p\n",
		 inode->i_ino, sync_mode, di);

	if (!sb || !sb->s_bdev || !di) {
		dfs_info("write_inode skip ino=%lu sb=%p bdev=%p di=%p\n",
			 inode->i_ino, sb, sb ? sb->s_bdev : NULL, di);
		return -EIO;
	}

	if (inode->i_ino == 0)
		return 0;

	if (sb->s_blocksize < DFS_INODE_SIZE)
		return -EOPNOTSUPP;

	offset = dfs_inode_offset(sb, inode->i_ino);
	bdev_bytes = bdev_nr_bytes(sb->s_bdev);
	if (offset + DFS_INODE_SIZE > bdev_bytes)
		return -ENOSPC;

	index = offset >> PAGE_SHIFT;
	page_off = offset & (PAGE_SIZE - 1);
	if (page_off + DFS_INODE_SIZE > PAGE_SIZE)
		return -EOPNOTSUPP;

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
	loff_t start = ALIGN_DOWN(offset, sb->s_blocksize);
	loff_t max_len;
	u64 bdev_bytes = bdev_nr_bytes(sb->s_bdev);

	if (!di)
		return -EIO;
	if (length <= 0)
		return -EINVAL;

	iomap->bdev = sb->s_bdev;
	iomap->offset = start;
	iomap->addr = di->base + start;
	max_len = di->chunk_bytes - start;

	if (max_len <= 0)
		return -ENOSPC;

	iomap->length = min_t(loff_t, max_len,
				     length + (offset - start));

	if (iomap->addr >= bdev_bytes)
		return -ENOSPC;
	if (iomap->addr + iomap->length > bdev_bytes)
		iomap->length = bdev_bytes - iomap->addr;
	if (iomap->length == 0)
		return -ENOSPC;

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

	if (offset >= di->chunk_bytes)
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
