#define FUSE_USE_VERSION 26
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fuse.h>
#include <sys/mman.h>
#include <inttypes.h>

#define MAX_FILENUM 1024
#define BLOCKS_INODE 50
#define BLOCK_SIZE 4096
#define INODE_SIZE 512
#define BLOCKNUM 32*1024
#define INODENUM 1024
#define MAX_FILENAME 256

//超级块SuperBlock起始地址为0,其结构如下
typedef struct {
	int blocksize;				//block大小	4KB
	int inodesize;				//inode大小 512B
	int sum_inodes;				//inode的总数
	int free_inodes; 			//空闲inode的总数
	int sum_blocknr;			//块总数
	int free_blocknr;			//空闲块总数
	int first_inode;			//inode的起始点	
	int first_data;				//数据块起始点
}SuperBlock;

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

//inode 存储，权限，文件大小，分配给的block块等
typedef struct inode{
    char filename[MAX_FILENAME];
    int  blnum[BLOCKS_INODE];
    int bindirect;              //间接索引，把block号码放在一个新的block块中
    struct filestate *st;
    struct inode *next;
}inode;

//node数组指向inode的地址 mem数组指向block的地址
static void *node[INODENUM];
static void *mem[BLOCKNUM];
static inode *root;
static SuperBlock *super;

int32_t *inode_bitmap;	    //inode bitmap inode位图

int32_t *block_bitmap;		//block bitmap block位图


//分配inode给文件
static int malloc_inode()
{
    int j;
    int i;

    if(super->free_inodes <= 0)
        return -ENOSPC;
    else 
        for(i = 0,j = 0;i < INODENUM / 32;i++)
            //若bitmap中已经被分配，则询问下一个inode是否被分配
            // == -1即意味着32位全为1 
            if(inode_bitmap[i] == -1)
                continue;
            else {
                if((inode_bitmap[i] >> j) % 2== 0) {
                    //移位操作 判断该位是否为0 若未分配（为0）那么返回inode号码
                    super->free_inodes--;
                    inode_bitmap[i] += (1 << j);
                    node[i*32+j] = mmap(NULL, INODE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                    return i*32+j;
                }
                else j++;
            }
} 


//分配一个block给文件inode中pointer空指针
static int malloc_block(inode *node)
{
    int i,j;

    for(i = 0,j = 0;i < BLOCKNUM / 32;i++) {
        //从位图中选择出未分配的block
        if(block_bitmap[i] == -1)
            continue;
        else
            if((block_bitmap[i] >> j) %2 ==0) 
                break;
            else j++;
    }
    if(i == BLOCKNUM /32)
        return -ENOSPC;
    //给block分配内存
    mem[i*32+j] = mmap(NULL, BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(mem[i*32+j] == MAP_FAILED) 
        return -ENOSPC;
    node->st->st_blocks++;
    super->free_blocknr--;
    block_bitmap[i] += (1 << j);
    
    return i*32+j;
}


//回收inode中的第n个block
void free_block(inode *node,int n)
{
    int i,j,k;
    if(n < BLOCKS_INODE) {
        //记录block的号码并将其内存释放
        i = node->blnum[n];
        if(mem[i] == NULL)
            return;
        node->blnum[n] = 0;
        munmap(mem[i],BLOCK_SIZE);
    }
    else {
        //block号码存储在间接索引的block中的情况
        if(node->bindirect == 0)
            return;
        int *p = (int *)mem[node->bindirect];
        i = *(p + n - BLOCKS_INODE);
        if(mem[i] == NULL)
            return;
        *(p + n -BLOCKS_INODE) = 0;
        munmap(mem[i],BLOCK_SIZE);
    }
    j = i / 32;
    k = i % 32;
    block_bitmap[j] -= (1 << k);
    node->st->st_blocks--;
    super->free_blocknr++;
}

//回收inode
static void free_inode(inode *p)
{
    int i,j,k;
    if(!p)  return;
    //回收inode指向的存放block号码的block
    if(p->bindirect != 0) {
        j = p->bindirect / 32;
        k = p->bindirect % 32;
        block_bitmap[j] -= (1 << k);
        munmap(mem[p->bindirect],BLOCK_SIZE);
    }
    i = p->st->st_ino;
    j = i / 32;
    k = i % 32;
    inode_bitmap[j] -= (1 << k); 
    super->free_inodes++;
    munmap(p,INODE_SIZE);
}



//取得文件的inode
static struct inode *get_inode(const char *name)
{
    struct inode *node = root;
    //从inode链表中找到正确的node
    while(node) {
        if(strcmp(node->filename,name+1)!= 0)
            node = node->next;
        else   
            return node;
    }
    return NULL;
}


static void create_inode(const char *filename, const struct stat *st)
{
    int t = malloc_inode();
    struct inode *new = (inode *)node[t];

    memcpy(new->filename, filename, strlen(filename) + 1);
    new->st = (struct filestate *)malloc(sizeof(struct filestate));
    //  由于使用的是struct filestate而非struct stat 因此逐个赋值
    new->st->st_ino = t;
    new->st->st_mode = S_IFREG | 0644;
    new->st->st_uid = fuse_get_context()->uid;
    new->st->st_gid = fuse_get_context()->gid;
    new->st->st_blksize = BLOCK_SIZE;
    new->st->st_blocks = 0;
    new->bindirect = 0;
    for(int i=0;i < BLOCKS_INODE;i++)
        new->blnum[i] = 0;
    //  头插法进入inode链表
    new->next = root->next;
    root->next = new;
}


static void *oshfs_init(struct fuse_conn_info *conn)
{
    int i;

    mem[0] = mmap(NULL, BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mem[1] = mmap(NULL, BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	node[0] = mmap(NULL,INODE_SIZE,PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    block_bitmap = (int *)mem[1];
    inode_bitmap = (int *)(mem[0]+1024);
    memset(inode_bitmap,0,INODENUM / 32 * sizeof(int));
    memset(block_bitmap,0,BLOCKNUM / 32 * sizeof(int));
    //superblock的初始化以及root的初始化
	super = (SuperBlock *)mem[0];
	super->blocksize = BLOCK_SIZE;
	super->inodesize = INODE_SIZE;
	super->sum_inodes = MAX_FILENUM;
	super->sum_blocknr = BLOCKNUM;
	super->free_inodes = MAX_FILENUM - 1;
	super->free_blocknr = BLOCKNUM - 1;
	super->first_inode = 1;
	super->first_data = MAX_FILENUM;

    root = (inode *)node[0];
	root->st = (struct filestate *)malloc(sizeof(struct filestate));
    strcpy(root->filename, "/");
    //blnum[]中的值为0 意味着无效
    for(i = 0;i<BLOCKS_INODE; i++)
        root->blnum[i] = 0;
    root->bindirect = 0;
    root->st->st_ino = 0;
	root->st->st_uid = getuid();
    root->st->st_mode = S_IFDIR | 0755;
    root->st->st_gid = getgid();
    root->st->st_blksize = BLOCK_SIZE;
    root->st->st_blocks = 0;
    root->st->st_size = 0;

    block_bitmap[0] = 3;
    inode_bitmap[0] = 1;
    printf("%d %d\n",block_bitmap[2],inode_bitmap[10]);
    return NULL;
}


static int oshfs_getattr(const char *path, struct stat *stbuf)
{
    int ret = 0;
    struct inode *node = get_inode(path);

    if(strcmp(path, "/") == 0) {
        memset(stbuf, 0, sizeof(struct stat));
        stbuf->st_mode = S_IFDIR | 0755;
    } else if(node) {
        //原因同上
        stbuf->st_ino = node->st->st_ino;
        stbuf->st_mode = node->st->st_mode;
        stbuf->st_uid = node->st->st_uid;
        stbuf->st_gid = node->st->st_gid;
        stbuf->st_size = node->st->st_size;
        stbuf->st_blksize = node->st->st_blksize;
        stbuf->st_blocks = node->st->st_blocks;
    } else {
        ret = -ENOENT;
    }
    return ret;
}


static int oshfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
    struct inode *p = ((struct inode *)node[0])->next;
    struct stat *p_st;

    p_st = (struct stat *)malloc(sizeof(struct stat));
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    while(p) {
        //原因同上
        //此外，malloc一个p_st是为了满足filler函数参数中必须是struct stat的要求
        p_st->st_ino = p->st->st_ino;
        p_st->st_mode = p->st->st_mode;
        p_st->st_uid = p->st->st_uid;
        p_st->st_gid = p->st->st_gid;
        p_st->st_size = p->st->st_size;
        p_st->st_blksize = p->st->st_blksize;
        p_st->st_blocks = p->st->st_blocks;
        filler(buf,p->filename,p_st,0);
        p = p->next;
    }
    free(p_st);
    return 0;
}


static int oshfs_mknod(const char *path, mode_t mode, dev_t dev)
{
    struct stat st;
    st.st_mode = S_IFREG | 0644;
    st.st_uid = fuse_get_context()->uid;
    st.st_gid = fuse_get_context()->gid;
    st.st_nlink = 1;
    st.st_size = 0;
    st.st_blksize = BLOCK_SIZE;
    create_inode(path + 1, &st);
    return 0;
}


static int oshfs_open(const char *path, struct fuse_file_info *fi)
{
    return 0;
}


static int oshfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    int i,j,k,n;
    ssize_t off,newoff,min;
    int32_t *p;
    struct inode *node = get_inode(path);
    
    node->st->st_size = offset + size;          // 计算文件的新的大小
    j = offset / BLOCK_SIZE;                    // 记录从哪个block号开始更改
    off = offset % BLOCK_SIZE;                  // 从block号中哪一字节开始更改
    // 找到第j个block的号码
    if(j < BLOCKS_INODE) { 
        n = node->blnum[j];
        if(n == 0) {
            //若指向的block未分配
            k = malloc_block(node);
            node->blnum[j] = k;
            memcpy(mem[k],buf,size);
        }
        else {
            //block已经分配了
            k = size - (BLOCK_SIZE - off +1);          //表示存完该block后，剩余的字节数
            min = size < (BLOCK_SIZE - off +1) ? size:(BLOCK_SIZE -off +1);
            memcpy(mem[n] + off,buf,min);
            
            while(k > 0) {
                j++;
                off = min;
                min = k < BLOCK_SIZE ? k:BLOCK_SIZE;
                //上一个block存不下 新分配一个block
                n = malloc_block(node);
                if(j < BLOCKS_INODE) 
                    node->blnum[j] = n;
                else {
                    node->bindirect = malloc_block(node);
                    p = (int *)mem[node->bindirect];
                    *(p + j - BLOCKS_INODE) = n;
                }
                memcpy(mem[n],buf + off,min);
                off += min;
                k -= BLOCK_SIZE;
            }
        }
    }
    else {
        //若bindirect == 0 分配存放block号码的block
        if(node->bindirect == 0) {
            k = malloc_block(node);
            node->st->st_blocks--;          //减去因为分配这个block而导致的数目不相符
            node->bindirect = k;
        }
        p = (int *)mem[node->bindirect];
        n = *(p + j - BLOCKS_INODE);        //n获得了block号码
        //若指向的block未分配
        if(n == 0 || mem[n] == NULL) {
            k = malloc_block(node);
            *(p + j - BLOCKS_INODE) = k;
            memcpy(mem[k],buf,size);
        }
        else {
            //若block已经被分配
            k = size - (BLOCK_SIZE - off +1);
            min = size < (BLOCK_SIZE - off +1) ? size:(BLOCK_SIZE -off +1);
            memcpy(mem[n] + off,buf,min);

            while(k > 0) {
                j++;
                off = min;
                min = k < BLOCK_SIZE ? k:BLOCK_SIZE;
                //上一个block存不下 新分配一个block
                k = malloc_block(node);
                *(p + j - BLOCKS_INODE) = k;
                memcpy(mem[n],buf + off,min);
                off += min;
                k -= BLOCK_SIZE;
            }
        }
    }


    return size;
}


//从第beg个块开始释放后面所有的块的内存
void trun(inode *node,int beg,int blnr)
{
    int i,k,m;

    for(i = beg;i < blnr;i++)
            free_block(node,i);
        if(beg < BLOCKS_INODE && node->bindirect != 0) {
            //若把间接存放在block里面的block号码空间都释放了
            //则也应该释放bindirect这个block
            k = node->bindirect / 32;
            m = node->bindirect % 32;
            block_bitmap[k] -= (1 << m);
            munmap(mem[node->bindirect],BLOCK_SIZE);
            node->bindirect = 0;
        }
}


static int oshfs_truncate(const char *path, off_t size)
{
    int i,blnr;
    int j,k,m;
    char *buf;
    struct inode *node = get_inode(path);

    buf = (char*)malloc(sizeof(char)*BLOCK_SIZE);
    node->st->st_size = size;
    blnr = node->st->st_blocks;         //记录inode的block总数
    j = size / BLOCK_SIZE;              //找到开始截断的第j个block
    //把这个块中余下的字符串拷贝到buf中
    if(size % BLOCK_SIZE != 0)
        if(j < BLOCKS_INODE) {
            memcpy(buf,mem[node->blnum[j]],size % BLOCK_SIZE);
            //从第j个块开始，把后面的块释放掉
            trun(node,blnr,j);
            k = malloc_block(node);
            node->blnum[j] = k;
            memcpy(mem[k],buf,size % BLOCK_SIZE);
        }
        else {
            int32_t *p = (int32_t *)mem[node->bindirect];
            m = *(p + j - BLOCKS_INODE);
            memcpy(buf,mem[m],size % BLOCK_SIZE);
            //从第j个块开始，把后面的块释放掉
            trun(node,blnr,j);
            k = malloc_block(node);
            *(p + j -BLOCKS_INODE) = k;
            memcpy(mem[k],buf,size % BLOCK_SIZE);
        }
    else {
        //从第j个块开始，把后面的块释放掉
        trun(node,blnr,j);
        }
    free(buf);
    return 0;
}


static int oshfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    int i = 0;
    int j,m,n,k;
    ssize_t off,min;
    int32_t *p;
    struct inode *node = get_inode(path);
    int ret = size;

    if(offset + size > node->st->st_size)
        ret = node->st->st_size - offset;
    
    j = offset / BLOCK_SIZE;            // 从k号开始读block,找到第k个block
    m = offset % BLOCK_SIZE;            // m表示块内偏移量
    k = ret - (BLOCK_SIZE - m +1);      // j表示多余的字符长度
    min = ret < (BLOCK_SIZE - m +1) ? ret : (BLOCK_SIZE -m +1);  // min取最小的字节

    if(j < BLOCKS_INODE) 
        n = node->blnum[j];
    else {
        p = (int *)mem[node->bindirect];
        n = *(p + j - BLOCKS_INODE);
    }

    memcpy(buf,mem[n] + m,min);
    off = BLOCK_SIZE - m +1;
    while(k > 0) {
        //找到下一个块 读取内容
        j++;
        if(j < BLOCKS_INODE) 
            n = node->blnum[j];
        else {
            p = (int *)mem[node->bindirect];
            n = *(p + j - BLOCKS_INODE);
        }
        min = k < BLOCK_SIZE ? k:BLOCK_SIZE;
        memcpy(buf + off,mem[n],min);
        off += BLOCK_SIZE;
        k -= BLOCK_SIZE;
    }
    return ret;
}


static int oshfs_unlink(const char *path)
{
    struct inode *p = (inode *)node[0];
	struct inode *name = get_inode(path);
    int i;
    int blnr = p->st->st_blocks;
    //找到对应的inode
	while(p) {
		if (strcmp(p->next->filename ,name->filename) != 0)
			p = p->next;
		else {
			p->next = name->next;
            for(i = 0;i < blnr;i++)
                free_block(p,i);
			free_inode(name);
			break;
		}
	}

    return 0;
}


static const struct fuse_operations op = {
    .init = oshfs_init,
    .getattr = oshfs_getattr,
    .readdir = oshfs_readdir,
    .mknod = oshfs_mknod,
    .open = oshfs_open,
    .write = oshfs_write,
    .truncate = oshfs_truncate,
    .read = oshfs_read,
    .unlink = oshfs_unlink,
};

int main(int argc, char *argv[])
{
    return fuse_main(argc, argv, &op, NULL);
}
