#define SIMPLEFS_MAGIC 0x10032013
#define SIMPLEFS_DEFAULT_BLOCK_SIZE 4096
#define SIMPLEFS_FILENAME_MAXLEN 255
#define SIMPLEFS_START_INO 10
#define SIMPLEFS_SYMLINK_PATH_MAX SIMPLEFS_DEFAULT_BLOCK_SIZE

/**
 * Reserver inodes for super block, inodestore
 * and datablock
 */
#define SIMPLEFS_RESERVED_INODES 3

/*封装函数打印宏 -2019/03/31-*/
#define __PRINT_FUNC_INFO() do { \
    pr_info("__FUNCTION[%s],LINE[%d]\n",__FUNCTION__,__LINE__); \
}while(0)

/*封装函数打印宏用户mount_dev*/
#define __PRINT_FUNC_MOUNT_BDEV_INFO(fs_type_name, flags, dev_name) do { \
    pr_info("fs_type_name[%s], flags[%d], dev_name[%s]\n", fs_type_name, flags, dev_name);\
}while(0)

#ifdef SIMPLEFS_DEBUG
#define sfs_trace(fmt, ...) {                       \
    printk(KERN_ERR "[simplefs] %s +%d:" fmt,       \
           __FILE__, __LINE__, ##__VA_ARGS__);      \
}
#define sfs_debug(level, fmt, ...) {                \
    printk(level "[simplefs]:" fmt, ##__VA_ARGS__); \
}
#else
#define sfs_trace(fmt, ...) no_printk(fmt, ##__VA_ARGS__)
#define sfs_debug(level, fmt, ...) no_printk(fmt, ##__VA_ARGS__)
#endif

/* Hard-coded inode number for the root directory */
const int SIMPLEFS_ROOTDIR_INODE_NUMBER = 1;

/* The disk block where super block is stored */
const int SIMPLEFS_SUPERBLOCK_BLOCK_NUMBER = 0;

/* The disk block where the inodes are stored */
const int SIMPLEFS_INODESTORE_BLOCK_NUMBER = 1;

/* The disk block where the name+inode_number pairs of the
 * contents of the root directory are stored */
const int SIMPLEFS_ROOTDIR_DATABLOCK_NUMBER = 2;

/* The name+inode_number pair for each file in a directory.
 * This gets stored as the data for a directory */
struct simplefs_dir_record {
    char filename[SIMPLEFS_FILENAME_MAXLEN];
    uint64_t inode_no;
};

struct simplefs_inode {
    mode_t mode;					//文件类型和访问权限
    uint64_t inode_no;				//inode编号
    uint64_t data_block_number;		//数据块编号
    uint64_t link_counter; 			//simplefs文件系统支持硬链接计数 modify 2019-05-19

    union {
    	uint64_t file_size;			//文件长度(以字节为单位)
    	uint64_t dir_children_count;//unused currently
    };
};

const int SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED = 64;
/* min (
    	SIMPLEFS_DEFAULT_BLOCK_SIZE / sizeof(struct simplefs_inode),
    	sizeof(uint64_t) //The free_blocks tracker in the sb
 	); */

/* FIXME: Move the struct to its own file and not expose the members
 * Always access using the simplefs_sb_* functions and
 * do not access the members directly */
struct simplefs_super_block {
    uint64_t version;															//超级块版本号
    uint64_t magic;																//文件系统魔术字
    uint64_t block_size;														//文件系统块长度(以字节为单位)

    /* FIXME: This should be moved to the inode store and not part of the sb */
    uint64_t inodes_count;														//文件系统里创建文件inode计数

    uint64_t free_blocks;														//超级块内剩余块个数
    char padding[SIMPLEFS_DEFAULT_BLOCK_SIZE - (5 * sizeof(uint64_t))];			//for what?
};

/*define for symlink file*/
struct simplefs_symlink_record {
    uint64_t size;
    char filename[SIMPLEFS_SYMLINK_PATH_MAX];
};

