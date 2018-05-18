

# 实验三README

## 文件系统结构

```c
# define MAX_FILENUM 1024		//最多的文件数量
# define BLOCKS_INODE 50		//inode中可以存放的block号码数
# define BLOCK_SIZE 4096		//一个块的大小
# define INODE_SIZE 512			//一个inode的大小
# define BLOCKNUM 32*1024		//总共的block数目
# define INODENUM 1024			//总共的inode数目
# define MAX_FILENAME 256		//最长的文件名长度
```

```c
//超级块SuperBlock起始地址为0,其结构如下`
`typedef struct {`
    int blocksize;              //block大小 4KB
    int inodesize;              //inode大小 2KB
    int sum_inodes;             //inode的总数
	int free_inodes;            //空闲inode的总数
	size_t sum_blocknr;            //块总数
    size_t free_blocknr;           //空闲块总数
    int first_inode;            //inode的起始点 
    int first_data;             //数据块起始点
`}SuperBlock;`
```

超级块放在mem[0]处，存储以上内容。记录文件系统的基本信息。

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

为了节省inode空间，这里只取了struct stat结构体中需要的项。有inode号码，文件权限，用户ID，用户组ID，整个文件的大小，文件块大小以及块数目。 

```c
//inode 存储，权限，文件大小，分配给的block块等
typedef struct inode{
    char filename[MAX_FILENAME];
    ssize_t  blnum[BLOCKS_INODE];
    ssize_t bindirect;              //间接索引，把block号码放在一个新的block块中
    struct filestate *st;
    struct inode *next;
}inode;
```

inode结构体，存放了文件名（filename），间接索引block号码，该block存放了inode中放不下的block号码。struct filestate结构体，存储着文件的信息，以及指向下一个inode的指针。

**block块没有定义结构体，因为整个block块中全部存放了数据，不需要定义什么结构体。**全部存放数据的好处是方便数据的对齐，对一些设备会比较友好。

```c
//block size 4KB block numbers 32K 
static void *node[INODENUM];
static void *mem[BLOCKNUM];
static inode *root;
static SuperBlock *super;
 
int *inode_bitmap;        //inode bitmap inode位视图

int *block_bitmap;
```

全局变量一览。数组mem存放的是block，数组node存放inode。**bitmap表示block和bitmap的分配情况，0表示未分配，1表示分配。block_bitmap数组存放在mem[1]中，而inode_bitmap数组存放在mem[0]中。**

## 文件系统基本情况

### 版本1.0

实现的文件系统大体上参考了linux中ext2文件系统的结构。我按自己的想法对read、write、readdir、getattr、truncate、init、create等函数进行简单的实现，源代码为oshfs.c。

由于电脑内存空间不足，我设计的该文件系统总大小只有130M左右，无法支持特别大的文件。。。除此之外，规定文件名不得超过128个字节，最多支持512个文件，最大文件为2M左右。

### 版本2.0 （目前的os.c为2.0版本）

实现2.0版本的文件系统仍然参考了linux中的ext2文件系统结构。对版本1.0中的不足（尤其是文件大小过小）进行改进。源代码为os.c，现在的可执行文件oshfs是由它编译产生的。2.0fuse文件系统整个大小为130M左右**（助教检查2.0版本吧。。。。）*****该文件系统中，文件名长度不能超过265个，最多支持1024个文件数量，最大文件130M，但在理论上，实现了索引式的文件系统最大文件可以达到4GB。block块大小为4KB，一共有32K个block，inode大小512Bytes，一共有1024个inode。**

## ****内存管理

总文件系统大小为130M左右。其中包含了32k个大小为4k的数据块，和512个大小为512Bytes的inode，还有一部分全局变量以及bitmap。在ext2中，文件inode实现了多级索引，即inode可以指向另一个inode，然后在索引相应的block（如下图）。我只实现了直接索引和一级间接索引和二级间接索引，但对于该文件系统来说已经足够了。

内存分配过程如下：
	1、首先创建一个文件，分配一个inode。分配时根据bitmap数组中的数据，找到一个未分配的inode，并写入文件的信息。

​	2、写操作时分配block。也是根据bitmap数组中的数据，找到一个未分配的inode，分配给该文件，把block编号给inode，然后放在inode的blnum这个数组中。而数组只有50个元素，当分配的块多于50个时，就要分配一个block，把号码给bindirect，这个块就是记录block号码的新空间，这个block可以记录1024个块号码。因此，此时，一个文件有4KB*（1024+50），相当于4M左右。

​	3、当分配的块多于（1024+50）个时，启用二级索引块。找到一个未分配的块，记录下存储着block块号码的块的号码，然后在该块中记录存储数据的block号码。实现二级索引（如下图），因此，实现了之后，可以获得4KB*1024**1024即4GB的最大文件容量，但由于文件系统本身的限制，只能存放130M的文件。

对于内存的回收，首先将block回收，如果block有超过50个，还要回收bindirect记录的block号码，再把inode回收。

![img](http://docs.linuxtone.org/ebooks/C&CPP/c/images/fs.datablockaddr.png)

## 扩展性

由于电脑内存不太充足，最大文件数量和最大文件不是很令人满意，不过如果要增加，可以修改宏定义进行扩展。

修改BLOCKNUM，增加BLOCK的块数，可以直接增大最大文件的限制。

这里已经实现了二级间接索引，理论上直接把文件大小扩展为4GB是没有问题的。

## 附录

**对于助教在github上提出，无法在该文件系统中运行cp /bin/sh test;cp /bin/sh test;的指令，很奇怪的一点是，在我的电脑上一切正常。如果助教的电脑上出现了什么问题，我也不知道怎么解决。。。。**

详见下图：

![2018-05-18 22-37-29屏幕截图](/home/dingfeng/桌面/3-dingfen/2018-05-18 22-37-29屏幕截图.png)![2018-05-18 22-38-32屏幕截图](/home/dingfeng/桌面/3-dingfen/2018-05-18 22-38-32屏幕截图.png)![2018-05-18 22-46-16屏幕截图](/home/dingfeng/桌面/3-dingfen/2018-05-18 22-46-16屏幕截图.png)