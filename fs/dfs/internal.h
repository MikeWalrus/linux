/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/fs.h>
#include <linux/workqueue.h>

#define dfs_info(fmt, ...) \
	pr_debug("dfs: " fmt, ##__VA_ARGS__)

struct iomap_ops;

#define DFS_SUPER_MAGIC 0x00444653
#define DFS_SUPER_VERSION 1
#define DFS_DEFAULT_BLOCK_SIZE 4096
#define DFS_CHUNK_BYTES (2ULL * 1024 * 1024 * 1024 * 1024)
#define DFS_INODE_SIZE 512
#define DFS_INODE_TABLE_BLOCK 1

#define DFS_DIRENT_NAME_MAX 255
#define DFS_DIR_REC_LEN(name_len) (sizeof(struct dfs_dir_entry) + (name_len))

struct dfs_super_block {
	__le32 magic;
	__le32 version;
	__le32 block_size;
	__le32 flags;
	__le64 created_ns;
	__le32 next_ino;
	__le32 reserved32;
	__le64 reserved[4];
};

struct dfs_disk_inode {
	__le32 ino;
	__le64 base;
	__le32 mode;
	__le32 nlink;
	__le64 size;
	__le32 uid;
	__le32 gid;
	__le32 flags;
	__le32 generation;
	__le64 atime_ns;
	__le64 ctime_ns;
	__le64 btime_ns;
	__le64 mtime_ns;
	__le64 reserved[54];
	__le32 reserved32;
} __packed;

struct dfs_dir_entry {
	__le32 ino;
	__le16 rec_len;
	u8 name_len;
	u8 file_type;
	char name[];
} __packed;

struct dfs_sb_info {
	struct super_block *sb;
	struct delayed_work commit_work;
	atomic_t commit_pending;
	u64 chunk_blocks;
	atomic_t next_ino;
	atomic_t super_dirty;
};

struct dfs_inode_info {
	u64 base;
	u64 chunk_bytes;
};

extern const struct iomap_ops dfs_iomap_ops;
extern const struct address_space_operations dfs_aops;

u64 dfs_chunk_bytes(struct super_block *sb);
loff_t dfs_inode_offset(struct super_block *sb, u32 ino);
int dfs_inode_disk_location(struct super_block *sb, u32 ino,
				 loff_t *offset, pgoff_t *index,
				 unsigned int *page_off);
int dfs_inode_base(struct super_block *sb, u32 ino, u64 *base,
			   u64 *chunk_bytes);
bool dfs_inode_base_valid(struct super_block *sb, u32 ino, u64 base,
			  u64 chunk_bytes);
int dfs_map_file_range(struct inode *inode, loff_t offset, loff_t length,
			 loff_t *start_out, loff_t *len_out, u64 *addr_out);

struct inode *dfs_get_inode(struct super_block *sb, const struct inode *dir,
			    umode_t mode, dev_t dev);
struct inode *dfs_iget(struct super_block *sb, u32 ino, umode_t mode);
int dfs_make_empty(struct inode *inode, struct inode *parent);
int dfs_add_entry(struct inode *dir, const struct qstr *name, struct inode *inode);
int dfs_delete_entry(struct inode *dir, const struct qstr *name);
int dfs_empty_dir(struct inode *inode);
int dfs_find_entry(struct inode *dir, const struct qstr *name,
			  loff_t *pos_out, u16 *rec_len_out,
			  u32 *ino_out, u8 *type_out);
int dfs_readdir(struct file *file, struct dir_context *ctx);
bool dfs_root_dir_valid(struct inode *inode);
unsigned int dfs_dir_rec_len(unsigned int name_len);
void dfs_schedule_commit(struct super_block *sb);
int dfs_alloc_inode_info(struct inode *inode);
void dfs_free_inode(struct inode *inode);
int dfs_write_inode(struct inode *inode, struct writeback_control *wbc);
int dfs_fsync(struct file *file, loff_t start, loff_t end, int datasync);
int dfs_read_inode_disk(struct super_block *sb, u32 ino,
			 struct dfs_disk_inode *out);
int dfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		 struct iattr *attr);

extern const struct file_operations dfs_file_operations;
extern const struct inode_operations dfs_file_inode_operations;
