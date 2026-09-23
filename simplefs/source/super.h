#include "simple.h"

static inline struct simplefs_super_block *SIMPLEFS_SB(struct super_block *sb)
{
    return sb->s_fs_info;	//获取sb里对应具体文件系统的fs私有数据
}

static inline struct simplefs_inode *SIMPLEFS_INODE(struct inode *inode)
{
    return inode->i_private;
}
