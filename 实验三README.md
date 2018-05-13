# 实验三README

## 文件系统结构

```c
//超级块SuperBlock起始地址为0,其结构如下`
`typedef struct {`
    int blocksize;              //block大小   4KB
    int inodesize;              //inode大小 2KB
    int sum_inodes;             //inode的总数
	int free_inodes;            //空闲inode的总数
	int sum_blocknr;            //块总数
    int free_blocknr;           //空闲块总数
    int first_inode;            //inode的起始点 
    int first_data;             //数据块起始点
`}SuperBlock;`
```

超级块放在mem[0]处，存储以上内容。

```c
//struct filestate是struct stat的缩量版
//为节省inode空间，故只存了一部分的有用的值
struct filestate {
//  dev_t     st_dev;         /* ID of device containing file */
    ino_t     st_ino;         /* inode number */
    mode_t    st_mode;        /* protection */
//  nlink_t   st_nlink;       /* number of hard links */
    uid_t     st_uid;         /* user ID of owner */
    gid_t     st_gid;         /* group ID of owner */
//  dev_t     st_rdev;        /* device ID (if special file) */
    off_t     st_size;        /* total size, in bytes */
    blksize_t st_blksize;     /* blocksize for filesystem I/O */
    blkcnt_t  st_blocks;      /* number of blocks allocated */
};
```

为了节省inode空间，这里只取了struct stat结构体中需要的项。 

```c
//inode 存储，权限，文件大小，分配给的block块等
typedef struct inode {
    char filename[128];
    void *pointer[MAX_FILE_BLOCK];
    struct link{
        int blocknr;                //block块的链表
        struct link *next;
    }*blocklink;
    struct filestate *st;
    struct inode *next;
}inode; 
```

inode结构体，存放了文件名（filename），指向文件中的block指针数组(pointer)，block的链表，struct filestate结构体，存储着文件的信息，以及指向下一个inode的指针。

```c
//block size 4KB block numbers 32K 
static void *node[INODENUM];
static void *mem[BLOCKNUM];
static inode *root;
static SuperBlock *super;
 
int inode_bitmap[INODENUM / 32];        //inode bitmap inode位视图

int block_bitmap[BLOCKNUM / 32];
```

全局变量一览。数组mem存放的是block，数组node存放inode。bitmap表示block和bitmap的分配情况，0表示未分配，1表示分配。

## 文件系统基本情况

实现的文件系统大体上参考了linux中ext2文件系统的结构。我按自己的想法对read、write、readdir、getattr、truncate、init、create等函数进行简单的实现，源代码为oshfs.c。

由于电脑原因，该文件系统总大小只有130M左右，无法支持特别大的文件。。。除此之外，规定文件名不得超过128个字节，最多支持512个文件，最大文件为2M。

## 内存管理

总文件系统大小为130M左右。其中包含了32k个大小为4k的数据块，和512个大小为2k的inode，还有一部分全局变量以及bitmap。在ext2中，文件inode实现了多级索引，即inode可以指向另一个inode，然后在索引相应的block。但是由于时间和能力有限，我只实现了直接索引，这也是单个文件大小被限制的主要原因。。。。

内存分配过程如下：
	1、首先创建一个文件，分配一个inode。分配时根据bitmap数组中的数据，找到一个未分配的inode，并写入文件的信息。

​	2、分配block。也是根据bitmap数组中的数据，找到一个未分配的inode，分配给该文件，把block编号给inode，在inode里面组成一个block链表。这样做的目的是，防止无序的block编号影响的文件的读写操作。

对于内存的回收，首先将block回收，再把inode回收。

![img](http://docs.linuxtone.org/ebooks/C&CPP/c/images/fs.datablockaddr.png)

## 扩展性

由于设备原因，最大文件数量和最大文件不是很令人满意，可以修改几个常数进行扩展。

比如，修改BLOCKSIZE和BLOCKNUM增加块数的数量以及大小，修改INODENUM和INODESIZE增加单个文件的大小，注意大小最好是2的幂次。