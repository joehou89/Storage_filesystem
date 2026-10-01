/************************************************************************************* 
                      版权所有 (C), 2016-2026
************************************************************************************** 
文 件 名 : simple.c
版 本 号 : version 1.0
作     者  : houchao
生成日期 : 2026 年 9 月 21 日
功能描述 : simple 内核文件系统内部接口实现
修改历史 :
    1.日 期 : 2026 年 9 月 21 日
      作 者 :   houchao
   修改内容 : 创建文件
*************************************************************************************/

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/statfs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/version.h>
#include <linux/compiler.h>
#include <linux/namei.h>
#include <linux/fs.h>
#include <linux/mpage.h>
#include <linux/aio.h>
#include "super.h"
#include <linux/pagemap.h>


#define f_dentry f_path.dentry
/* A super block lock that must be used for any critical section operation on the sb,
 * such as: updating the free_blocks, inodes_count etc. */
static DEFINE_MUTEX(simplefs_sb_lock);
static DEFINE_MUTEX(simplefs_inodes_mgmt_lock);

/* FIXME: This can be moved to an in-memory structure of the simplefs_inode.
 * Because of the global nature of this lock, we cannot create
 * new children (without locking) in two different dirs at a time.
 * They will get sequentially created. If we move the lock
 * to a directory-specific way (by moving it inside inode), the
 * insertion of two children in two different directories can be
 * done in parallel */
static DEFINE_MUTEX(simplefs_directory_children_update_lock);

static struct kmem_cache *sfs_inode_cachep;

void simplefs_sb_sync(struct super_block *p_sb)
{
    struct buffer_head *p_bh           = NULL;
    struct simplefs_super_block *p_ssb = SIMPLEFS_SB(p_sb);

    p_bh = sb_bread(p_sb, SIMPLEFS_SUPERBLOCK_BLOCK_NUMBER);
    BUG_ON(!p_bh);

    p_bh->b_data = (char *)p_ssb;
    mark_buffer_dirty(p_bh);
    sync_dirty_buffer(p_bh);
    brelse(p_bh);
}

struct simplefs_inode *simplefs_inode_search(struct super_block *p_sb,
    struct simplefs_inode *p_start, struct simplefs_inode *p_simple_inode)
{
    uint64_t count = 0;

    while (p_start->inode_no != p_simple_inode->inode_no && count < SIMPLEFS_SB(p_sb)->inodes_count)
    {
        count++;
        p_start++;
    }

    return (p_start->inode_no == p_simple_inode->inode_no ? p_start : NULL);
}

/*
* 函数说明:将新增的inode添加到inode子区域中
* 输入参数:struct super_block *p_sb
		   struct simplefs_inode *p_sinode
* 输出参数:无
* 返回值 	:无
* 修改说明: 
	时间:2026/09/22
	作者:houchao
	说明:函数优化,增加注释信息
*/
void simplefs_inode_add(struct super_block *p_sb, struct simplefs_inode *p_sinode)
{
	struct simplefs_super_block *p_ssb  = SIMPLEFS_SB(p_sb);
	struct buffer_head *p_bh		    = NULL;
	struct simplefs_inode *p_tmp_sinode = NULL;

	if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock))
	{
		sfs_trace("failed to acquire mutex lock\n");
		goto l_out;
	}

	p_bh = sb_bread(p_sb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
	BUG_ON(!p_bh);

	p_tmp_sinode = (struct simplefs_inode *)p_bh->b_data;

	if (mutex_lock_interruptible(&simplefs_sb_lock))
	{
		sfs_trace("failed to acquire simplefs sb lock\n");
        mutex_unlock(&simplefs_inodes_mgmt_lock);
		goto l_out;
	}

    /* Append the new inode in the end in the inode store */
    p_tmp_sinode += p_ssb->inodes_count;
    //memcpy(p_tmp_sinode, p_sinode, sizeof(struct simplefs_inode));
    p_ssb->inodes_count++;
    printk("simplefs_inode_add: simplefs_super_block inodes count: %lld\n", p_ssb->inodes_count);

    // 持久化sb、buffer head内存信息
    simplefs_sb_sync(p_sb);
    mark_buffer_dirty(p_bh);
    sync_dirty_buffer(p_bh);
    brelse(p_bh);

    mutex_unlock(&simplefs_sb_lock);
    mutex_unlock(&simplefs_inodes_mgmt_lock);

l_out:
	return;
}

static int simplefs_read_link(struct dentry *p_dentry, char __user *p_buffer, int buflen)
{
    int ret                            = 0;
    struct inode *p_inode              = NULL;
    struct simplefs_inode *p_sfs_inode = NULL;
    struct buffer_head * p_bh          = NULL;
    
    __PRINT_FUNC_INFO();
    p_inode = p_dentry->d_inode;
    p_sfs_inode = p_inode->i_private;

    p_bh = sb_bread(p_inode->i_sb, p_sfs_inode->data_block_number);
    BUG_ON(!p_bh);
    
    ret = vfs_readlink(NULL, p_buffer, buflen, (char*)p_bh->b_data);

    mark_buffer_dirty(p_bh);
    sync_dirty_buffer(p_bh);
    brelse(p_bh);
    
    return ret;	
}

static void *simplefs_follow_link(struct dentry *p_dentry, struct nameidata *p_nd)
{
    struct inode *p_inode              = NULL;
    struct simplefs_inode *p_sfs_inode = NULL;
    struct buffer_head * p_bh          = NULL;
    
    p_inode = p_dentry->d_inode;
    p_sfs_inode = p_inode->i_private;
    	
    __PRINT_FUNC_INFO();
    p_bh = sb_bread(p_inode->i_sb, p_sfs_inode->data_block_number);
    BUG_ON(!p_bh);
    
    nd_set_link(p_nd, (char*)p_bh->b_data);

    return NULL;
}

void simplefs_inode_del(struct super_block *p_sb, struct simplefs_inode *inode)
{
    struct simplefs_super_block *p_ssb = SIMPLEFS_SB(p_sb);
    struct simplefs_inode *p_sinode    = NULL;
    struct buffer_head *p_bh           = NULL;
    
    if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock))
    {
        sfs_trace("Failed to acquire mutex lock\n");
        goto l_out;
    }
    
    // 1.读取磁盘第1块内容到内存缓存区
    p_bh = sb_bread(p_sb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
    BUG_ON(!p_bh);

    // 2.将磁盘内容强转成对应的simplefs inode内容
    p_sinode = (struct simplefs_inode *)p_bh->b_data;

    // 3.获取一把sb_lock锁
    if (mutex_lock_interruptible(&simplefs_sb_lock))
    {
        sfs_trace("failed to acquire simplefs sb lock\n");
        goto l_fail;
    }	

    //查找一个sfs_inode
    //inode_iterator = simplefs_inode_search(vsb, (struct simplefs_inode *)bh->b_data, inode);
    //if(NULL == inode_iterator) {
    //	goto out_sb_lock;
    //}

    //inode_iterator += inode->inode_no;
    p_sinode += p_ssb->inodes_count;
    memset(p_sinode, 0x0, sizeof(struct simplefs_inode));
    p_ssb->inodes_count--;
    printk("simplefs_inode_del: simplefs_super_block inodes count: %lld\n", p_ssb->inodes_count);

    simplefs_sb_sync(p_sb);
    mark_buffer_dirty(p_bh);
    sync_dirty_buffer(p_bh);
    brelse(p_bh);

    mutex_unlock(&simplefs_sb_lock);
    mutex_unlock(&simplefs_inodes_mgmt_lock);

l_out:
    return;

l_fail:
    mutex_unlock(&simplefs_inodes_mgmt_lock);
}

/* This function returns a blocknumber which is free.
 * The block will be removed from the freeblock list.
 *
 * In an ideal, production-ready filesystem, we will not be dealing with blocks,
 * and instead we will be using extents
 *
 * If for some reason, the file creation/deletion failed, the block number
 * will still be marked as non-free. You need fsck to fix this.*/
/*
* 函数说明:给inode分配数据块
* 输入参数:struct super_block *p_sb
* 输出参数:uint64_t *p_block_number,表示数据块序号
* 返回值   :0表示执行成功;<0表示执行失败
* 修改说明: 
    时间:2026/09/22
    作者:houchao
    说明:函数优化,增加注释信息
         该文件系统一共能用64个数据块,同时,0,1,2这三个数据块不能给普通用户使用
*/
int simplefs_sb_get_a_freeblock(struct super_block *p_sb, uint64_t *p_block_number)
{
    struct simplefs_super_block *p_simple_sb = NULL;
    int i                                    = 0;
    int ret                                  = 0;

    if (mutex_lock_interruptible(&simplefs_sb_lock))
    {
        sfs_trace("failed to acquire mutex lock\n");
        ret = -EINTR;
        goto l_out;
    }

    p_simple_sb = SIMPLEFS_SB(p_sb);
    printk("%s %d sb->free_blocks start, %d, %d\n", __FUNCTION__, __LINE__, (int)p_simple_sb->free_blocks,
        simplefs_cal_free_blks(p_simple_sb->free_blocks));

    /* Loop until we find a free block. We start the loop from 3,
     * as all prior blocks will always be in use */
    for (i = 3; i < SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED; i++)
    {
        //printk("simplefs_sb_get_a_freeblock circle i[%d]\n", i);
        if (p_simple_sb->free_blocks & (1UL << i))
        {
            break;
        }
    }

    printk("simplefs_sb_get_a_freeblock i[%d]\n", i);
    if (unlikely(i == SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED))
    {
        printk(KERN_ERR "no more free blocks available");
        mutex_unlock(&simplefs_sb_lock);
        ret = -ENOSPC;
        goto l_out;
    }

    *p_block_number = i;
    printk("simplefs_sb_get_a_freeblock i[%d],out[%d]\n", i,(int)*p_block_number);

    /* Remove the identified block from the free list */
    p_simple_sb->free_blocks &= ~(1UL << i);
    printk("%s %d sb->free_blocks end, %d, %d\n", __FUNCTION__, __LINE__, (int)p_simple_sb->free_blocks,
        simplefs_cal_free_blks(p_simple_sb->free_blocks));

    simplefs_sb_sync(p_sb);

    mutex_unlock(&simplefs_sb_lock);

l_out:
    return ret;
}

int simplefs_sb_put_a_freeblock(struct super_block *p_sb, uint64_t *p_out)
{
    int ret                         = 0;
    struct simplefs_super_block *sb = SIMPLEFS_SB(p_sb);
    
    if (mutex_lock_interruptible(&simplefs_sb_lock))
    {
        sfs_trace("Failed to acquire mutex lock\n");
        ret = -EINTR;
        goto l_out;
    }
    
    if (unlikely(*p_out >= SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED || *p_out < 3))
    {
        pr_info("Block number invalid!\n");
        ret = -ENOSPC;
        goto l_fail;
    }

    sb->free_blocks |= 1 << *p_out;
    simplefs_sb_sync(p_sb);

l_fail:
    mutex_unlock(&simplefs_sb_lock);

l_out:
    return ret;
}

/*
* 函数说明:读取文件系统创建的对象数count
* 输入参数:struct super_block *p_sb
* 输出参数:uint64_t *p_count
* 返回值	  :0表示执行成功;<0表示执行失败
* 修改说明: 
      时间:2026/09/22
      作者:houchao
      说明:函数优化,增加注释信息
*/
static int simplefs_sb_get_objects_count(struct super_block *p_sb, uint64_t *p_count)
{
    int ret                         = 0;
    struct simplefs_super_block *sb = SIMPLEFS_SB(p_sb);

    if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock))
    {
        sfs_trace("failed to acquire mutex lock\n");
        ret = -EINTR;
        goto l_out;
    }

    *p_count = sb->inodes_count;
    mutex_unlock(&simplefs_inodes_mgmt_lock);

    printk("%s %d sb->inodes_count[%llu]\n", __FUNCTION__, __LINE__, sb->inodes_count);

l_out:
    return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
static int simplefs_iterate(struct file *filp, struct dir_context *ctx)
#else
static int simplefs_readdir(struct file *filp, void *dirent, filldir_t filldir)
#endif
{
    loff_t pos                           = 0;
    struct inode *p_inode                = NULL;
    struct super_block *p_sb             = NULL;
    struct buffer_head *p_bh             = NULL;
    struct simplefs_inode *p_sfs_inode   = NULL;
    struct simplefs_dir_record *p_record = NULL;
    int i                                = 0;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
    pos = ctx->pos;
#else
    pos = filp->f_pos;
#endif
    p_inode = filp->f_dentry->d_inode;
    p_sb = p_inode->i_sb;

    if (pos)
    {
    	/* FIXME: We use a hack of reading pos to figure if we have filled in all data.
    	 * We should probably fix this to work in a cursor based model and
    	 * use the tokens correctly to not fill too many data in each cursor based call */
        return 0;
    }

    p_sfs_inode = SIMPLEFS_INODE(p_inode);

    if (unlikely(!S_ISDIR(p_sfs_inode->mode)))
    {
    	printk(KERN_ERR
    	       "inode [%llu][%lu] for fs object [%s] not a directory\n",
    	       p_sfs_inode->inode_no, p_inode->i_ino,
    	       filp->f_dentry->d_name.name);
    	return -ENOTDIR;
    }

    p_bh = sb_bread(p_sb, p_sfs_inode->data_block_number);
    BUG_ON(!p_bh);

    p_record = (struct simplefs_dir_record *)p_bh->b_data;
    for (i = 0; i < p_sfs_inode->dir_children_count; i++)
    {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
        dir_emit(ctx, p_record->filename, SIMPLEFS_FILENAME_MAXLEN, p_record->inode_no, DT_UNKNOWN);
        ctx->pos += sizeof(struct simplefs_dir_record);
#else
        filldir(dirent, p_record->filename, SIMPLEFS_FILENAME_MAXLEN, pos, p_record->inode_no, DT_UNKNOWN);
        filp->f_pos += sizeof(struct simplefs_dir_record);
#endif
        pos += sizeof(struct simplefs_dir_record);
        p_record++;
    }
    brelse(p_bh);

    return 0;
}

/* This functions returns a simplefs_inode with the given inode_no
 * from the inode store, if it exists. */
/*
* 函数说明:获取对应文件的inode
* 输入参数:struct super_block *p_sb,表示超级块指针
           uint64_t inode_no, inode序号
* 输出参数:无
* 返回值     :struct simplefs_inode *
* 修改说明: 时间:2026/09/21
            作者:houchao
            说明:函数优化,增加注释信息
*/
struct simplefs_inode *simplefs_get_inode(struct super_block *p_sb, uint64_t inode_no)
{
    struct simplefs_super_block *p_sfs_sb = SIMPLEFS_SB(p_sb);
    struct simplefs_inode *p_sfs_inode    = NULL;
    struct simplefs_inode *p_tmp_inode    = NULL;
    struct buffer_head *p_bh              = NULL;
    int i                                 = 0;

    /* The inode store can be read once and kept in memory permanently while mounting.
     * But such a model will not be scalable in a filesystem with
     * millions or billions of files (inodes) */
    p_bh = sb_bread(p_sb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
    BUG_ON(!p_bh);
    p_sfs_inode = (struct simplefs_inode *)p_bh->b_data;

#if 0
    if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
    	printk(KERN_ERR "Failed to acquire mutex lock %s +%d\n",
    	       __FILE__, __LINE__);
    	return NULL;
    }
#endif

    for (i = 0; i < p_sfs_sb->inodes_count; i++)
    {
    	if (p_sfs_inode->inode_no == inode_no)
        {
    		p_tmp_inode = kmem_cache_alloc(sfs_inode_cachep, GFP_KERNEL);
    		memcpy(p_tmp_inode, p_sfs_inode, sizeof(*p_tmp_inode));
    		break;
    	}
    	p_sfs_inode++;
    }

    brelse(p_bh);

    return p_tmp_inode;
}

ssize_t simplefs_read(struct file *p_filp, char __user *p_buf, size_t len, loff_t *p_pos)
{
    /* After the commit dd37978c5 in the upstream linux kernel,
     * we can use just filp->f_inode instead of the
     * f->f_path.dentry->d_inode redirection */
    struct inode *p_inode              = p_filp->f_inode;
    struct simplefs_inode *p_sfs_inode = SIMPLEFS_INODE(p_filp->f_path.dentry->d_inode);
    struct buffer_head *p_bh           = NULL;
    char *p_buffer                     = NULL;
    struct address_space *p_mapping    = NULL;
    struct page *p_page                = NULL;
    char *p_kaddr                      = NULL;
    int nbytes                         = 0;
    pgoff_t index                      = 0;
    int ret                            = 0;

    index = *p_pos >> PAGE_CACHE_SHIFT;
    p_mapping = p_inode->i_mapping;	//获取inode内存地址映射指针

    if (*p_pos >= p_sfs_inode->file_size)
    {
        /* Read request with offset beyond the filesize */
        return 0;
    }

    /*direct I/O operation*/
    if(p_filp->f_flags & O_DIRECT)
    {
        pr_info("Direct IO start.\n");
        p_bh = sb_bread(p_filp->f_path.dentry->d_inode->i_sb, p_sfs_inode->data_block_number);

        if (unlikely(NULL == p_bh))
        {
            printk(KERN_ERR "Reading the block number [%llu] failed.", p_sfs_inode->data_block_number);
            return 0;
        }

        p_buffer = (char *)p_bh->b_data;
        nbytes = min((size_t) p_sfs_inode->file_size, len);

        if (copy_to_user(p_buf, p_buffer, nbytes))
        {
            brelse(p_bh);
            printk(KERN_ERR "Error copying file contents to the userspace buffer\n");
            return -EFAULT;
        }

        brelse(p_bh);
    }
    else
    {
        /*I/O operation by page cache*/
        pr_info("Page cache read IO start.\n");
        if (unlikely(NULL == p_mapping))
        {
            pr_err("inode->i_mapping is invalid, page cache io opt failed!\n");
            ret = -EFAULT;       
            goto l_out;
        }

        p_page = find_get_page(p_mapping, index);
        if (unlikely(NULL == p_page))
        {
            p_page = page_cache_alloc_cold(p_mapping);
            if(!p_page)
            {
                pr_err("page cache alloc cold failed!");
                ret = ENOMEM;
                goto l_out;
            }

            ret = add_to_page_cache_lru(p_page, p_mapping, index, GFP_KERNEL);
    		if (ret)
            {
                page_cache_release(p_page);
                if (-EEXIST == ret)
                {
                    ret = 0;
                }
                goto l_out;
            }
            nbytes = p_mapping->a_ops->readpage(p_filp, p_page);
        }
    	else
        {
            nbytes = p_inode->i_size;
        }

        p_kaddr = kmap(p_page);
        p_kaddr[nbytes] = '\0';
        p_kaddr += *p_pos;
        if(copy_to_user(p_buf, p_kaddr, nbytes))
        {
    		pr_err("copy data to userspace failed!\n");
    		ret = -EFAULT;
    		goto l_out;
        }

        kunmap(p_page);
        unlock_page(p_page);
        page_cache_release(p_page);
    }

    *p_pos += nbytes;
    ret = nbytes;
    file_accessed(p_filp); //why do this?

l_out:
    return ret;

}

/* Save the modified inode */
int simplefs_inode_save(struct super_block *p_sb, struct simplefs_inode *p_sfs_inode)
{
    struct simplefs_inode *p_sinode = NULL;
    struct buffer_head *p_bh        = NULL;
    int ret                         = 0;

    p_bh = sb_bread(p_sb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
    BUG_ON(!p_bh);

    if (mutex_lock_interruptible(&simplefs_sb_lock))
    {
        sfs_trace("Failed to acquire mutex lock\n");
        ret = -EINTR;
        goto l_out;
    }

    p_sinode = simplefs_inode_search(p_sb, (struct simplefs_inode *)p_bh->b_data, p_sfs_inode);
    if (likely(p_sinode))
    {
        memcpy(p_sinode, p_sfs_inode, sizeof(*p_sinode));
        printk(KERN_INFO "The inode updated\n");

        mark_buffer_dirty(p_bh);
        sync_dirty_buffer(p_bh);
    }
    else
    {
        mutex_unlock(&simplefs_sb_lock);
        printk(KERN_ERR "The new filesize could not be stored to the inode.");
        ret = -EIO;
        goto l_out;
    }

    brelse(p_bh);

    mutex_unlock(&simplefs_sb_lock);
    ret = 0;

l_out:
    return ret;

}

/* FIXME: The write support is rudimentary. I have not figured out a way to do writes
 * from particular offsets (even though I have written some untested code for this below) efficiently. */
ssize_t simplefs_write(struct file *p_filp, const char __user *p_buf, size_t len, loff_t *p_pos)
{
    /* After the commit dd37978c5 in the upstream linux kernel,
     * we can use just filp->f_inode instead of the
     * f->f_path.dentry->d_inode redirection */
    struct inode *p_inode              = NULL;
    struct simplefs_inode *p_sfs_inode = NULL;
    struct buffer_head *p_bh           = NULL;
    struct super_block *p_sb           = NULL;
    char *p_buffer                     = NULL;
    struct address_space *p_mapping    = NULL;
    char *p_kaddr                      = NULL;
    struct page *p_page                = NULL;
    pgoff_t index                      = 0;
    int retval                         = 0;
    int ret                            = 0;

    p_inode  = p_filp->f_inode;
    p_mapping = p_inode->i_mapping;
    index = (*p_pos) >> PAGE_CACHE_SHIFT;

    retval = generic_write_checks(p_filp, p_pos, &len, 0);
    if (retval)
    {
        return retval;
    }

    p_sfs_inode = SIMPLEFS_INODE(p_inode);
    p_sb = p_inode->i_sb;

    /*Direct I/O*/
    if (p_filp->f_flags & O_DIRECT)
    {
        pr_info("Direct IO start.\n");
        p_inode = p_filp->f_path.dentry->d_inode;
        p_bh = sb_bread(p_filp->f_path.dentry->d_inode->i_sb, p_sfs_inode->data_block_number);

    	if (unlikely(NULL == p_bh))
        {
    	    printk(KERN_ERR "Reading the block number [%llu] failed.", p_sfs_inode->data_block_number);
            return 0;
        }
        p_buffer = (char *)p_bh->b_data;

        /* Move the pointer until the required byte offset */
        p_buffer += *p_pos;

        if (copy_from_user(p_buffer, p_buf, len))
        {
            brelse(p_bh);
            printk(KERN_ERR "Error copying file contents from the userspace buffer to the kernel space\n");
            return -EFAULT;
        }
        //*ppos += len;

        mark_buffer_dirty(p_bh);
        sync_dirty_buffer(p_bh);
        brelse(p_bh);
    }
    else
    {
        /*I/O operation by page cache*/		
        pr_info("Page cache write IO start.\n");
        if (unlikely(NULL == p_mapping))
	    {
            pr_err("inode->i_mapping is invalid, page cache io opt failed!\n");
            ret = -EFAULT;       
            goto l_out;
        }
        p_page = grab_cache_page_write_begin(p_mapping, index, 0);
        if(unlikely(NULL == p_page))
        {
            pr_err("grab cache page write failed!");
            ret = -ENOMEM;
            goto l_out;
        }
        else
        {
            pr_info("grab cache page write ok!");
        }

        p_kaddr = kmap(p_page);
        p_kaddr += *p_pos;
        if(copy_from_user(p_kaddr, p_buf, len))
        {
            pr_err("copy data from userspace failed!");
            ret = -EFAULT;
            goto l_out;
        }
        kunmap(p_page);

        //wht do this ?
        if (!PageUptodate(p_page))
        {
            SetPageUptodate(p_page);
        }

        set_page_dirty(p_page);

        unlock_page(p_page);
        page_cache_release(p_page);
    }

    *p_pos += len;
    p_inode->i_size = *p_pos;
    /* Set new size
     * sfs_inode->file_size = max(sfs_inode->file_size, *ppos);
     *
     * FIXME: What to do if someone writes only some parts in between ?
     * The above code will also fail in case a file is overwritten with
     * a shorter buffer */
    if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock))
    {
        sfs_trace("Failed to acquire mutex lock\n");
        len = -EINTR;
        goto l_out;
    }

    p_sfs_inode->file_size = *p_pos;
    retval = simplefs_inode_save(p_sb, p_sfs_inode);
    if (retval)
    {
        len = retval;
    }
    
    mutex_unlock(&simplefs_inodes_mgmt_lock);

l_out:
    return len;
}

int simplefs_fsync(struct file *p_file, loff_t start, loff_t end, int datasync)
{
    struct inode *p_inode              = p_file->f_inode;
    struct super_block *p_sb           = p_inode->i_sb;
    struct address_space *p_mapping    = p_inode->i_mapping;
    struct simplefs_inode *p_sfs_inode = SIMPLEFS_INODE(p_inode);
    struct buffer_head *p_bh           = NULL;
    struct page *p_page                = NULL;
    pgoff_t index                      = start >> PAGE_CACHE_SHIFT;
    char *p_buffer                     = NULL;
    char *p_kaddr                      = NULL;

    printk("this is %s\n", __func__);
    		   
    p_page = find_get_page(p_mapping, index);
    if (p_page)
    {
        printk("cache page for write get ok!\n");
    }
    else
    {
        printk("no cache page for write!\n");
        return 0;
    }
    		   
    p_kaddr = kmap(p_page);
    p_bh = sb_bread(p_sb, p_sfs_inode->data_block_number);
    if (unlikely(NULL == p_bh))
    {
        printk(KERN_ERR "Reading the block number [%llu] failed.", p_sfs_inode->data_block_number);
        return 0;
    }

    p_buffer = (char *)p_bh->b_data;
    memcpy(p_buffer, p_kaddr, p_inode->i_size);
    kunmap(p_page);
    page_cache_release(p_page);

    mark_buffer_dirty(p_bh);
    sync_dirty_buffer(p_bh);
    brelse(p_bh);
    				   
    return 0;
}

const struct file_operations simplefs_file_operations =
{
    .read = simplefs_read,
    .write = simplefs_write,
    //.read  = simplefs_read_iter,
    //.write = simplefs_write_iter,
    .fsync = simplefs_fsync,
};

const struct file_operations simplefs_dir_operations =
{
    .owner = THIS_MODULE,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
    .iterate = simplefs_iterate,
#else
    .readdir = simplefs_readdir,
#endif
};

int simplefs_get_block(struct inode *p_inode, sector_t iblock, struct buffer_head *p_bh_result, int create)
{
    struct super_block *p_sb           = p_inode->i_sb;
    struct simplefs_inode *p_sfs_inode = SIMPLEFS_INODE(p_inode);

    if(create)
    {
        set_buffer_new(p_bh_result);
    }
    map_bh(p_bh_result, p_sb, p_sfs_inode->data_block_number);

    return 0;
}

static int simplefs_readpage(struct file *p_file, struct page *p_page)
{
    return mpage_readpage(p_page, simplefs_get_block);	
}

static int simplefs_readpages(struct file *p_file, struct address_space *p_mapping, struct list_head *p_pages, unsigned nr_pages)
{
    return mpage_readpages(p_mapping, p_pages, nr_pages, simplefs_get_block);
}

static int simplefs_writepage(struct page *p_page, struct writeback_control *p_wbc)
{
    return block_write_full_page(p_page, simplefs_get_block, p_wbc);
}

static int simplefs_writepages(struct address_space *p_mapping, struct writeback_control *p_wbc)
{
    return mpage_writepages(p_mapping, p_wbc, simplefs_get_block);
}

static int simplefs_write_begin(struct file *p_file, struct address_space *p_mapping,
    loff_t pos, unsigned len, unsigned flags, struct page **pp_pagep, void **pp_fsdata)
{
 	return block_write_begin(p_mapping, pos, len, flags, pp_pagep, simplefs_get_block);
}

static int simplefs_write_end(struct file *p_file, struct address_space *p_mapping,
    loff_t pos, unsigned len, unsigned copied, struct page *p_page, void *p_fsdata)
{
    return generic_write_end(p_file, p_mapping, pos, len, copied, p_page, p_fsdata);
}

static ssize_t simplefs_direct_IO(int rw, struct kiocb *p_iocb, const struct iovec *p_iov, loff_t offset, unsigned long nr_segs)
{
    struct file *file = p_iocb->ki_filp;
    struct inode *inode = file->f_mapping->host;				
    return blockdev_direct_IO(rw, p_iocb, inode, p_iov, offset, nr_segs, simplefs_get_block);
}
    		
const struct address_space_operations simplefs_aops =
{
    .readpage     = simplefs_readpage,
    .readpages    = simplefs_readpages,
    .writepage    = simplefs_writepage,
    .writepages   = simplefs_writepages,
    .write_begin  = simplefs_write_begin,
    .write_end    = simplefs_write_end,
    .direct_IO    = simplefs_direct_IO,
};

static int simplefs_create(struct inode *p_dir, struct dentry *p_dentry, umode_t mode, bool excl);

struct dentry *simplefs_lookup(struct inode *p_parent_inode, struct dentry *p_dentry, unsigned int flags);

static int simplefs_mkdir(struct inode *p_parent_inode, struct dentry *p_dentry, umode_t mode);

static int simplefs_rmdir(struct inode *p_parent_inode, struct dentry *p_dentry);

static int simplefs_unlink(struct inode *p_parent_inode, struct dentry *p_dentry);

static int simplefs_link(struct dentry *p_old_dentry, struct inode *p_dir, struct dentry *p_dentry);

static int simplefs_symlink(struct inode *p_dir, struct dentry *p_dentry, const char *p_symname);

// 对于simplefs inode实现的op语义操作函数
static struct inode_operations simplefs_inode_ops =
{
    .create = simplefs_create,    // 创建普通文件
    .lookup = simplefs_lookup,    // 目录查找
    .mkdir = simplefs_mkdir,      // 创建目录
    .rmdir = simplefs_rmdir,      // 删除目录
    .unlink = simplefs_unlink,    // 删除文件
    .link = simplefs_link,        // 创建硬链接
    .symlink = simplefs_symlink,  // 创建符号链接
};

static struct inode_operations simplefs_symlink_inode_ops =
{
    .readlink = simplefs_read_link,
    .follow_link = simplefs_follow_link,
};

/*
* 函数说明:在父目录中添加一条条目信息
* 输入参数:struct inode *p_parent_inode
		   struct simplefs_inode *p_sfs_inode
		   const char *filename
* 输出参数:无
* 返回值 	:0表示执行成功;<0表示执行失败
* 修改说明: 
	时间:2026/09/22
	作者:houchao
	说明:函数优化,增加注释信息
*/
static int simplefs_dir_add_entry_info(struct inode *p_parent_inode, struct simplefs_inode *p_sfs_inode,
    const char *filename)
{
    struct super_block *p_sb                           = NULL;
    struct buffer_head *p_bh                           = NULL;
    struct simplefs_inode *p_sinode                    = NULL;
    struct simplefs_dir_record *p_dir_record           = NULL;
    int ret = 0;

    __PRINT_FUNC_INFO();
    p_sb = p_parent_inode->i_sb;
    p_sinode = SIMPLEFS_INODE(p_parent_inode);
    p_bh = sb_bread(p_sb, p_sinode->data_block_number);
    BUG_ON(!p_bh);

    pr_info("FUNC[%s],LINE[%d],parent_dir_inode->data_block_number:%llu\n",__FUNCTION__,__LINE__,p_sinode->data_block_number);

    p_dir_record = (struct simplefs_dir_record *)p_bh->b_data;
    
    /* Navigate to the last record in the directory contents */
    p_dir_record += p_sinode->dir_children_count;
    
    p_dir_record->inode_no = p_sfs_inode->inode_no;
    strlcpy(p_dir_record->filename, filename, SIMPLEFS_FILENAME_MAXLEN);
    
    mark_buffer_dirty(p_bh);
    sync_dirty_buffer(p_bh);
    brelse(p_bh);

    if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock))
    {
        sfs_trace("failed to acquire inode mgmt lock\n");
        ret = -EINTR;
        goto l_out;
    }

    p_sinode->dir_children_count++;
    ret = simplefs_inode_save(p_sb, p_sinode);
    mutex_unlock(&simplefs_inodes_mgmt_lock);

l_out:
    return ret;
}

/*
* 函数说明:文件系统删除inode时对条目删除操作
* 输入参数:struct inode *p_parent_inode, 上层inode指针
		   struct simplefs_inode *p_sinode
		   const char *p_filename
* 输出参数:无
* 返回值   :0表示执行成功;<0表示执行失败
* 修改说明: 
	  时间:2026/09/22
	  作者:houchao
	  说明:函数优化,增加注释信息
*/
static int simplefs_dir_del_entry_info(struct inode *p_parent_inode, struct simplefs_inode *p_sinode,
    const char *p_filename)
{
    // 先获取对应的sb
    struct super_block *p_sb = p_parent_inode->i_sb;
    // 获取对应父目录dir的inode
    struct simplefs_inode *p_parent_sinode = SIMPLEFS_INODE(p_parent_inode);
    struct simplefs_dir_record *p_first    = NULL;
	struct simplefs_dir_record *p_last     = NULL;
    uint64_t dir_children_count            = 0;
    struct buffer_head *p_bh               = NULL;
    int ret                                = 0;
    int i                                  = 0;

    __PRINT_FUNC_INFO();

    // 通过sb_bread获取bh
    p_bh = sb_bread(p_sb, p_parent_sinode->data_block_number);
    BUG_ON(!p_bh);
    
    // 获取第一个record记录条目信息
    p_first = (struct simplefs_dir_record *)p_bh->b_data;

    // 获取父目录对应inode的子条目个数
    dir_children_count = p_parent_sinode->dir_children_count;

    // 通过子条目个数        inode_no，便利record记录信息
    for (i = 0; i < dir_children_count; p_first++, i++)
    {
        // 解决删除硬链接文件要多删除一次问题 20190906
    	if ((p_first->inode_no == p_sinode->inode_no) && (0 == strcmp(p_first->filename, p_filename)))
        {
            break;
        }
    }

    if(i == dir_children_count)
    {
        ret = -ENOENT;
        goto l_out;
    }
    
    // 通过一个last指针操作记录信息,每次都是用最后一个simplefs inode的dir record信息拷贝到要删除的那个
    // 这种写法是否存在内存泄漏? 是在目录文件inode block的区域上新增内容,理论上不会存在泄露情况
    p_last = (struct simplefs_dir_record *)p_bh->b_data;
    p_last += dir_children_count - 1;
    memcpy(p_first,p_last,sizeof(struct simplefs_dir_record));

    // 持久化
    mark_buffer_dirty(p_bh);
    sync_dirty_buffer(p_bh);
    brelse(p_bh);
    
    if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
    	sfs_trace("Failed to acquire mutex lock\n");
        ret = -EINTR;
    	goto l_out;
    }

    // 父目录条目个数减1
    p_parent_sinode->dir_children_count--;

    // 保存bh信息
    ret = simplefs_inode_save(p_sb, p_parent_sinode);
    
    mutex_unlock(&simplefs_inodes_mgmt_lock);

l_out:
    return ret;
}

void simplefs_inode_hardlink_cn_add(struct super_block *p_sb, struct simplefs_inode *p_sfs_inode)
{
    struct buffer_head *p_bh                = NULL;
    struct simplefs_inode *p_inode_iterator = NULL;
    
    __PRINT_FUNC_INFO();

    if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock))
    {
    	sfs_trace("Failed to acquire mutex lock\n");
    	return;
    }
    
    p_bh = sb_bread(p_sb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
    BUG_ON(!p_bh);
    	
    if (mutex_lock_interruptible(&simplefs_sb_lock))
    {
        sfs_trace("Failed to acquire mutex lock\n");
        goto l_out_mgmt;
    }
    
    p_inode_iterator = simplefs_inode_search(p_sb, (struct simplefs_inode*)p_bh->b_data, p_sfs_inode);	
    if(unlikely(NULL == p_inode_iterator))
    {
    	pr_info("simplefs_inode_search failed!!!");
    	goto l_out_bh;
    }

    p_inode_iterator->link_counter++;

l_out_bh:
    mark_buffer_dirty(p_bh);
    simplefs_sb_sync(p_sb);
    brelse(p_bh);
    mutex_unlock(&simplefs_sb_lock);
    
l_out_mgmt:
    mutex_unlock(&simplefs_inodes_mgmt_lock);
    
}

/*
* 函数说明: 针对新创建的inode赋值"文件"操作ops指针
* 输入参数:struct inode *p_inode
           struct simplefs_inode *p_sfs_inode
           umode_t mode
* 输出参数:无
* 返回值	  :无
* 修改说明: 
      时间:2026/09/22
      作者:houchao
      说明:函数拆封
*/
static void simplefs_file_ops(struct inode *p_inode, struct simplefs_inode *p_sfs_inode, umode_t mode)
{
    // 1.入参检查
    BUG_ON(NULL == p_inode || NULL == p_sfs_inode);

    // 2.通过mode执行判断处理
    switch (mode & S_IFMT)
    {
        case S_IFDIR:  // 针对目录文件填充file操作指针
        {
            printk(KERN_INFO "new directory creation request\n");
            p_sfs_inode->dir_children_count = 0;  // 针对目录文件,子条目数为0
            p_inode->i_fop = &simplefs_dir_operations;
            break;
        }
        case S_IFREG:  // 针对普通文件填充file操作指针
        {
            printk(KERN_INFO "new file creation request\n");
            p_sfs_inode->file_size = 0;  // 针对普通文件, 文件大小为0
            p_inode->i_fop = &simplefs_file_operations;
            //用于封装I/O缓存读写的操作表，i_mapping是用于管理缓冲项和页I/O操作,相关操作表封装在a_ops里
            p_inode->i_mapping->a_ops = &simplefs_aops;
            break;
        }
        case S_IFLNK:
        {
            p_inode->i_op = &simplefs_symlink_inode_ops;
            p_inode->i_mapping->a_ops = &simplefs_aops;
            break;
        }
        default:
        {
            printk(KERN_ERR "Unknown inode type. Neither a directory nor a file");
            BUG_ON(true);
        }
    }

    return;
}

/*函数说明:文件系统创建或打开一个文件时inode层面的具体创建操作
* 输入参数:struct inode *p_dir
*           struct dentry *p_dentry
*           umode_t mode
* 输出参数:无
* 返回值	  :0表示执行成功;<0表示执行失败
* 修改说明: 
*     时间:2026/09/21
*     作者:houchao
*     说明:函数优化,增加注释信息
*/
static int simplefs_create_fs_object(struct inode *p_parent_inode, struct dentry *p_dentry, umode_t mode)
{
    struct inode *p_inode              = NULL;
    struct simplefs_inode *p_sfs_inode = NULL;
    struct super_block *p_sb           = NULL;
    uint64_t count                     = 0;
    int ret                            = 0;

    __PRINT_FUNC_INFO();

    // 0.加锁,目录子节点锁
    if (mutex_lock_interruptible(&simplefs_directory_children_update_lock))
    {
        sfs_trace("failed to acquire mutex lock\n");
        ret = -EINTR;
	    goto l_out;
    }

    p_sb = p_parent_inode->i_sb;
    // 1.获取文件系统支持的创建对象的个数
    ret = simplefs_sb_get_objects_count(p_sb, &count);
    if (unlikely(ret < 0))
    {
        goto l_unlock;
    }

    // 2.判断创建的对象数是否超过定义的上限64
    if (unlikely(count >= SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED))
    {
    	/* The above condition can be just == insted of the >= */
    	printk(KERN_ERR "maximum number of objects supported by simplefs is already reached");
        ret = -ENOSPC;
        goto l_unlock;
    }

    // 3.判断mode模式是否合法, 只支持创建目录或文件
    if (!S_ISDIR(mode) && !S_ISREG(mode))
    {
    	printk(KERN_ERR "creation request but for neither a file nor a directory");
        ret = -EINVAL;
        goto l_unlock;
    }

    // 4. 分配一个新的vfs层的inode
    // 注意: 分配内存尽可能放在后面,避免异常情况退出还要做释放处理,容易引入内存泄露等bug
    p_inode = new_inode(p_sb);
    if (unlikely(NULL == p_inode))
    {
        ret = -ENOMEM;
        goto l_unlock;
    }
    p_inode->i_sb = p_sb;
    p_inode->i_op = &simplefs_inode_ops;  // 目录或普通文件inode操作集
    p_inode->i_atime = p_inode->i_mtime = p_inode->i_ctime = CURRENT_TIME;
    p_inode->i_ino = (count + SIMPLEFS_START_INO - SIMPLEFS_RESERVED_INODES + 1);

    // 5.分配一个新的simplefs文件系统的私有sfs_inode
    p_sfs_inode = kmem_cache_zalloc(sfs_inode_cachep, GFP_KERNEL);
    if (unlikely(NULL == p_sfs_inode))
    {
        iput(p_inode);
        ret = -ENOMEM;
        goto l_unlock;
    }
    p_sfs_inode->inode_no = p_inode->i_ino;
    pr_info("__FUNCTION[%s],LINE[%d], inode_no: %lld\n",__FUNCTION__,__LINE__, p_sfs_inode->inode_no);
    p_inode->i_private = p_sfs_inode;  // vfs和simplefs的内存inode通过i_private绑定
    p_sfs_inode->mode = mode;

    // 6.填充file文件操作指针ops
    simplefs_file_ops(p_inode, p_sfs_inode, mode);

    // 7.分配一个空闲的数据块
    /* First get a free block and update the free map,
     * Then add inode to the inode store and update the sb inodes_count,
     * Then update the parent directory's inode with the new child.
     *
     * The above ordering helps us to maintain fs consistency
     * even in most crashes
     */
    ret = simplefs_sb_get_a_freeblock(p_sb, &p_sfs_inode->data_block_number);
    if (unlikely(ret < 0))
    {
        printk(KERN_ERR "simplefs could not get a freeblock");
        iput(p_inode);
        goto l_unlock;
    }

    // 8.把sfs_inode添加到sb中
    simplefs_inode_add(p_sb, p_sfs_inode);

    // 9. 在父目录中添加一条条目信息
    ret = simplefs_dir_add_entry_info(p_parent_inode, p_sfs_inode, p_dentry->d_name.name);
    if(unlikely(ret))
    {
        pr_info("simplefs dir add inode failed!\n");
        iput(p_inode);
        goto l_unlock;
    }
    mutex_unlock(&simplefs_directory_children_update_lock);

    // 10.设置属主
    inode_init_owner(p_inode, p_parent_inode, mode);

    // 11.将inode和dentry绑定起来
    d_add(p_dentry, p_inode);

l_out:
    return ret;

l_unlock:
    mutex_unlock(&simplefs_directory_children_update_lock);
    goto l_out;

}

/*
* 函数说明:文件系统删除inode层面的具体创建操作
* 输入参数:struct inode *p_parent_inode, 上层inode指针
           struct dentry *p_dentry, 目录结构体指针
* 输出参数:无
* 返回值	  :0表示执行成功;<0表示执行失败
* 修改说明: 
      时间:2026/09/22
      作者:houchao
      说明:函数优化,增加注释信息
*/
static int simplefs_delete_fs_object(struct inode *p_parent_inode, struct dentry *p_dentry)
{
    struct inode *p_inode                  = d_inode(p_dentry);
    struct simplefs_inode *p_parent_sinode = NULL;
	struct simplefs_inode *p_sinode        = NULL;
    struct super_block *p_sb               = NULL;
    int ret                                = 0;

    __PRINT_FUNC_INFO();

    // 1.获取目录子条目锁
    if (mutex_lock_interruptible(&simplefs_directory_children_update_lock))
    {
        sfs_trace("failed to acquire dir child lock\n");
        ret = -EINTR;
        goto l_out;
    }

    // 2.获取父目录对应的simplefs inode
    p_parent_sinode = SIMPLEFS_INODE(p_parent_inode);
    if(unlikely(NULL == p_parent_sinode))
    {
        ret = -EINTR;
        goto l_fail;
    }

    // 3.对父目录inode里的inode序号进行判断
    if(0 == p_parent_sinode->inode_no)
    {
        ret = -EINTR;
        goto l_fail;
    }

    // 4.获取dentry对应inode的sfs_inode
    p_sinode = SIMPLEFS_INODE(p_inode);
    if(unlikely(NULL == p_sinode))
    {
        ret = -EINTR;
        goto l_fail;
    }

    // 5.对sinode mode进行判断处理,如果是目录文件,且child count非0,说明此时还存在其他文件,不允许删除
    if(S_ISDIR(p_sinode->mode) && (p_sinode->dir_children_count != 0))
    {
        pr_info("dentry[%s] is dir, exist files or dirs\n",p_dentry->d_name.name);
        ret = -ENOTEMPTY;
        goto l_fail;
    }

    // 6.删除父目录中的条目信息
    ret = simplefs_dir_del_entry_info(p_parent_inode, p_sinode, p_dentry->d_name.name);
    if(unlikely(ret < 0))
    {
        goto l_fail;
    }

    pr_info("func[%s],line[%d], sfs_inode[%p] link_counter[%llu]\n", __FUNCTION__, __LINE__, p_sinode, p_sinode->link_counter);

    // 7.添加硬链接处理逻辑
    if(p_sinode->link_counter >= 2)
    {
        printk("simplefs_delete_fs_object: handle link ops, p_sinode->link_counter: %lld\n", p_sinode->link_counter);
        dput(p_dentry);
        goto l_fail;
    }

    // 8.释放inode内存空间
    p_sb = p_parent_inode->i_sb;
    simplefs_inode_del(p_sb, p_sinode);

    // 9.释放inode指向的data空间
    ret = simplefs_sb_put_a_freeblock(p_sb, &p_sinode->data_block_number);

    // 10.释放dentry
    dput(p_dentry);

l_fail:
    mutex_unlock(&simplefs_directory_children_update_lock);

l_out:
    return ret;

}

/*
* 函数说明:针对文件对象创建符号链接文件
* 输入参数:struct inode *p_parent_inode
*			struct dentry *p_dentry
*			const char * symname
* 输出参数:无
* 返回值	 :0表示执行成功;<0表示执行失败
* 修改说明: 
*	  时间:2026/09/23
*	  作者:houchao
*	  说明:函数优化,增加注释信息
*/
static int simplefs_hardlink_fs_object(struct dentry *p_old_dentry, struct inode *p_dir,
    struct dentry *p_dentry)
{
    int ret                          = 0;
    struct super_block *sb           = p_dir->i_sb; 
    struct inode *inode              = d_inode(p_old_dentry);
    struct simplefs_inode *sfs_inode = SIMPLEFS_INODE(inode);  //通过old_dentry获取sfs_inode

    if (mutex_lock_interruptible(&simplefs_directory_children_update_lock))
    {
        sfs_trace("Failed to acquire mutex lock\n");
        ret = -EINTR;
        goto l_out;
    }

    //在父dir中添加一个条目信息
    ret = simplefs_dir_add_entry_info(p_dir, sfs_inode, p_dentry->d_name.name);
    if(ret)
    {
        pr_info("simplefs dir add inode failed!\n");
        goto l_unlock;
    }

    //文件sfs_inode结构体中link_counter计数值+1
    simplefs_inode_hardlink_cn_add(sb, sfs_inode);

l_unlock:
    mutex_unlock(&simplefs_directory_children_update_lock);

l_out:
    return ret;	
}

/*
* 函数说明:针对文件对象创建符号链接文件
* 输入参数:struct inode *p_parent_inode
*			struct dentry *p_dentry
*			const char * symname
* 输出参数:无
* 返回值	 :0表示执行成功;<0表示执行失败
* 修改说明: 
*	  时间:2026/09/23
*	  作者:houchao
*	  说明:函数优化,增加注释信息
*/
static int simplefs_symlink_fs_object(struct inode *p_parent_inode, struct dentry *p_dentry,
    const char *p_symname)
{
    struct inode *p_inode              = NULL;
    struct simplefs_inode *p_sfs_inode = NULL;
    struct super_block *p_sb           = p_parent_inode->i_sb;
    umode_t mode                       = S_IFLNK | S_IRWXUGO;
    char *p_buff                       = NULL;
    struct buffer_head *p_bh           = NULL;
    int ret                            = 0;
    int len                            = 0;
    uint64_t count                     = 0;

    printk("simplefs_symlink_fs_object start.\n");

    // 1.获取互斥锁
    if (mutex_lock_interruptible(&simplefs_directory_children_update_lock))
    {
        sfs_trace("Failed to acquire mutex lock\n");
        ret = -EINTR;
        goto l_out;
    }

    // 2.获取sfs_inode->inode_counts
    ret = simplefs_sb_get_objects_count(p_sb, &count);
    if (unlikely(ret < 0))
    {
        mutex_unlock(&simplefs_directory_children_update_lock);
        goto l_out;
    }

    if (unlikely(count >= SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED))
    {
        /* The above condition can be just == insted of the >= */
    	printk(KERN_ERR "Maximum number of objects supported by simplefs is already reached");
        mutex_unlock(&simplefs_directory_children_update_lock);
        ret = -ENOSPC;
        goto l_out;
    }

    printk("simplefs_symlink_fs_object new inode.\n");
    p_inode = new_inode(p_sb);
    if(unlikely(NULL == p_inode))
    {
        mutex_unlock(&simplefs_directory_children_update_lock);
        ret = -ENOMEM;
        goto l_out;
    }

    printk("simplefs_symlink_fs_object get sfs inode.\n");
    p_sfs_inode = kmem_cache_alloc(sfs_inode_cachep, GFP_KERNEL);
    if(unlikely(NULL == p_sfs_inode))
    {
        mutex_unlock(&simplefs_directory_children_update_lock);
        ret = -ENOMEM;
        goto l_out;
    }
    p_sfs_inode->inode_no = p_inode->i_ino;
    p_sfs_inode->link_counter = 1;
    p_inode->i_private = p_sfs_inode;
    p_sfs_inode->mode = mode;
    p_inode->i_ino = p_sfs_inode->inode_no;

    if (S_ISDIR(p_sfs_inode->mode))
    {
        printk(KERN_INFO "New directory creation request\n");
        p_sfs_inode->dir_children_count = 0;
        p_inode->i_fop = &simplefs_dir_operations;
    }
    else if (S_ISREG(p_sfs_inode->mode))
    {
        printk(KERN_INFO "New file creation request\n");
        p_sfs_inode->file_size = 0;
        p_inode->i_fop = &simplefs_file_operations;
    }
    else if (S_ISLNK(p_sfs_inode->mode))
    {
        printk(KERN_INFO "New soft link creation request\n");
        p_sfs_inode->file_size = 0;
        p_inode->i_fop = &simplefs_file_operations;
        p_inode->i_op = &simplefs_symlink_inode_ops;
        p_inode->i_mapping->a_ops = &simplefs_aops;
        p_inode->i_mode = mode;
        p_inode->i_sb = p_sb;
        p_inode->i_atime = p_inode->i_mtime = p_inode->i_ctime = CURRENT_TIME;
    }

    /*申请一块data块区存放symlink路径*/
    printk("simplefs_symlink_fs_object get a free block.\n");
    ret = simplefs_sb_get_a_freeblock(p_sb, &p_sfs_inode->data_block_number);
    if (unlikely(ret < 0))
    {
        printk(KERN_ERR "simplefs could not get a freeblock\n");
        mutex_unlock(&simplefs_directory_children_update_lock);
        goto l_out;
    }

    p_bh = sb_bread(p_sb, p_sfs_inode->data_block_number);
    BUG_ON(!p_bh);
    p_buff = (char *)p_bh->b_data;
    len = strlen(p_symname)+1;
    memcpy(p_buff, p_symname, len);
    
    mark_buffer_dirty(p_bh);
    sync_dirty_buffer(p_bh);
    brelse(p_bh);

    p_sfs_inode->file_size = len;
    p_inode->i_size = len;

    /*把sfs_inode添加到sb中*/	
    printk("simplefs_symlink_fs_object add inode to sb.\n");
    simplefs_inode_add(p_sb, p_sfs_inode);

    /*在父dir中添加一个条目信息*/	
    printk("simplefs_symlink_fs_object add dir info to datablock.\n");
    ret = simplefs_dir_add_entry_info(p_parent_inode, p_sfs_inode, p_dentry->d_name.name);
    if(unlikely(ret))
    {
        pr_info("simplefs dir add inode failed!\n");
        mutex_unlock(&simplefs_directory_children_update_lock);
        goto l_out;
    }

    mutex_unlock(&simplefs_directory_children_update_lock);
    inode_init_owner(p_inode, p_parent_inode, mode);
    d_add(p_dentry, p_inode);
    
    printk("simplefs_symlink_fs_object end.\n");

l_out:
    return ret;

}

/*函数说明:文件系统创建目录文件
* 输入参数:struct inode *p_parent_inode
*           struct dentry *p_dentry
*           umode_t mode
* 输出参数:无
* 返回值	  :0表示执行成功;<0表示执行失败
* 修改说明: 
*     时间:2026/09/23
*     作者:houchao
*     说明:函数优化,增加注释信息
*/
static int simplefs_mkdir(struct inode *p_parent_inode, struct dentry *p_dentry, umode_t mode)
{
    __PRINT_FUNC_INFO();
    /* I believe this is a bug in the kernel, for some reason, the mkdir callback
     * does not get the S_IFDIR flag set. Even ext2 sets is explicitly */
    return simplefs_create_fs_object(p_parent_inode, p_dentry, S_IFDIR | mode);
}

/*函数说明:文件系统删除目录文件
* 输入参数:struct inode *p_parent_inode
*		   struct dentry *p_dentry
* 输出参数:无
* 返回值	:0表示执行成功;<0表示执行失败
* 修改说明: 
*   时间:2026/09/23
*   作者:houchao
*   说明:新增函数
*/
static int simplefs_rmdir(struct inode *p_parent_inode, struct dentry *p_dentry)
{
    __PRINT_FUNC_INFO();
    return simplefs_delete_fs_object(p_parent_inode, p_dentry);
}

/*
* 函数说明:文件系统创建普通文件
* 输入参数:struct inode *p_dir
*           struct dentry *p_dentry
*           umode_t mode
* 输出参数:无
* 返回值	  :0表示执行成功;<0表示执行失败
* 修改说明: 
*     时间:2026/09/21
*     作者:houchao
*     说明:函数优化,增加注释信息
*
*     时间:2026/09/23
*     作者:houchao
*     说明:修改注释信息
*/
static int simplefs_create(struct inode *p_dir, struct dentry *p_dentry, umode_t mode, bool excl)
{
    __PRINT_FUNC_INFO();
    return simplefs_create_fs_object(p_dir, p_dentry, mode);
}

/*函数说明:文件系统删除普通文件
* 输入参数:struct inode *p_parent_inode
*		   struct dentry *p_dentry
* 输出参数:无
* 返回值	:0表示执行成功;<0表示执行失败
* 修改说明: 
*   时间:2026/09/23
*   作者:houchao
*   说明:新增函数注释
*/
static int simplefs_unlink(struct inode *p_parent_inode, struct dentry *p_dentry)
{
    __PRINT_FUNC_INFO();
    return simplefs_delete_fs_object(p_parent_inode, p_dentry);
}

static int simplefs_link(struct dentry *p_old_dentry, struct inode *p_dir,
    		 struct dentry *p_dentry)
{
    __PRINT_FUNC_INFO();
    return simplefs_hardlink_fs_object(p_old_dentry, p_dir, p_dentry);
}

static int simplefs_symlink(struct inode *p_dir, struct dentry *p_dentry,
    						  const char *p_symname)
{
    __PRINT_FUNC_INFO();
    return simplefs_symlink_fs_object(p_dir, p_dentry, p_symname);
}

/*函数说明:遍历指定的目录
* 输入参数:struct inode *p_parent_inode
*     	   struct dentry *p_dentry
*      	   unsigned int flags
* 输出参数:无
* 返回值     :struct dentry *,返回dentry指针
* 修改说明: 
*       时间:2026/09/23
*       作者:houchao
*       说明:新增函数注释,代码优化,减少圈复杂度
*/
struct dentry *simplefs_lookup(struct inode *p_parent_inode, struct dentry *p_dentry, unsigned int flags)
{
    struct simplefs_inode *p_parent_sinode   = SIMPLEFS_INODE(p_parent_inode);
    struct simplefs_dir_record *p_dir_record = NULL;
    struct inode *p_inode                    = NULL;
    struct simplefs_inode *p_sfs_inode       = NULL;
    struct super_block *p_sb                 = p_parent_inode->i_sb;
    struct buffer_head *p_bh                 = NULL;
    int i                                    = 0;

    __PRINT_FUNC_INFO();
    // 1.首先通过父inode的指针找到对应文件系统的私有指针,根据sinode->data_block_number读取盘上对应数据块的内容到内存
    p_bh = sb_bread(p_sb, p_parent_sinode->data_block_number);
    BUG_ON(!p_bh);

    // 2.强转b_data内存为目录结构体指针, 进行遍历操作
    p_dir_record = (struct simplefs_dir_record *)p_bh->b_data;
    for (i = 0; i < p_parent_sinode->dir_children_count; i++)
    {
        if (0 == strcmp(p_dir_record->filename, p_dentry->d_name.name))
        {
            /* FIXME: There is a corner case where if an allocated inode,
             * is not written to the inode store, but the inodes_count is
             * incremented. Then if the random string on the disk matches
             * with the filename that we are comparing above, then we
             * will use an invalid uninitialized inode */

            p_sfs_inode = simplefs_get_inode(p_sb, p_dir_record->inode_no);
            p_inode = new_inode(p_sb);
            inode_init_owner(p_inode, p_parent_inode, p_sfs_inode->mode);
            // 填充inode file op操作函数
            simplefs_file_ops(p_inode, p_sfs_inode, p_inode->i_mode);
            p_inode->i_ino = p_dir_record->inode_no;
            p_inode->i_sb = p_sb;
            p_inode->i_op = &simplefs_inode_ops;
            /* FIXME: We should store these times to disk and retrieve them */
            p_inode->i_atime = p_inode->i_mtime = p_inode->i_ctime = CURRENT_TIME;
            p_inode->i_private = p_sfs_inode;
            d_add(p_dentry, p_inode);
            goto l_out;
        }
        p_dir_record++;
    }

    printk(KERN_ERR "no inode found for the filename [%s]\n", p_dentry->d_name.name);

l_out:
    return NULL;
}

/*
* 函数说明:删除文件系统sb操作
* 输入参数:struct inode *p_parent_inode
*          struct simplefs_inode *p_sfs_inode
*          const char *filename
* 输出参数:无
* 返回值 	:0表示执行成功;<0表示执行失败
* 修改说明: 
*    时间:2026/09/23
*    作者:houchao
*    说明:函数优化,增加注释信息
*/
void simplefs_destory_inode(struct inode *p_inode)
{
    struct simplefs_inode *sfs_inode = SIMPLEFS_INODE(p_inode);
    __PRINT_FUNC_INFO();
    printk(KERN_INFO "Freeing private data of inode %p (%lu)\n", sfs_inode, p_inode->i_ino);
    kmem_cache_free(sfs_inode_cachep, sfs_inode);
}

/*
* 函数说明:文件系统sb支持stat语义操作
* 输入参数:struct inode *p_parent_inode
*          struct simplefs_inode *p_sfs_inode
*          const char *filename
* 输出参数:无
* 返回值 	:0表示执行成功;<0表示执行失败
* 修改说明: 
*    时间:2026/09/23
*    作者:houchao
*    说明:函数优化,增加注释信息
*/
static int simplefs_statfs(struct dentry *p_dentry, struct kstatfs *p_buf)
{
    struct inode *p_inode                 = p_dentry->d_inode;
    struct super_block *p_sb              = p_inode->i_sb;
    struct simplefs_super_block *p_sfs_sb = SIMPLEFS_SB(p_sb);
	uint64_t total_bytes                  = 0;
    uint64_t total_blocks                 = 0;
    int ret                               = 0;
    uint64_t count                        = 0;

    // 1.获取底层块设备的总字节数
    if (likely(p_sb->s_bdev))
    {
        // 对于 CentOS 7 (3.10 内核)，使用 i_size_read
        total_bytes = i_size_read(p_sb->s_bdev->bd_inode);
        // 注：如果是 5.10 以上的新内核，推荐用 total_bytes = bdev_nr_bytes(sb->s_bdev);
    }
    else
    {
        // 如果是纯内存文件系统，则没有 s_bdev
        total_bytes = SIMPLEFS_MAX_BLOCKS; // 默认值(固定值)
    }

    // 2.加锁,目录子节点锁
    if (mutex_lock_interruptible(&simplefs_directory_children_update_lock))
    {
        sfs_trace("failed to acquire mutex lock\n");
        ret = -EINTR;
	    goto l_out;
    }

    // 3.获取当前已使用的块数量
    ret = simplefs_sb_get_objects_count(p_sb, &count);
    if (unlikely(ret < 0))
    {
        mutex_unlock(&simplefs_directory_children_update_lock);
        goto l_out;
    }
    mutex_unlock(&simplefs_directory_children_update_lock);

    // 4.设置文件系统魔数
    p_buf->f_type = SIMPLEFS_MAGIC;

    // 5.设置块大小
    p_buf->f_bsize  = p_sb->s_blocksize; 
    p_buf->f_frsize = p_sb->s_blocksize;
    
    // 6.设置总块数和空闲块数 (df 根据这几个值算容量)
    total_blocks = total_bytes >> p_sb->s_blocksize_bits;
    pr_info("FUNCTION[%s],LINE[%d], total_blocks:%lld, used_blocks:%lld\n",__FUNCTION__,__LINE__, total_blocks, count);
    p_buf->f_blocks = total_blocks;
    p_buf->f_bfree  = (count > total_blocks) ? 0 : total_blocks - count;
    p_buf->f_bavail = (count > total_blocks) ? 0 : total_blocks - count;

    // 7.设置 inode 相关信息 (df -i 会用到)
    p_buf->f_files  = SIMPLEFS_MAX_INODES;
    p_buf->f_ffree  = SIMPLEFS_MAX_INODES - p_sfs_sb->inodes_count;

    // 8.设置文件名最大长度
    p_buf->f_namelen = 255;

    // 9.其他可选设置
    p_buf->f_fsid.val[0] = 0; // 文件系统 ID，一般填 0
    p_buf->f_fsid.val[1] = 0;

l_out:
    return ret; // 必须返回 0 表示成功
}

/*
* 结构体说明: 文件系统sb元数据操作函数集合
*/
static const struct super_operations simplefs_sops =
{
    .destroy_inode = simplefs_destory_inode,
    .statfs = simplefs_statfs,
};

/* This function, as the name implies, Makes the super_block valid and
 * fills filesystem specific information in the super block */ 
/*
* 函数说明:文件系统填充sb的执行函数
* 输入参数:struct super_block *p_sb,表示超级块指针
           void *p_data,没有使用 
           int silent,没有使用
* 输出参数:无
* 返回值     :0表示执行成功;<0表示执行失败
* 修改说明: 时间:2026/09/21
            作者:houchao
            说明:函数优化,增加注释信息
*/
int simplefs_fill_super(struct super_block *p_sb, void *p_data, int silent)
{
    struct inode *p_root_inode             = NULL;
    struct buffer_head *p_bh               = NULL;
    struct simplefs_super_block *p_sb_disk = NULL;
    int ret                                = -EPERM;

    // 从磁盘上读取第0个块到内存buffer_head中
    p_bh = sb_bread(p_sb, SIMPLEFS_SUPERBLOCK_BLOCK_NUMBER);
    BUG_ON(!p_bh);

    // 强转类型获取对应simplefs文件系统的sb
    p_sb_disk = (struct simplefs_super_block *)p_bh->b_data;
    if (unlikely(NULL == p_sb_disk))
    {
        goto l_out;
    }
    printk(KERN_INFO "The magic number obtained in disk is: [%llu]\n", p_sb_disk->magic);

    if (unlikely(p_sb_disk->magic != SIMPLEFS_MAGIC))
    {
    	printk(KERN_ERR "The filesystem that you try to mount is not of type simplefs. Magicnumber mismatch.\n");
    	goto l_out;
    }

    if (unlikely(p_sb_disk->block_size != SIMPLEFS_DEFAULT_BLOCK_SIZE))
    {
    	printk(KERN_ERR "simplefs seem to be formatted using a non-standard block size.\n");
    	goto l_out;
    }

    printk(KERN_INFO
           "simplefs filesystem of version [%llu] formatted with a block size of [%llu] detected in the device.\n",
           p_sb_disk->version, p_sb_disk->block_size);

    // 给super block的字段进行赋值操作
    /* A magic number that uniquely identifies our filesystem type */
    p_sb->s_magic = SIMPLEFS_MAGIC;
    /* For all practical purposes, we will be using this s_fs_info as the super block */
    p_sb->s_fs_info = p_sb_disk;
    p_sb->s_maxbytes = SIMPLEFS_DEFAULT_BLOCK_SIZE;
    p_sb->s_op = &simplefs_sops;

    // 给vfs inode字段进行赋值操作
    p_root_inode = new_inode(p_sb);
    p_root_inode->i_ino = SIMPLEFS_ROOTDIR_INODE_NUMBER;
    inode_init_owner(p_root_inode, NULL, S_IFDIR);
    p_root_inode->i_sb = p_sb;
    p_root_inode->i_op = &simplefs_inode_ops;  // inode 操作op
    p_root_inode->i_fop = &simplefs_dir_operations;  // dentry 操作op
    p_root_inode->i_atime = p_root_inode->i_mtime = p_root_inode->i_ctime = CURRENT_TIME;
    p_root_inode->i_private = simplefs_get_inode(p_sb, SIMPLEFS_ROOTDIR_INODE_NUMBER);

    /* TODO: move such stuff into separate header. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 3, 0)
    p_sb->s_root = d_make_root(p_root_inode);
#else
    p_sb->s_root = d_alloc_root(p_root_inode);
    if (!p_sb->s_root)
    {
    	iput(p_root_inode);
    }
#endif

    if (!p_sb->s_root)
    {
        ret = -ENOMEM;
        goto l_out;
    }

    ret = 0;

l_out:
    brelse(p_bh);
    return ret;
}

/*
* 函数说明:文件系统mount入口函数
* 输入参数:struct file_system_type *p_fs_type,表示文件系统结构体
*          int flags, 挂载类型
*          const char *p_dev_name, 挂载目录
* 输出参数:void *p_data, 指向出参的指针
* 返回值     :struct dentry *,表示返回root根目录
* 修改说明: 时间:2026/09/21
*           作者:houchao
*           说明:函数优化,增加注释信息
*/
static struct dentry *simplefs_mount(struct file_system_type *p_fs_type, int flags, const char *p_dev_name,
    void *p_data)
{
    struct dentry *ret = NULL;
    __PRINT_FUNC_INFO();
    __PRINT_FUNC_MOUNT_BDEV_INFO(p_fs_type->name, flags, p_dev_name);

    ret = mount_bdev(p_fs_type, flags, p_dev_name, p_data, simplefs_fill_super);
    if (unlikely(NULL == ret))
    {
    	printk(KERN_ERR "error mounting simplefs\n");
    }

    return ret;
}

/*
* 函数说明:清理sb入口函数
* 输入参数:struct super_block *p_sb
* 输出参数:无
* 返回值 	:无
* 修改说明: 时间:2026/09/23
*			作者:houchao
*			说明:函数优化,增加注释信息
*/
static void simplefs_kill_superblock(struct super_block *p_sb)
{
    printk(KERN_INFO
           "simplefs superblock is destroyed. Unmount succesful.\n");
    /* This is just a dummy function as of now. As our filesystem gets matured,
     * we will do more meaningful operations here */

    kill_block_super(p_sb);

    return;
}

/*
* 注册内核文件系统类型结构体
*/
struct file_system_type simplefs_fs_type =
{
    .owner = THIS_MODULE,
    .name = "simplefs",
    .mount = simplefs_mount,
    .kill_sb = simplefs_kill_superblock,
    .fs_flags = FS_REQUIRES_DEV,
};

/*
* 函数说明:初始化内核文件系统注册接口
* 输入参数:无
* 输出参数:无
* 返回值     :0表示执行成功;<0表示执行失败
* 修改说明: 时间:2026/09/21
*           作者:houchao
*           说明:函数优化,增加注释信息
*
*/
static int simplefs_init(void)
{
    int ret = 0;

    sfs_inode_cachep = kmem_cache_create("sfs_inode_cache", sizeof(struct simplefs_inode), 0,
        SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD, NULL);
    if (unlikely(NULL == sfs_inode_cachep))
    {
        ret = -ENOMEM;
        goto l_out;
    }

    ret = register_filesystem(&simplefs_fs_type);
    if (unlikely(ret))
    {
    	printk(KERN_ERR "failed to register simplefs, error:%d\n", ret);
    }

l_out:
    return ret;

}

/*
* 函数说明:注销内核文件系统注册接口
* 输入参数:无
* 输出参数:无
* 返回值     :无
* 修改说明: 时间:2026/09/21
            作者:houchao
            说明:函数优化,增加注释信息
*
*/
static void simplefs_exit(void)
{
    int ret = 0;

    ret = unregister_filesystem(&simplefs_fs_type);
    if (unlikely(ret))
    {
    	printk(KERN_ERR "failed to unregister simplefs, error:%d\n", ret);
        goto l_out;
    }

    kmem_cache_destroy(sfs_inode_cachep);
    printk(KERN_INFO "sucessfully unregistered simplefs\n");

l_out:
    return;
}

module_init(simplefs_init);
module_exit(simplefs_exit);

MODULE_LICENSE("GPL");
//MODULE_LICENSE("CC0");
MODULE_AUTHOR("Sankar P");


