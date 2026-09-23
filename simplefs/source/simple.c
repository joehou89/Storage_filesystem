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

void simplefs_sb_sync(struct super_block *vsb)
{
    struct buffer_head *bh;
    struct simplefs_super_block *sb = SIMPLEFS_SB(vsb);

    bh = sb_bread(vsb, SIMPLEFS_SUPERBLOCK_BLOCK_NUMBER);
    BUG_ON(!bh);

    bh->b_data = (char *)sb;
    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);
}

struct simplefs_inode *simplefs_inode_search(struct super_block *sb,
    	struct simplefs_inode *start,
    	struct simplefs_inode *search)
{
    uint64_t count = 0;
    while (start->inode_no != search->inode_no
    		&& count < SIMPLEFS_SB(sb)->inodes_count) {
    	count++;
    	start++;
    }

    if (start->inode_no == search->inode_no) {
    	return start;
    }

    return NULL;
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

static int simplefs_read_link(struct dentry *dentry, char __user *buffer, int buflen)
{
    int ret;
    struct inode *inode;
    struct simplefs_inode * sfs_inode;
    struct buffer_head * bh;
    
    __PRINT_FUNC_INFO();
    inode = dentry->d_inode;
    sfs_inode = inode->i_private;

    bh = sb_bread(inode->i_sb, sfs_inode->data_block_number);
    BUG_ON(!bh);
    
    ret = vfs_readlink(NULL, buffer, buflen, (char*)bh->b_data);

    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);
    
    return ret;	
}

static void *simplefs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
    struct inode * inode;
    struct simplefs_inode * sfs_inode;
    struct buffer_head * bh;
    
    inode = dentry->d_inode;
    sfs_inode = inode->i_private;
    	
    __PRINT_FUNC_INFO();
    bh = sb_bread(inode->i_sb, sfs_inode->data_block_number);
    BUG_ON(!bh);
    
    nd_set_link(nd, (char*)bh->b_data);

    return NULL;
}


void simplefs_inode_del(struct super_block *vsb, struct simplefs_inode *inode)
{
    struct simplefs_super_block *sb = SIMPLEFS_SB(vsb);
    struct buffer_head *bh;
    struct simplefs_inode *inode_iterator;
    
    if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
    	sfs_trace("Failed to acquire mutex lock\n");
    	return;
    }
    
    //获取一个bh
    bh = sb_bread(vsb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
    BUG_ON(!bh);

    inode_iterator = (struct simplefs_inode *)bh->b_data;

    //获取一把sb_lock锁
    if (mutex_lock_interruptible(&simplefs_sb_lock)) {
    	sfs_trace("Failed to acquire mutex lock\n");
    	goto out_mgmt;
    }	

    //查找一个sfs_inode
    //inode_iterator = simplefs_inode_search(vsb, (struct simplefs_inode *)bh->b_data, inode);
    //if(NULL == inode_iterator) {
    //	goto out_sb_lock;
    //}

    //inode_iterator += inode->inode_no;
    inode_iterator += sb->inodes_count;
    memset(inode_iterator, 0x0, sizeof(struct simplefs_inode));
    sb->inodes_count--;
    
    mark_buffer_dirty(bh);
    simplefs_sb_sync(vsb);
    brelse(bh);
    
    mutex_unlock(&simplefs_sb_lock);
    
out_mgmt:
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

    if (mutex_lock_interruptible(&simplefs_sb_lock)) {
    	sfs_trace("failed to acquire mutex lock\n");
    	ret = -EINTR;
    	goto l_out;
    }

    p_simple_sb = SIMPLEFS_SB(p_sb);
    printk("%s %d sb->free_blocks[%d] start\n", __FUNCTION__, __LINE__, (int)p_simple_sb->free_blocks);

    /* Loop until we find a free block. We start the loop from 3,
     * as all prior blocks will always be in use */
    for (i = 3; i < SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED; i++)
    {
        printk("simplefs_sb_get_a_freeblock circle i[%d]\n", i);
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
    printk("%s %d sb->free_blocks[%d] end\n", __FUNCTION__, __LINE__, (int)p_simple_sb->free_blocks);

    simplefs_sb_sync(p_sb);

    mutex_unlock(&simplefs_sb_lock);

l_out:
    return ret;
}

int simplefs_sb_put_a_freeblock(struct super_block *vsb, uint64_t * out)
{
    int ret = 0;
    struct simplefs_super_block *sb = SIMPLEFS_SB(vsb);
    
    if (mutex_lock_interruptible(&simplefs_sb_lock)) {
    	sfs_trace("Failed to acquire mutex lock\n");
    	ret = -EINTR;
    	goto end;
    }
    
    if (unlikely(*out >= SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED || *out < 3)) {
    	pr_info("Block number invalid!\n");
    	ret = -ENOSPC;
    	goto end;
    }
    
    sb->free_blocks |= 1 << *out;
    simplefs_sb_sync(vsb);
end:
    mutex_unlock(&simplefs_sb_lock);
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
    loff_t pos;
    struct inode *inode;
    struct super_block *sb;
    struct buffer_head *bh;
    struct simplefs_inode *sfs_inode;
    struct simplefs_dir_record *record;
    int i;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
    pos = ctx->pos;
#else
    pos = filp->f_pos;
#endif
    inode = filp->f_dentry->d_inode;
    sb = inode->i_sb;

    if (pos) {
    	/* FIXME: We use a hack of reading pos to figure if we have filled in all data.
    	 * We should probably fix this to work in a cursor based model and
    	 * use the tokens correctly to not fill too many data in each cursor based call */
    	return 0;
    }

    sfs_inode = SIMPLEFS_INODE(inode);

    if (unlikely(!S_ISDIR(sfs_inode->mode))) {
    	printk(KERN_ERR
    	       "inode [%llu][%lu] for fs object [%s] not a directory\n",
    	       sfs_inode->inode_no, inode->i_ino,
    	       filp->f_dentry->d_name.name);
    	return -ENOTDIR;
    }

    bh = sb_bread(sb, sfs_inode->data_block_number);
    BUG_ON(!bh);

    record = (struct simplefs_dir_record *)bh->b_data;
    for (i = 0; i < sfs_inode->dir_children_count; i++) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
    	dir_emit(ctx, record->filename, SIMPLEFS_FILENAME_MAXLEN,
    		record->inode_no, DT_UNKNOWN);
    	ctx->pos += sizeof(struct simplefs_dir_record);
#else
    	filldir(dirent, record->filename, SIMPLEFS_FILENAME_MAXLEN, pos,
    		record->inode_no, DT_UNKNOWN);
    	filp->f_pos += sizeof(struct simplefs_dir_record);
#endif
    	pos += sizeof(struct simplefs_dir_record);
    	record++;
    }
    brelse(bh);

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

ssize_t simplefs_read(struct file * filp, char __user * buf, size_t len,
    	      loff_t * ppos)
{
    /* After the commit dd37978c5 in the upstream linux kernel,
     * we can use just filp->f_inode instead of the
     * f->f_path.dentry->d_inode redirection */
    struct inode *inode = filp->f_inode;
    struct simplefs_inode *sfs_inode =
        SIMPLEFS_INODE(filp->f_path.dentry->d_inode);
    struct buffer_head *bh;
    char *buffer;
    int nbytes;
    
    pgoff_t index;
    struct address_space *mapping;
    struct page *page;
    char *kaddr = NULL;
    int ret;
    
    index = *ppos >> PAGE_CACHE_SHIFT;
    mapping = inode->i_mapping;	//获取inode内存地址映射指针

    if (*ppos >= sfs_inode->file_size) {
    	/* Read request with offset beyond the filesize */
    	return 0;
    }

    /*direct I/O operation*/
    if(filp->f_flags & O_DIRECT) {
    	pr_info("Direct IO start.\n");
    	bh = sb_bread(filp->f_path.dentry->d_inode->i_sb,
    					    sfs_inode->data_block_number);

    	if (!bh) {
    		printk(KERN_ERR "Reading the block number [%llu] failed.",
    		       sfs_inode->data_block_number);
    		return 0;
    	}

    	buffer = (char *)bh->b_data;
    	nbytes = min((size_t) sfs_inode->file_size, len);

    	if (copy_to_user(buf, buffer, nbytes)) {
    		brelse(bh);
    		printk(KERN_ERR
    		       "Error copying file contents to the userspace buffer\n");
    		return -EFAULT;
    	}

    	brelse(bh);
    }
    else { /*I/O operation by page cache*/
    	pr_info("Page cache read IO start.\n");
    	if (NULL == mapping) {
    		pr_err("inode->i_mapping is invalid, page cache io opt failed!\n");
    		ret = -EFAULT;       
    		goto out;
    	}
    	page = find_get_page(mapping, index);
    	if (unlikely(NULL == page)) {
    		page = page_cache_alloc_cold(mapping);
    		if(!page) {
    			pr_err("page cache alloc cold failed!");
    			ret = ENOMEM;
    			goto out;
    		}
    		ret = add_to_page_cache_lru(page, mapping, index, GFP_KERNEL);
    		if (ret) {
    			page_cache_release(page);
    			if (-EEXIST == ret) {
    				ret = 0;
    			}
    			goto out;
    		}
    		nbytes = mapping->a_ops->readpage(filp, page);
    	}
    	else {
    		nbytes = inode->i_size;
    	}
    	
    	kaddr = kmap(page);
    	kaddr[nbytes] = '\0';
    	kaddr += *ppos;
    	if(copy_to_user(buf, kaddr, nbytes)) {
    		pr_err("copy data to userspace failed!\n");
    		ret = -EFAULT;
    		goto out;
    	}
    	
    	kunmap(page);
    	unlock_page(page);
    	page_cache_release(page);	
    }

    *ppos += nbytes;
    ret = nbytes;
    file_accessed(filp); //why do this?
out:
    return ret;
}

/* Save the modified inode */
int simplefs_inode_save(struct super_block *sb, struct simplefs_inode *sfs_inode)
{
    struct simplefs_inode *inode_iterator;
    struct buffer_head *bh;

    bh = sb_bread(sb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
    BUG_ON(!bh);

    if (mutex_lock_interruptible(&simplefs_sb_lock)) {
    	sfs_trace("Failed to acquire mutex lock\n");
    	return -EINTR;
    }

    inode_iterator = simplefs_inode_search(sb,
    	(struct simplefs_inode *)bh->b_data,
    	sfs_inode);

    if (likely(inode_iterator)) {
    	memcpy(inode_iterator, sfs_inode, sizeof(*inode_iterator));
    	printk(KERN_INFO "The inode updated\n");

    	mark_buffer_dirty(bh);
    	sync_dirty_buffer(bh);
    } else {
    	mutex_unlock(&simplefs_sb_lock);
    	printk(KERN_ERR
    	       "The new filesize could not be stored to the inode.");
    	return -EIO;
    }

    brelse(bh);

    mutex_unlock(&simplefs_sb_lock);

    return 0;
}

/* FIXME: The write support is rudimentary. I have not figured out a way to do writes
 * from particular offsets (even though I have written some untested code for this below) efficiently. */
ssize_t simplefs_write(struct file * filp, const char __user * buf, size_t len,
    	       loff_t * ppos)
{
    /* After the commit dd37978c5 in the upstream linux kernel,
     * we can use just filp->f_inode instead of the
     * f->f_path.dentry->d_inode redirection */
    struct inode *inode;
    struct simplefs_inode *sfs_inode;
    struct buffer_head *bh;
    struct super_block *sb;
    char *buffer;
    int retval;

    struct address_space *mapping;
    char *kaddr = NULL;
    pgoff_t index;
    struct page *page;
    int ret;

    inode  = filp->f_inode;
    mapping = inode->i_mapping;
    index = (*ppos) >> PAGE_CACHE_SHIFT;

    retval = generic_write_checks(filp, ppos, &len, 0);
    if (retval) {
    	return retval;
    }

    sfs_inode = SIMPLEFS_INODE(inode);
    sb = inode->i_sb;

    /*Direct I/O*/
    if (filp->f_flags & O_DIRECT) {
    	pr_info("Direct IO start.\n");
    	inode = filp->f_path.dentry->d_inode;
    	bh = sb_bread(filp->f_path.dentry->d_inode->i_sb,
    					    sfs_inode->data_block_number);

    	if (!bh) {
    		printk(KERN_ERR "Reading the block number [%llu] failed.",
    		       sfs_inode->data_block_number);
    		return 0;
    	}
    	buffer = (char *)bh->b_data;

    	/* Move the pointer until the required byte offset */
    	buffer += *ppos;

    	if (copy_from_user(buffer, buf, len)) {
    		brelse(bh);
    		printk(KERN_ERR
    		       "Error copying file contents from the userspace buffer to the kernel space\n");
    		return -EFAULT;
    	}
    	//*ppos += len;

    	mark_buffer_dirty(bh);
    	sync_dirty_buffer(bh);
    	brelse(bh);
    }
    else { /*I/O operation by page cache*/		
    	pr_info("Page cache write IO start.\n");
    	if (NULL == mapping) {
    			pr_err("inode->i_mapping is invalid, page cache io opt failed!\n");
    			ret = -EFAULT;       
    			goto out;
    	}
    	page = grab_cache_page_write_begin(mapping, index, 0);
    	if(!page) {
    		pr_err("grab cache page write failed!");
    		ret = -ENOMEM;
    		goto out;
    	}
    	else {
    		pr_info("grab cache page write ok!");
    	}

    	kaddr = kmap(page);
    	kaddr += *ppos;
    	if(copy_from_user(kaddr, buf, len)) {
    		pr_err("copy data from userspace failed!");
    		ret = -EFAULT;
    		goto out;
    	}
    	kunmap(page);
    	
    	//wht do this ?
    	if (!PageUptodate(page))
    		SetPageUptodate(page);

    	set_page_dirty(page);

    	unlock_page(page);
    	page_cache_release(page);
    	
    }

    *ppos += len;
    inode->i_size = *ppos;
    /* Set new size
     * sfs_inode->file_size = max(sfs_inode->file_size, *ppos);
     *
     * FIXME: What to do if someone writes only some parts in between ?
     * The above code will also fail in case a file is overwritten with
     * a shorter buffer */
    if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
    	sfs_trace("Failed to acquire mutex lock\n");
    	return -EINTR;
    }

    sfs_inode->file_size = *ppos;
    retval = simplefs_inode_save(sb, sfs_inode);
    if (retval) {
    	len = retval;
    }
    
    mutex_unlock(&simplefs_inodes_mgmt_lock);

out:
    return len;
}

int simplefs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
    struct inode *inode = file->f_inode;
    struct super_block *sb = inode->i_sb;
    struct address_space *mapping = inode->i_mapping;
    struct simplefs_inode *sfs_inode = SIMPLEFS_INODE(inode);
    			   
    struct buffer_head *bh;
    struct page *page;
    pgoff_t index = start >> PAGE_CACHE_SHIFT;
    			   
    char *buffer;
    char *kaddr;
    			   
    printk("this is %s\n", __func__);
    		   
    page = find_get_page(mapping, index);
    if (page) {
    	printk("cache page for write get ok!\n");
    }
    else {
    	printk("no cache page for write!\n");
    	return 0;
    }
    		   
    kaddr = kmap(page);
    				   
    bh = sb_bread(sb, sfs_inode->data_block_number);
    	if (!bh) {
    	printk(KERN_ERR "Reading the block number [%llu] failed.", sfs_inode->data_block_number);
    	return 0;
    }
    buffer = (char *)bh->b_data;
    			   
    memcpy(buffer, kaddr, inode->i_size);
    			   
    kunmap(page);
    			   
    page_cache_release(page);
    				   
    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);
    				   
    return 0;
}

const struct file_operations simplefs_file_operations = {
    .read = simplefs_read,
    .write = simplefs_write,
    //.read  = simplefs_read_iter,
    //.write = simplefs_write_iter,
    .fsync = simplefs_fsync,
};

const struct file_operations simplefs_dir_operations = {
    .owner = THIS_MODULE,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
    .iterate = simplefs_iterate,
#else
    .readdir = simplefs_readdir,
#endif
};

int simplefs_get_block(struct inode *inode, sector_t iblock, struct buffer_head *bh_result, int create)
{
    struct super_block *sb = inode->i_sb;
    struct simplefs_inode *sfs_inode = SIMPLEFS_INODE(inode);

    if(create)
    	set_buffer_new(bh_result);
    map_bh(bh_result, sb, sfs_inode->data_block_number);
    return 0;
}

static int simplefs_readpage(struct file *file, struct page *page)
{
    return mpage_readpage(page, simplefs_get_block);	
}

static int simplefs_readpages(struct file *file, struct address_space *mapping, struct list_head *pages, unsigned nr_pages)
{
    return mpage_readpages(mapping, pages, nr_pages, simplefs_get_block);
}

static int simplefs_writepage(struct page *page, struct writeback_control *wbc)
{
    return block_write_full_page(page, simplefs_get_block, wbc);
}

static int simplefs_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
    return mpage_writepages(mapping, wbc, simplefs_get_block);
}

static int simplefs_write_begin(struct file *file, struct address_space *mapping,
    	loff_t pos, unsigned len, unsigned flags,
    	struct page **pagep, void **fsdata)
{
 	return block_write_begin(mapping, pos, len, flags, pagep, simplefs_get_block);
}

static int simplefs_write_end(struct file *file, struct address_space *mapping,
    		loff_t pos, unsigned len, unsigned copied,
    		struct page *page, void *fsdata)
{
    return generic_write_end(file, mapping, pos, len, copied, page, fsdata);
}

static ssize_t simplefs_direct_IO(int rw, struct kiocb *iocb, const struct iovec *iov, loff_t offset, unsigned long nr_segs)
{
    struct file *file = iocb->ki_filp;
    struct inode *inode = file->f_mapping->host;				
    return blockdev_direct_IO(rw, iocb, inode, iov,offset, nr_segs, simplefs_get_block);
}
    		
const struct address_space_operations simplefs_aops = {
    .readpage     = simplefs_readpage,
    .readpages    = simplefs_readpages,
    .writepage    = simplefs_writepage,
    .writepages   = simplefs_writepages,
    .write_begin  = simplefs_write_begin,
    .write_end    = simplefs_write_end,
    .direct_IO    = simplefs_direct_IO,
};

static int simplefs_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl);

struct dentry *simplefs_lookup(struct inode *parent_inode, struct dentry *child_dentry, unsigned int flags);

static int simplefs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode);

static int simplefs_rmdir(struct inode *p_parent_inode, struct dentry *p_dentry);

static int simplefs_unlink(struct inode *dir, struct dentry *dentry);

static int simplefs_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry);

static int simplefs_symlink(struct inode * dir, struct dentry * dentry, const char * symname);

// 对于simplefs inode实现的op语义操作函数
static struct inode_operations simplefs_inode_ops = {
    .create = simplefs_create,    // 创建普通文件
    .lookup = simplefs_lookup,    // 目录查找
    .mkdir = simplefs_mkdir,      // 创建目录
    .rmdir = simplefs_rmdir,      // 删除目录
    .unlink = simplefs_unlink,    // 删除文件
    .link = simplefs_link,        // 创建硬链接
    .symlink = simplefs_symlink,  // 创建符号链接
};

static struct inode_operations simplefs_symlink_inode_ops = {
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

void simplefs_inode_hardlink_cn_add(struct super_block *sb, struct simplefs_inode *sfs_inode)
{
    struct buffer_head *bh;
    struct simplefs_inode *inode_iterator;
    
    __PRINT_FUNC_INFO();

    if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
    	sfs_trace("Failed to acquire mutex lock\n");
    	return;
    }
    
    bh = sb_bread(sb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
    BUG_ON(!bh);
    	
    if (mutex_lock_interruptible(&simplefs_sb_lock)) {
    	sfs_trace("Failed to acquire mutex lock\n");
    	goto out_mgmt;
    }
    
    inode_iterator = simplefs_inode_search(sb, (struct simplefs_inode*)bh->b_data, sfs_inode);	
    if(NULL == inode_iterator)
    {
    	pr_info("simplefs_inode_search failed!!!");
    	goto out_bh;
    }

    inode_iterator->link_counter++;

out_bh:
    mark_buffer_dirty(bh);
    simplefs_sb_sync(sb);
    brelse(bh);
    mutex_unlock(&simplefs_sb_lock);
    
out_mgmt:
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

/*
* 函数说明:文件系统创建或打开一个文件时inode层面的具体创建操作
* 输入参数:struct inode *p_dir
           struct dentry *p_dentry
           umode_t mode
* 输出参数:无
* 返回值	  :0表示执行成功;<0表示执行失败
* 修改说明: 
      时间:2026/09/21
      作者:houchao
      说明:函数优化,增加注释信息
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
    p_sfs_inode = kmem_cache_alloc(sfs_inode_cachep, GFP_KERNEL);
    if (unlikely(NULL == p_sfs_inode))
    {
        iput(p_inode);
        ret = -ENOMEM;
        goto l_unlock;
    }
    p_sfs_inode->inode_no = p_inode->i_ino;
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
    struct inode *p_inode = d_inode(p_dentry);
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

    pr_info("func[%s],line[%d], sfs_inode[%p] link_counter[%llu]", __FUNCTION__, __LINE__, p_sinode, p_sinode->link_counter);

    // 7.添加硬链接处理逻辑
    if(p_sinode->link_counter >= 2)
    {
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
static int simplefs_hardlink_fs_object(struct dentry *old_dentry, struct inode *dir,
    		 struct dentry *dentry)
{
    int ret;
    struct super_block *sb = dir->i_sb; 
    struct inode *inode = d_inode(old_dentry);
    //通过old_dentry获取sfs_inode
    struct simplefs_inode *sfs_inode = SIMPLEFS_INODE(inode);	

    if (mutex_lock_interruptible(&simplefs_directory_children_update_lock)) {
    	sfs_trace("Failed to acquire mutex lock\n");
    	ret = -EINTR;
    	goto out;
    }	

    //在父dir中添加一个条目信息
    ret = simplefs_dir_add_entry_info(dir, sfs_inode, dentry->d_name.name);
    if(ret) {
    	pr_info("simplefs dir add inode failed!\n");
    	goto unlock;
    }

    //文件sfs_inode结构体中link_counter计数值+1
    simplefs_inode_hardlink_cn_add(sb, sfs_inode);

unlock:
    mutex_unlock(&simplefs_directory_children_update_lock);
out:
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
static int simplefs_symlink_fs_object(struct inode * p_parent_inode, struct dentry *p_dentry,
    const char * symname)
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
    	printk(KERN_ERR
    	       "Maximum number of objects supported by simplefs is already reached");
        mutex_unlock(&simplefs_directory_children_update_lock);
        return -ENOSPC;
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
    len = strlen(symname)+1;
    memcpy(p_buff, symname, len);
    
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
        return ret;
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
static int simplefs_mkdir(struct inode *p_parent_inode, struct dentry *p_dentry,
    		  umode_t mode)
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

static int simplefs_link(struct dentry *old_dentry, struct inode *dir,
    		 struct dentry *dentry)
{
    __PRINT_FUNC_INFO();
    return simplefs_hardlink_fs_object(old_dentry, dir, dentry);
}

static int simplefs_symlink(struct inode * dir, struct dentry * dentry,
    						  const char * symname)
{
    __PRINT_FUNC_INFO();
    return simplefs_symlink_fs_object(dir, dentry, symname);
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
static const struct super_operations simplefs_sops = {
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
struct file_system_type simplefs_fs_type = {
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


