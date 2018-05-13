#define FUSE_USE_VERSION 26
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fuse.h>
#include <sys/mman.h>
#include <inttypes.h>

#define MAX_FILENUM 512
#define MAX_FILE_BLOCK 250
#define BLOCK_SIZE 4096
#define INODE_SIZE 2048
#define BLOCKNUM 32*1024
#define INODENUM 512


//超级块SuperBlock起始地址为0,其结构如下
typedef struct {
	int blocksize;				//block大小	4KB
	int inodesize;				//inode大小 2KB
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

//block size 4KB block numbers 32K 
static void *node[INODENUM];
static void *mem[BLOCKNUM];
static inode *root;
static SuperBlock *super;

int inode_bitmap[INODENUM / 32];	    //inode bitmap inode位视图

int block_bitmap[BLOCKNUM / 32];		//block bitmap block位视图

//在block块链表中的第num后添加一个block
static void linkadd(inode *node,int k,int num)
{
    struct link *head = node->blocklink;

    int i=0;
    while(head->next && i <= num) {
        head = head->next;
        i++;
    }

    struct link *p=(struct link *)malloc(sizeof(struct link));
    p->blocknr = k;
    p->next = head->next;
    head->next = p;
}

//分配inode给文件
static struct inode *malloc_inode(int *t)
{
    int j=0;
    
    if(super->free_inodes <= 0)
        return NULL;
    else {
        for(int i = 0;i < INODENUM / 32;i++) {
            //若bitmap中已经被分配，则询问下一个inode是否被分配
            // == -1即意味着32位全为1 
            if(inode_bitmap[i] == -1)
                continue;
            else {
                if((inode_bitmap[i] >> j) % 2== 0) {
                    //移位操作 判断该位是否为0
                    super->free_inodes--;
                    inode_bitmap[i] += (1 << j);
                    node[i*32+j]=mmap(NULL, INODE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                    *t = i*32 +j;
                    return node[i*32+j];
                }
                else j++;
            }
        }
    }
} 


//分配一个block给文件inode中pointer空指针
static int malloc_block(inode *node)
{
    int i;
    int j=0;
    int k;
    for(i = 0;i < BLOCKNUM / 32;i++) {
        //从位图中选择出未分配的block
        if(block_bitmap[i] == -1)
            continue;
        else {
            if((block_bitmap[i] >> j) % 2== 0) 
                break;
            else j++;
        }
    }
    for(k = 0;k < MAX_FILE_BLOCK;k++)
        if(node->pointer[k] == NULL) {
            //选一个空指针将它分配
            mem[i*32+j]=mmap(NULL, BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if(mem[i*32+j] == MAP_FAILED) return -ENOENT;
            node->pointer[k] = mem[i*32+j];
            node->st->st_blocks++;
            super->free_blocknr--;
            block_bitmap[i] += (1 << j);
            break;
        }
    
    return k;
}


//回收q指向的block
void free_block(inode *node,struct link *q)
{
    void *p = node->pointer[q->blocknr];
    int i,j,k;
    for(i = 0;i < BLOCKNUM;i++) {
        if(p == mem[i]) {
            j = i / 32;
            k = i % 32;
            block_bitmap[j] -= (1 << k);
            node->st->st_blocks--;
            super->free_blocknr++;
            break;
        }
    }
    free(q);
    munmap(p,BLOCK_SIZE);
}

/*
static int realloc_block(inode *node,int m)
{
    int i = 0;
    int k;
    struct link *tail;
    struct link *q;
    if(m > node->st->st_blocks) {
        for(i = 0;i < m - node->st->st_blocks;i++) {
            k = malloc_block(node);
            linkadd(node,k,node->st->st_blocks -2);
        }
        return m;
    }
    else {
        for(i = 0;i < node->st->st_blocks - m;i++) {
            while(tail) {
                q = tail;
                tail = tail->next;
            }
            free_block(node,q);
        }
        return m;
    }    
}
*/

//回收inode
static void free_inode(inode *p)
{
    int i,j,k;
    struct link *head = p->blocklink->next;
    struct link *q;

    if(!p)  return;
    while(head) {
        q = head;
        head = head->next;
        free_block(p,q);
    }

    for(i = 0;i < INODENUM;i++) {
        if(p == node[i]) {
            j = i / 32;
            k = i % 32;
            inode_bitmap[j] -= (1 << k); 
            super->free_inodes++;
            break;
        }
    }
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

//创建inode
static void create_inode(const char *filename, const struct stat *st)
{
    int t;
    struct inode *root = (struct inode *)node[0];
    struct inode *new = malloc_inode(&t);
    memcpy(new->filename, filename, strlen(filename) + 1);
    new->st = (struct filestate *)malloc(sizeof(struct filestate));
    new->blocklink = (struct link *)malloc(sizeof(struct link));
    new->blocklink->next = NULL;
//  由于使用的是struct filestate而非struct stat 因此逐个赋值
    new->st->st_ino = t;
    new->st->st_mode = S_IFREG | 0644;
    new->st->st_uid = fuse_get_context()->uid;
    new->st->st_gid = fuse_get_context()->gid;
    new->st->st_blksize = BLOCK_SIZE;
    new->st->st_blocks = 0;

    for(int i=0;i < MAX_FILE_BLOCK;i++)
        new->pointer[i] = NULL;
    //头插法进入inode链表
    new->next = root->next;
    root->next = new;
}

static void *oshfs_init(struct fuse_conn_info *conn)
{
    int i;

    mem[0] = mmap(NULL, BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	node[0] = mmap(NULL,INODE_SIZE,PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

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
    for(i=0;i<MAX_FILE_BLOCK;i++)
        root->pointer[i] = NULL;

    root->st->st_ino = 0;
	root->st->st_uid = getuid();
    root->st->st_mode = S_IFDIR | 0755;
    root->st->st_gid = getgid();
    root->st->st_blksize = BLOCK_SIZE;
    root->st->st_blocks = 0;
    root->st->st_size = 0;

    block_bitmap[0] = 1;
    inode_bitmap[0] = 1;
    return NULL;
}

//getattr文件
static int oshfs_getattr(const char *path, struct stat *stbuf)
{
    int ret = 0;
    struct inode *node = get_inode(path);

    if(strcmp(path, "/") == 0) {
        memset(stbuf, 0, sizeof(struct stat));
        stbuf->st_mode = S_IFDIR | 0755;
    } else if(node) {
        //原因同上
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

//readdir
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

//mknod
static int oshfs_mknod(const char *path, mode_t mode, dev_t dev)
{
    struct stat st;
    st.st_mode = S_IFREG | 0644;
    st.st_uid = fuse_get_context()->uid;
    st.st_gid = fuse_get_context()->gid;
    st.st_nlink = 1;
    st.st_size = 0;
    create_inode(path + 1, &st);
    return 0;
}

//open打开文件
static int oshfs_open(const char *path, struct fuse_file_info *fi)
{
    int ret = 0;
    struct inode *node = get_inode(path);
    if(node)
        return 0;
    else 
        return -ENOENT;
}

//求最小值函数，下面会用到
static int min(int a,int b)
{
    if(a < b)
        return a;
    else return b;
}


//对文件进行写操作write
static int oshfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    int i,k;
    int j;
    int off,newoff;
    struct inode *node = get_inode(path);
    struct link *head = node->blocklink->next;
    
    node->st->st_size = offset + size;          // 计算文件的新的大小

    i = offset / BLOCK_SIZE;                    // 记录从哪个block号开始更改
    off = offset % BLOCK_SIZE;                  // 从block号中哪一字节开始更改
    j = 0;

    while(head && j < i) {
        //找到开始更改的block
        head = head->next;
        j++;
    }

    if(size <= BLOCK_SIZE - off) {
        //如果需要加入的字符在当前块就可以填入
        if(!head) {
            k = malloc_block(node);
            linkadd(node,k,i);
            memcpy(node->pointer[k] + off,buf,size);
        }
        else {
            memcpy(node->pointer[head->blocknr] + off,buf,size);
        }
    }
    else {
        //若需要加入的字符串会跨过两个块
        memcpy(node->pointer[head->blocknr] + off,buf,BLOCK_SIZE - off +1);
        j = size - (BLOCK_SIZE - off +1);       //j记录还剩下的未填入block的字节数
        //开始分配需要的块
        while(j > 0) {
            k = malloc_block(node);
            linkadd(node,k,i);
            newoff = min(j,BLOCK_SIZE);         //计算偏移量，避免把buf中的字符串漏拷贝或重复拷贝
            memcpy(node->pointer[k],buf + off,newoff);
            off += newoff;
            j -= BLOCK_SIZE;
            i++;
        }
    }

    return size;
}

//truncate截断
static int oshfs_truncate(const char *path, off_t size)
{
    int i = 0;
    int j;
    char *buf;
    struct inode *node = get_inode(path);
    struct link *head = node->blocklink->next;
    struct link *q;

    node->st->st_size = size;
    j = size / BLOCK_SIZE;
    //找到开始截断的block
    while(i < j && head) {
        head = head->next;
        i++;
    }
    //把这个块中余下的字符串拷贝到buf中
    memcpy(buf,node->pointer[head->blocknr],size % BLOCK_SIZE);
    q = head;
    //把后面的块全部删去
    head = head->next;
    q->next = NULL;
    while(head) {
        q = head->next;
        free_block(node,head);
        head = q;
    }
    return 0;
}

//read
static int oshfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    int i = 0;
    int m;
    struct inode *node = get_inode(path);
    struct link *head = node->blocklink->next;
    int ret = size;
    if(offset + size > node->st->st_size)
        ret = node->st->st_size - offset;
    
    //从k号开始读block,找到第k个block
    int k = offset / BLOCK_SIZE;
    m = offset % BLOCK_SIZE;
    while(i < k && head) {
        head = head->next;
        i++;
    }

    //j表示多余的字符长度
    int j = ret - (BLOCK_SIZE - m +1);
    memcpy(buf,node->pointer[head->blocknr] + offset,min(ret,BLOCK_SIZE - m +1));
    int off = BLOCK_SIZE - m +1;

    while(j > 0) {
        head = head->next;
        memcpy(buf + off,node->pointer[head->blocknr],min(j,BLOCK_SIZE));
        off += BLOCK_SIZE;
        j -= BLOCK_SIZE;
    }
    return ret;
}

//unlink函数
static int oshfs_unlink(const char *path)
{
	struct inode *p = (inode *)node[0];
	struct inode *name = get_inode(path);

    //找到对应的inode
	while(p) {
		if (strcmp(p->next->filename ,name->filename) != 0)
			p = p->next;
		else {
			p->next = name->next;
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
