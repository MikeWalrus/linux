// SPDX-License-Identifier: GPL-2.0-only

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/ramfs.h>
#include <linux/sched.h>

#include "internal.h"

static unsigned long dfs_get_unmapped_area(struct file *file,
					 unsigned long addr,
					 unsigned long len,
					 unsigned long pgoff,
					 unsigned long flags)
{
	return mm_get_unmapped_area(file, addr, len, pgoff, flags);
}

const struct file_operations dfs_file_operations = {
	.read_iter	= generic_file_read_iter,
	.write_iter	= generic_file_write_iter,
	.mmap_prepare	= generic_file_mmap_prepare,
	.fsync		= noop_fsync,
	.splice_read	= filemap_splice_read,
	.splice_write	= iter_file_splice_write,
	.llseek		= generic_file_llseek,
	.get_unmapped_area	= dfs_get_unmapped_area,
};

const struct inode_operations dfs_file_inode_operations = {
	.setattr	= simple_setattr,
	.getattr	= simple_getattr,
};
