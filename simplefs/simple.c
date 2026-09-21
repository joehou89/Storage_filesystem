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

void simplefs_inode_add(struct super_block *vsb, struct simplefs_inode *inode)
{
    struct simplefs_super_block *sb = SIMPLEFS_SB(vsb);
    struct buffer_head *bh;
    struct simplefs_inode *inode_iterator;

    if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
    	sfs_trace("Failed to acquire mutex lock\n");
    	return;
    }

    bh = sb_bread(vsb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
    BUG_ON(!bh);

    inode_iterator = (struct simplefs_inode *)bh->b_data;

    if (mutex_lock_interruptible(&simplefs_sb_lock)) {
    	sfs_trace("Failed to acquire mutex lock\n");
    	goto out_mgmt;
    }

    /* Append the new inode in the end in the inode store */
    inode_iterator += sb->inodes_count;

    memcpy(inode_iterator, inode, sizeof(struct simplefs_inode));
    sb->inodes_count++;

    mark_buffer_dirty(bh);
    simplefs_sb_sync(vsb);
    brelse(bh);

    mutex_unlock(&simplefs_sb_lock);
out_mgmt:
    mutex_unlock(&simplefs_inodes_mgmt_lock);
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
int simplefs_sb_get_a_freeblock(struct super_block *vsb, uint64_t * out)
{
    struct simplefs_super_block *sb = SIMPLEFS_SB(vsb);
    int i;
    int ret = 0;

    if (mutex_lock_interruptible(&simplefs_sb_lock)) {
    	sfs_trace("Failed to acquire mutex lock\n");
    	ret = -EINTR;
    	goto end;
    }

    printk("%s %d sb->free_blocks[%d] start\n", __FUNCTION__, __LINE__, (int)sb->free_blocks);

    /* Loop until we find a free block. We start the loop from 3,
     * as all prior blocks will always be in use */
    for (i = 3; i < SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED; i++) {
    	printk("simplefs_sb_get_a_freeblock circle i[%d]\n", i);
    	if (sb->free_blocks & (1UL << i)) {
    		break;
    	}
    }

    if(sb->free_blocks & ~(1UL << 55))
    {
    	printk("simplefs_sb_get_a_freeblock i=55, and sb->free_blocks is 0 \n");
    }
    
    printk("simplefs_sb_get_a_freeblock i[%d]\n", i);
    if (unlikely(i == SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED)) {
    	printk(KERN_ERR "No more free blocks available");
    	ret = -ENOSPC;
    	goto end;
    }

    *out = i;
    printk("simplefs_sb_get_a_freeblock i[%d],out[%d]\n", i,(int)*out);

    /* Remove the identified block from the free list */
    sb->free_blocks &= ~(1UL << i);
    printk("%s %d sb->free_blocks[%d] end\n", __FUNCTION__, __LINE__, (int)sb->free_blocks);

    simplefs_sb_sync(vsb);

end:
    mutex_unlock(&simplefs_sb_lock);
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

static int simplefs_sb_get_objects_count(struct super_block *vsb,
    				 uint64_t * out)
{
    struct simplefs_super_block *sb = SIMPLEFS_SB(vsb);

    if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
    	sfs_trace("Failed to acquire mutex lock\n");
    	return -EINTR;
    }
    *out = sb->inodes_count;
    mutex_unlock(&simplefs_inodes_mgmt_lock);

    printk("%s %d sb->inodes_count[%llu]\n", __FUNCTION__, __LINE__, sb->inodes_count);

    return 0;
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

static int simplefs_unlink(struct inode *dir, struct dentry *dentry);

static int simplefs_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry);

static int simplefs_symlink(struct inode * dir, struct dentry * dentry, const char * symname);

// 对于simplefs inode实现的op语义操作函数
static struct inode_operations simplefs_inode_ops = {
    .create = simplefs_create,
    .lookup = simplefs_lookup,
    .mkdir = simplefs_mkdir,
    .unlink = simplefs_unlink,
    .link = simplefs_link,
    .symlink = simplefs_symlink,
};

static struct inode_operations simplefs_symlink_inode_ops = {
    .readlink = simplefs_read_link,
    .follow_link = simplefs_follow_link,
};

static int simplefs_dir_add_entry_info(struct inode *dir, struct simplefs_inode *sfs_inode, const char *filename)
{
    struct super_block *sb;
    struct simplefs_inode *parent_dir_inode;
    struct buffer_head *bh;
    struct simplefs_dir_record *dir_contents_datablock;
    int ret;
    __PRINT_FUNC_INFO();
    sb = dir->i_sb;
    parent_dir_inode = SIMPLEFS_INODE(dir);
    bh = sb_bread(sb, parent_dir_inode->data_block_number);
    BUG_ON(!bh);
    
    pr_info("FUNC[%s],LINE[%d],parent_dir_inode->data_block_number:%llu\n",__FUNCTION__,__LINE__,parent_dir_inode->data_block_number);
    
    dir_contents_datablock = (struct simplefs_dir_record *)bh->b_data;
    
    /* Navigate to the last record in the directory contents */
    dir_contents_datablock += parent_dir_inode->dir_children_count;
    
    dir_contents_datablock->inode_no = sfs_inode->inode_no;
    strlcpy(dir_contents_datablock->filename, filename, SIMPLEFS_FILENAME_MAXLEN);
    
    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);
    
    if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
    	mutex_unlock(&simplefs_directory_children_update_lock);
    	sfs_trace("Failed to acquire mutex lock\n");
    	return -EINTR;
    }
    
    parent_dir_inode->dir_children_count++;
    ret = simplefs_inode_save(sb, parent_dir_inode);	
    mutex_unlock(&simplefs_inodes_mgmt_lock);
    mutex_unlock(&simplefs_directory_children_update_lock);
    return ret;
}

static int simplefs_dir_del_entry_info(struct inode *dir, struct simplefs_inode *sfs_inode, const char *filename)
{
    //得先获取对应的sb
    struct super_block *sb = dir->i_sb;

    //获取对应父目录dir的inode
    struct simplefs_inode *parent_dir_inode = SIMPLEFS_INODE(dir);
    uint64_t dir_children_count;
    struct buffer_head *bh;
    struct simplefs_dir_record *first,*last;
    int ret,i;

    __PRINT_FUNC_INFO();
    
    //获取父目录对应inode的子条目个数
    dir_children_count = parent_dir_inode->dir_children_count;

    //通过sb_bread获取bh
    bh = sb_bread(sb, parent_dir_inode->data_block_number);
    BUG_ON(!bh);
    
    //获取第一个record记录条目信息
    first = (struct simplefs_dir_record *)bh->b_data;
    //pr_info("first111 ino:%llu, firstname:%s",first->inode_no,first->filename);

    //通过子条目个数        inode_no，便利record记录信息
    for(i=0; i<dir_children_count; first++,i++) 
    {
    	if((first->inode_no == sfs_inode->inode_no) && (0 == strcmp(first->filename,filename)))    //解决删除硬链接文件要多删除一次问题 20190906
    	{
    		break;
    	}
    }

    if(i == dir_children_count) {
    	return -ENOENT; 
    }
    
    //通过一个last指针操作记录信息
    last = (struct simplefs_dir_record *)bh->b_data;
    last += dir_children_count - 1;
    memcpy(first,last,sizeof(struct simplefs_dir_record));
    //pr_info("first222 ino:%llu, firstname:%s",first->inode_no,first->filename);
    
    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);
    
    if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
    	sfs_trace("Failed to acquire mutex lock\n");
    	return -EINTR;
    }

    //父目录条目个数减1
    parent_dir_inode->dir_children_count--;

    //保存bh信息
    ret = simplefs_inode_save(sb, parent_dir_inode);
    
    mutex_unlock(&simplefs_inodes_mgmt_lock);
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

static int simplefs_create_fs_object(struct inode *dir, struct dentry *dentry,
    			     umode_t mode)
{
    struct inode *inode;
    struct simplefs_inode *sfs_inode;
    struct super_block *sb;
    uint64_t count;
    int ret;

    __PRINT_FUNC_INFO();
    if (mutex_lock_interruptible(&simplefs_directory_children_update_lock)) {
    	sfs_trace("Failed to acquire mutex lock\n");
    	return -EINTR;
    }
    sb = dir->i_sb;

    ret = simplefs_sb_get_objects_count(sb, &count);
    if (ret < 0) {
    	mutex_unlock(&simplefs_directory_children_update_lock);
    	return ret;
    }

    if (unlikely(count >= SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED)) {
    	/* The above condition can be just == insted of the >= */
    	printk(KERN_ERR
    	       "Maximum number of objects supported by simplefs is already reached");
    	mutex_unlock(&simplefs_directory_children_update_lock);
    	return -ENOSPC;
    }

    if (!S_ISDIR(mode) && !S_ISREG(mode)) {
    	printk(KERN_ERR
    	       "Creation request but for neither a file nor a directory");
    	mutex_unlock(&simplefs_directory_children_update_lock);
    	return -EINVAL;
    }

    /*new一个新的inode，申请内存空间*/
    inode = new_inode(sb);
    if (!inode) {
    	mutex_unlock(&simplefs_directory_children_update_lock);
    	return -ENOMEM;
    }

    inode->i_sb = sb;
    inode->i_op = &simplefs_inode_ops;
    inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
    inode->i_ino = (count + SIMPLEFS_START_INO - SIMPLEFS_RESERVED_INODES + 1);
    
    /*new一个新的sfs_inode，申请内存空间*/
    sfs_inode = kmem_cache_alloc(sfs_inode_cachep, GFP_KERNEL);
    sfs_inode->inode_no = inode->i_ino;
    inode->i_private = sfs_inode;
    sfs_inode->mode = mode;

    if (S_ISDIR(mode)) {
    	printk(KERN_INFO "New directory creation request\n");
    	sfs_inode->dir_children_count = 0;
    	inode->i_fop = &simplefs_dir_operations;
    } else if (S_ISREG(mode)) {
    	printk(KERN_INFO "New file creation request\n");
    	sfs_inode->file_size = 0;
    	inode->i_fop = &simplefs_file_operations;
    	inode->i_mapping->a_ops = &simplefs_aops;	//用于封装I/O缓存读写的操作表，i_mapping是用于管理缓冲项和页I/O操作,相关操作表封装在a_ops里
    }

    /* First get a free block and update the free map,
     * Then add inode to the inode store and update the sb inodes_count,
     * Then update the parent directory's inode with the new child.
     *
     * The above ordering helps us to maintain fs consistency
     * even in most crashes
     */
    ret = simplefs_sb_get_a_freeblock(sb, &sfs_inode->data_block_number);
    if (ret < 0) {
    	printk(KERN_ERR "simplefs could not get a freeblock");
    	mutex_unlock(&simplefs_directory_children_update_lock);
    	return ret;
    }

    /*把sfs_inode添加到sb中*/
    simplefs_inode_add(sb, sfs_inode);

    /*在父dir中添加一个条目信息*/
    ret = simplefs_dir_add_entry_info(dir,sfs_inode,dentry->d_name.name);
    if(ret) {
    	pr_info("simplefs dir add inode failed!\n");
    	return ret;
    }
    
    inode_init_owner(inode, dir, mode);
    d_add(dentry, inode);

    return 0;
}

static int simplefs_delete_fs_object(struct inode *dir, struct dentry *dentry)
{
    struct inode *inode = d_inode(dentry);
    struct simplefs_inode *parent_dir_inode, *sfs_inode;
    struct super_block *sb; 
    //uint64_t block_number;
    int ret;

    __PRINT_FUNC_INFO();

    sb = dir->i_sb;	
    if (mutex_lock_interruptible(&simplefs_directory_children_update_lock)) {
    	sfs_trace("Failed to acquire mutex lock\n");
    	return -EINTR;
    }

    //获取父目录对应的inode
    parent_dir_inode = SIMPLEFS_INODE(dir);
    if(NULL == parent_dir_inode) {
    	ret = -EINTR;
    	goto out;
    }

    //对父目录inode里的子条目个数进行判断
    if(0 == parent_dir_inode->inode_no) {
    	ret = -EINTR;
    	goto out;
    }

    //获取dentry对应inode的sfs_inode
    sfs_inode = SIMPLEFS_INODE(inode);
    if(NULL == sfs_inode) {
    	ret = -EINTR;
    	goto out;
    }

    //对sfs_inode mode如果是目录 则执行返错
    if(S_ISDIR(sfs_inode->mode) && (sfs_inode->dir_children_count != 0)) {
    	pr_info("dentry[%s] is dir\n",dentry->d_name.name);
    	ret = -ENOTEMPTY;
    	goto out;
    }

    /*删除父目录中的条目信息*/
    ret = simplefs_dir_del_entry_info(dir, sfs_inode, dentry->d_name.name);
    if(ret < 0) {
    	goto out;
    }

    pr_info("func[%s],line[%d], sfs_inode[%p] link_counter[%llu]", __FUNCTION__, __LINE__, sfs_inode, sfs_inode->link_counter);
    //添加硬链接处理逻辑
    if(sfs_inode->link_counter >= 2) {
    	dput(dentry);
    	goto out;
    }
    
    /*释放inode内存空间*/
    simplefs_inode_del(sb, sfs_inode);

    /*释放inode指向的data空间*/
    ret = simplefs_sb_put_a_freeblock(sb,&sfs_inode->data_block_number);
    
    /*释放dentry*/
    dput(dentry);
    
out:
    mutex_unlock(&simplefs_directory_children_update_lock);
    return ret;

}

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

static int simplefs_symlink_fs_object(struct inode * dir, struct dentry * dentry,
    			 const char * symname)
{
    int ret;
    struct inode *inode = NULL;
    struct simplefs_inode *sfs_inode = NULL;
    struct super_block *sb = dir->i_sb;
    umode_t mode = S_IFLNK | S_IRWXUGO;
    char *buff = NULL;
    struct buffer_head *bh;
    int len = 0;
    uint64_t count;

    printk("simplefs_symlink_fs_object start.\n");

    //获取互斥锁
    if (mutex_lock_interruptible(&simplefs_directory_children_update_lock)) {
    	sfs_trace("Failed to acquire mutex lock\n");
    	return -EINTR;
    }

    //获取sfs_inode->inode_counts     具体有什么用？
    ret = simplefs_sb_get_objects_count(sb, &count);
    if (ret < 0) {
    	mutex_unlock(&simplefs_directory_children_update_lock);
    	return ret;
    }

    if (unlikely(count >= SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED)) {
    /* The above condition can be just == insted of the >= */
    	printk(KERN_ERR
    	       "Maximum number of objects supported by simplefs is already reached");
    	mutex_unlock(&simplefs_directory_children_update_lock);
    	return -ENOSPC;
    }
    
    printk("simplefs_symlink_fs_object new inode.\n");
    inode = new_inode(sb);
    if(!inode) {
    	mutex_unlock(&simplefs_directory_children_update_lock);
    	return -ENOMEM;
    }

    printk("simplefs_symlink_fs_object get sfs inode.\n");

    sfs_inode = kmem_cache_alloc(sfs_inode_cachep, GFP_KERNEL);
    if(NULL == sfs_inode) {
    	mutex_unlock(&simplefs_directory_children_update_lock);
    	return -ENOMEM;
    }

    sfs_inode->inode_no = inode->i_ino;
    sfs_inode->link_counter = 1;
    inode->i_private = sfs_inode;
    sfs_inode->mode = mode;
    inode->i_ino = sfs_inode->inode_no;

    if (S_ISDIR(sfs_inode->mode)) {
    	printk(KERN_INFO "New directory creation request\n");
    	sfs_inode->dir_children_count = 0;
    	inode->i_fop = &simplefs_dir_operations;
    } else if (S_ISREG(sfs_inode->mode)) {
    	printk(KERN_INFO "New file creation request\n");
    	sfs_inode->file_size = 0;
    	inode->i_fop = &simplefs_file_operations;
    } else if (S_ISLNK(sfs_inode->mode)) {
    	printk(KERN_INFO "New soft link creation request\n");
    	sfs_inode->file_size = 0;
    	inode->i_fop = &simplefs_file_operations;
    	inode->i_op = &simplefs_symlink_inode_ops;
    	inode->i_mapping->a_ops = &simplefs_aops;
    	inode->i_mode = mode;
    	inode->i_sb = sb;
    	inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
    }

    /*申请一块data块区存放symlink路径*/
    printk("simplefs_symlink_fs_object get a free block.\n");
    ret = simplefs_sb_get_a_freeblock(sb, &sfs_inode->data_block_number);
    if (ret < 0) {
    	printk(KERN_ERR "simplefs could not get a freeblock\n");
    	mutex_unlock(&simplefs_directory_children_update_lock);
    	return ret;
    }

    bh = sb_bread(sb, sfs_inode->data_block_number);
    BUG_ON(!bh);
    buff = (char *)bh->b_data;
    len = strlen(symname)+1;
    memcpy(buff, symname, len);
    
    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);

    sfs_inode->file_size = len;
    inode->i_size = len;

    /*把sfs_inode添加到sb中*/	
    printk("simplefs_symlink_fs_object add inode to sb.\n");
    simplefs_inode_add(sb, sfs_inode);

    /*在父dir中添加一个条目信息*/	
    printk("simplefs_symlink_fs_object add dir info to datablock.\n");
    ret = simplefs_dir_add_entry_info(dir,sfs_inode,dentry->d_name.name);
    if(ret) {
    	pr_info("simplefs dir add inode failed!\n");
    	return ret;
    }

    mutex_unlock(&simplefs_directory_children_update_lock);
    inode_init_owner(inode, dir, mode);
    d_add(dentry, inode);
    
    printk("simplefs_symlink_fs_object end.\n");

    return 0;
}
    		 	
static int simplefs_mkdir(struct inode *dir, struct dentry *dentry,
    		  umode_t mode)
{
    __PRINT_FUNC_INFO();
    /* I believe this is a bug in the kernel, for some reason, the mkdir callback
     * does not get the S_IFDIR flag set. Even ext2 sets is explicitly */
    return simplefs_create_fs_object(dir, dentry, S_IFDIR | mode);
}

/*
* 函数说明:文件系统创建或打开一个文件时inode层面的创建操作
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
static int simplefs_create(struct inode *p_dir, struct dentry *p_dentry, umode_t mode, bool excl)
{
    __PRINT_FUNC_INFO();
    return simplefs_create_fs_object(p_dir, p_dentry, mode);
}
    		   
static int simplefs_unlink(struct inode *dir, struct dentry *dentry)
{
    __PRINT_FUNC_INFO();
    return simplefs_delete_fs_object(dir, dentry);
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
    		 	
struct dentry *simplefs_lookup(struct inode *parent_inode,
    		       struct dentry *child_dentry, unsigned int flags)
{
    struct simplefs_inode *parent = SIMPLEFS_INODE(parent_inode);
    struct super_block *sb = parent_inode->i_sb;
    struct buffer_head *bh;
    struct simplefs_dir_record *record;
    int i;
    
    __PRINT_FUNC_INFO();
    bh = sb_bread(sb, parent->data_block_number);
    BUG_ON(!bh);

    record = (struct simplefs_dir_record *)bh->b_data;
    for (i = 0; i < parent->dir_children_count; i++) {
    	if (!strcmp(record->filename, child_dentry->d_name.name)) {
    		/* FIXME: There is a corner case where if an allocated inode,
    		 * is not written to the inode store, but the inodes_count is
    		 * incremented. Then if the random string on the disk matches
    		 * with the filename that we are comparing above, then we
    		 * will use an invalid uninitialized inode */

    		struct inode *inode;
    		struct simplefs_inode *sfs_inode;

    		sfs_inode = simplefs_get_inode(sb, record->inode_no);

    		inode = new_inode(sb);
    		inode->i_ino = record->inode_no;
    		inode_init_owner(inode, parent_inode, sfs_inode->mode);
    		inode->i_sb = sb;
    		inode->i_op = &simplefs_inode_ops;

    		if (S_ISDIR(inode->i_mode))
    			inode->i_fop = &simplefs_dir_operations;
    		else if (S_ISREG(inode->i_mode)) {
    			inode->i_fop = &simplefs_file_operations;
    			inode->i_mapping->a_ops = &simplefs_aops;
    		}
    		else if (S_ISLNK(inode->i_mode)) {
    			inode->i_op = &simplefs_symlink_inode_ops;
    			inode->i_mapping->a_ops = &simplefs_aops;
    		}
    		else
    			printk(KERN_ERR
    			       "Unknown inode type. Neither a directory nor a file");

    		/* FIXME: We should store these times to disk and retrieve them */
    		inode->i_atime = inode->i_mtime = inode->i_ctime =
    		    CURRENT_TIME;

    		inode->i_private = sfs_inode;

    		d_add(child_dentry, inode);
    		return NULL;
    	}
    	record++;
    }

    printk(KERN_ERR
           "No inode found for the filename [%s]\n",
           child_dentry->d_name.name);

    return NULL;
}


/**
 * Simplest
 */
void simplefs_destory_inode(struct inode *inode)
{
    struct simplefs_inode *sfs_inode = SIMPLEFS_INODE(inode);
    __PRINT_FUNC_INFO();
    printk(KERN_INFO "Freeing private data of inode %p (%lu)\n",
           sfs_inode, inode->i_ino);
    kmem_cache_free(sfs_inode_cachep, sfs_inode);
}

static const struct super_operations simplefs_sops = {
    .destroy_inode = simplefs_destory_inode,
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
           const char *p_dev_name, 挂载目录
* 输出参数:void *p_data, 指向出参的指针
* 返回值     :struct dentry *,表示返回root根目录
* 修改说明: 时间:2026/09/21
            作者:houchao
            说明:函数优化,增加注释信息
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

static void simplefs_kill_superblock(struct super_block *sb)
{
    printk(KERN_INFO
           "simplefs superblock is destroyed. Unmount succesful.\n");
    /* This is just a dummy function as of now. As our filesystem gets matured,
     * we will do more meaningful operations here */

    kill_block_super(sb);
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
            作者:houchao
            说明:函数优化,增加注释信息
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


