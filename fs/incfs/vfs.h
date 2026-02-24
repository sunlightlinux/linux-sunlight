/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2018 Google LLC
 */

#ifndef _INCFS_VFS_H
#define _INCFS_VFS_H

extern const struct file_operations incfs_file_ops;
extern const struct inode_operations incfs_file_inode_ops;

void incfs_kill_sb(struct super_block *sb);
int incfs_init_fs_context(struct fs_context *fc);

extern const struct fs_parameter_spec incfs_param_specs[];

int incfs_link(struct dentry *what, struct dentry *where);
int incfs_unlink(struct dentry *dentry);

static inline struct mount_info *get_mount_info(struct super_block *sb)
{
	struct mount_info *result = sb->s_fs_info;

	WARN_ON(!result);
	return result;
}

static inline struct super_block *file_superblock(struct file *f)
{
	struct inode *inode = file_inode(f);

	return inode->i_sb;
}

#endif
