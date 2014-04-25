/*
 * LFTL main file
 * modified over mtdblock.c
 * Copyright © 1999-2010 David Woodhouse <dwmw2@infradead.org>
 * Copyright © 2000-2003 Nicolas Pitre <nico@fluxnic.net>
 * modified by srimugunthan dhandapani srimugunthan.dhandapani@gmail.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */



#include "my_blktrans.h"
#include "lfq.h"
#include <linux/kthread.h>

#define ASSERT_ON 1
//#define MYFTL_DEBUG 1
//#define LOCK_CWR_DEBUG 1
//#define LOCK_BUF_DEBUG 1

//#define  USE_VIRGO_RESTRICTED 1
//#define EIGHT_BANK_FLASH 1
//#define NON_SCHEDULED 1
//#define PLL_GC_DEBUG 1

#define BG_C_GC 1
//#define BG_UNC_GC 1
//#define NONBG_GC 1


#define INVALID_VALUE (-1)

uint32_t numpllbanks = 64;

//#define APR_DEBUG 1
//#define PREFETCH_ACL 1

module_param(numpllbanks, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(numpllbanks, "Number of parallel bank units in the flash");

uint32_t first_time = 0;
module_param(first_time, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(numpllbanks, "boolean value, if the module is loaded firsttime");

//#ifdef USE_VIRGO_RESTRICTED
uint32_t index_in_bank = 0;

module_param(index_in_bank, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(index_in_bank, "index in bank to start from");

//#endif

#define INVALID_CACHE_NUM 0x7F
#define INVALID_SECT_NUM 0xFFFFFF		


atomic_t num_gcollected;
atomic_t gc_on_writes_collisions;
atomic_t num_gc_wakeups;

atomic_t num_l0_gcollected;
atomic_t num_l1_gcollected;
atomic_t num_l2_gcollected;
atomic_t num_erase_gcollected;
atomic_t num_cperase_gcollected;

atomic_t num_gc_threads;
atomic_t num_prefetch_threads;

#if 0
char cache_num[16777216];
#endif
static DECLARE_BITMAP(page_bitmap, 16777216);
static DECLARE_BITMAP(page_incache_bitmap, 16777216);
static DECLARE_BITMAP(ftlblk_incache_bitmap, (16777216*8));
static DECLARE_BITMAP(maptab_bitmap, 16777216);
static DECLARE_BITMAP(gc_map,262144);
static DECLARE_BITMAP(gc_bankbitmap,64);

struct ftlcache 
{
	//uint8_t *buf;
	uint8_t cache_state;
	unsigned long cache_offset;
	unsigned long sect_idx;
	unsigned long page_idx;
	uint32_t logic_page;
	long unsigned int written_mask;
	
	uint32_t logic_sect_num[64];
	atomic_t writes_in_progress ;
	atomic_t flush_in_progress;
	atomic_t wait_to_flush;
	unsigned long last_touch;
	
}__attribute__((packed)); 

struct prefetchcache 
{
	unsigned long last_touch;
	long unsigned int read_mask;
	int state;
	int index_inlist;
}__attribute__((packed)); 

struct cur_wr_info{
	uint32_t first_blk;
	uint32_t last_blk;
	uint32_t last_gc_blk;
	uint32_t blk;
	uint8_t state;
	uint8_t last_wrpage;
	int centroid;
};

#define MAX_FTL_CACHEBUFS 256
#define MAX_FTL_PREFETCH_BUFS 32

#define STATE_EMPTY 0
#define STATE_DIRTY 1
#define STATE_FULL 2
#define STATE_CLEAN 3

#define GC_THRESH 100000
#define INVALID -1
#define RAND_SEL -2

#define ACCEPTABLE_THRESH 256
//#define INVALID_PAGE_NUMBER (-1ULL)
#define INVALID_PAGE_NUMBER (0xFFFFFFFF)


#define NO_BUF_FOR_USE -1
#define NUM_FREE_BUFS_THRESH 5

/* 64MB */
#define MAP_TABLE_SIZE 16777216
#define BLK_BITMAP_SIZE 4096

struct rw_semaphore map_tabl_lock;
uint64_t map_table[16777216];
#if 1
uint64_t reverse_map_tab[16777216];
uint64_t scanseqnumber[16777216];
#endif
uint64_t buf_lookup_tab[MAX_FTL_CACHEBUFS];

static int gc_testing_on = 0;

int pref_buf_count =0;

static struct kmem_cache *qnode_cache;

static struct lfq_queue_rcu empty_bufsq;
static struct lfq_queue_rcu full_bufsq;
static struct lfq_queue_rcu  pfetch_bufsq;
static struct lfq_queue_rcu spare_bufQ;

static struct lfq_queue_rcu spare_oobbufQ;

			 
static struct list_lru fdirty_bufs_list;
static struct list_lru empty_bufs_list;
static struct list_lru dirty_bufs_list;

void *cache_list_ptr[MAX_FTL_CACHEBUFS];	
void *spare_cache_list_ptr[MAX_FTL_CACHEBUFS];	
void *spare_oobbuf_list_ptr[MAX_FTL_CACHEBUFS];	

#define GC_NUM_TOBE_SCHEDULED 2

int scheduled_for_gc[GC_NUM_TOBE_SCHEDULED];

struct buf_sched_struct
{
	int buf_num;
	struct delayed_work workq; 
	
};

struct buf_sched_struct prefetch_buf_sched[MAX_FTL_PREFETCH_BUFS];
uint64_t pfetch_lookup_tab[MAX_FTL_CACHEBUFS];



struct per_bank_info
{
	atomic_t perbank_nfree_blks;
	atomic_t perbank_ndirty_pages; 
};

struct per_blk_info
{
	
	atomic_t num_valid_pages;
	DECLARE_BITMAP(valid_pages_map, 64);
};


struct oob_data
{
	char blk_type;	/*  Status of the block: data pages/map pages/unused */
	uint32_t logic_page_num;
	int32_t seq_number;	/* The sequence number of this block */
	
}__attribute__((packed));

#define DATA_BLK 1
#define MAP_BLK 2
#define FREE_BLK 0xFFFFFFFF	
#define NUM_GC_LEVELS 3
#define GC_LEVEL0 0
#define GC_LEVEL1 1
#define GC_LEVEL2 2

struct per_blk_info blk_info[262144];
	
struct per_bank_info bank_info[64];

uint8_t *prefetch_buf[MAX_FTL_PREFETCH_BUFS];
struct prefetchcache  p_fetch_info[MAX_FTL_PREFETCH_BUFS];
struct rw_semaphore pfetch_buf_lock[MAX_FTL_PREFETCH_BUFS];

struct bank_activity_matrix
{
	atomic_t  num_reads[64];
	atomic_t  num_writes[64];
	atomic_t  gc_goingon[64];
	atomic_t  num_reads_pref[64];
};	


#define GC_OP 0
#define WR_OP 1
#define PFTCH_OP 2
#define RD_OP 3

#if 0
static int cost_op[4][4] = {75, 16, 1, 2,
		     75, 8, 10, 2,
		     1, 20, 1, 1,
		     0, 0, 0, 0};
int init_cost_thresh[4] = {16,20,20,100};
#endif
/*	GC|	W|	P|	Rd	|
GC|______h|_____h|______l|	l	|
W |______h|_____l|______h|	l	|
P |______l|_____h|______l|	l	|
Rd|_____________________________________|
*/
		
static int cost_op[4][4] = {1000, 1000, 100, 100,
			1000, 100, 1000, 50,
			1000, 1000, 50, 50,
			0, 0, 0, 0};
int init_cost_thresh[4] = {1000,1000,1000,1000};   
	     
static int cost_thresh[64][4];

#if 0
struct bank_activity_matrix *activ_matrix;
int OP;
OP = GC_OP ;
activ_matrix = mtdblk->activity_matrix;

cost = activ_matrix.gc_goingon[bank].counter*cost_op[OP][GC_OP] + activ_matrix.num_writes[bank].counter*cost_op[OP][WR_OP] + activ_matrix.num_reads_pref[bank].counter*cost_op[OP][PFETCH_OP] + activ_matrix.num_reads[bank].counter*cost_op[OP][READ_OP];
if(cost < cost_thresh[OP])
{ 
	selected_bank = bank;
  	break;
}
#endif
		 
#define MAX_ACCESS_LIST 1024
#define INVALID_PAGE_NUMBER_32 0xFFFFFFFF
#define MAX_PREF_THREAD 2
#define NUM_GC_THREAD 1

static DECLARE_BITMAP(prefetch_onbitmap,MAX_ACCESS_LIST);
static DECLARE_BITMAP(acl_state,MAX_ACCESS_LIST);	

atomic_t activenumgcthread;

struct mtdblk_dev;

struct gcthread_arg_data
{
	int thrdnum;
	struct mtdblk_dev *mtdblk_ptr;
};

struct mtdblk_dev {
	struct mymtd_blktrans_dev mbd;
	int count;
	unsigned int cache_size;
	atomic_t freeblk_count;
	uint32_t num_blks;
	uint32_t num_cur_wr_blks;

	DECLARE_BITMAP(free_blk_map,262144);
	uint64_t ckptrd_mask;
	//long unsigned int free_blk_map[BLK_BITMAP_SIZE];
	uint32_t blksize;
	uint8_t blkshift;
	uint8_t pageshift;
	uint32_t num_parallel_banks;
	uint32_t blks_per_bank;
	uint32_t pages_per_blk;
	
	struct cur_wr_info cur_writing[MAX_FTL_CACHEBUFS];
	struct cur_wr_info rand_writing;
	struct rw_semaphore cur_wr_state[MAX_FTL_CACHEBUFS];
	struct rw_semaphore rand_wr_state;
	struct rw_semaphore bufstate_lock[MAX_FTL_CACHEBUFS];
	struct rw_semaphore free_map_lock[64];
	
	
	
	
	struct mutex select_buf_lock;
	//enum { STATE_EMPTY, STATE_CLEAN, STATE_DIRTY } cache_state;
	uint8_t *exper_buf;
	uint8_t *FFbuf;
	int exper_buf_sect_idx;
	struct mutex exper_buf_lock;
	struct mutex flush_buf_lock;
	uint8_t *buf[MAX_FTL_CACHEBUFS];
	struct mutex buf_lock[MAX_FTL_CACHEBUFS];
	struct ftlcache cached_buf[MAX_FTL_CACHEBUFS];
	
	
	

	int buf_idx[MAX_FTL_CACHEBUFS];

	struct mutex  buf_lookup_tab_mutex;
	
#if 0
	struct lock_freeQ lfree_empty_bufsq;
	struct lock_freeQ lfree_full_dirty_bufsq;
#endif
	uint64_t cache_fullmask;
	
	atomic_t cache_assign_count;
	atomic_t seq_num;
	
	
	struct bank_activity_matrix activity_matrix;
	struct task_struct *bufflushd;
	int gc_thresh[NUM_GC_LEVELS];
	struct task_struct *ftlgc_thrd[NUM_GC_THREAD];
	int reserved_blks_per_bank;
	
	int first_ckpt_blk;
	
	uint32_t accesslist[MAX_ACCESS_LIST];
	int acc_listindex;
	struct task_struct *ftl_prefetch_thrd[MAX_PREF_THREAD];
	int hwblks_per_bank;
	unsigned long last_wr_time;
	DECLARE_BITMAP(gc_active_map,64);
	int init_not_done;
	struct gcthread_arg_data gcthrd_arg[NUM_GC_THREAD];
	
};

			

			
static uint64_t get_ppage(struct mtdblk_dev *mtdblk, int cur_wr_index,int from_gc_context);

static struct mutex mtdblks_lock;

void free_cache_num_node(struct rcu_head *head)
{
	struct cache_num_node *node =
			container_of(head, struct cache_num_node, rcu);
	
	kmem_cache_free(qnode_cache, node);
	//kfree(node);
}						

/*
 * Cache stuff...
 *
 * Since typical flash erasable sectors are much larger than what Linux's
 * buffer cache can handle, we must implement read-modify-write on flash
 * sectors for each block write requests.  To avoid over-erasing flash sectors
 * and to speed things up, we locally cache a whole flash sector while it is
 * being written to until a different sector is required.
 */

static void erase_callback(struct erase_info *done)
{
	wait_queue_head_t *wait_q = (wait_queue_head_t *)done->priv;
	wake_up(wait_q);
}
#if 0
static int async_erase(struct mtdblk_dev *mtdblk,uint64_t blk)
{
	struct erase_info erase;
	DECLARE_WAITQUEUE(wait, current);
	wait_queue_head_t wait_q;

	int ret;
	struct mtd_info *mtd;
	uint64_t pos;
	//pos = blk * mtdblk->pages_per_blk*mtdblk->bytes_per_page;
	pos = (blk * mtdblk->pages_per_blk)<<mtdblk->pageshift;
	mtd = mtdblk->mbd.mtd;
	/*
	 * First, let's erase the flash block.
	 */

	init_waitqueue_head(&wait_q);
	erase.mtd = mtd;
	erase.callback = erase_callback;
	erase.addr = pos;
	erase.len = mtd->erasesize;
	erase.priv = (u_long)&wait_q;

	set_current_state(TASK_INTERRUPTIBLE);
	add_wait_queue(&wait_q, &wait);

	ret = mtd->erase(mtd, &erase);
	if (ret) {
		set_current_state(TASK_RUNNING);
		remove_wait_queue(&wait_q, &wait);
		printk (KERN_WARNING "mtdblock: erase of region %llx , len %ld "
				     "on \"%s\" failed\n",
			pos, erase.len, mtd->name);
		return ret;
	}
	
	schedule();  /* Wait for erase to finish. */
	remove_wait_queue(&wait_q, &wait);
	set_current_state(TASK_RUNNING);
	if (erase.state == MTD_ERASE_DONE)
	{
		
		return 0;
	}
	else if(erase.state == MTD_ERASE_FAILED)
	{
		
		printk(KERN_INFO  "myftl:erase failed %lld",blk);
		BUG();
		//return -EIO;
	}
	else
	{
		printk(KERN_INFO  "myftl:erase state unk %lld",blk);
		BUG();
	}

	return 0;
}
#endif

static int async_erase(struct mtdblk_dev *mtdblk,uint64_t blk)
{
	struct erase_info erase;
	
	DECLARE_WAITQUEUE(wait, current);
	wait_queue_head_t wait_q;

	int ret;
	struct mtd_info *mtd;
	uint64_t pos;
	//pos = blk * mtdblk->pages_per_blk*mtdblk->bytes_per_page;
	pos = (blk * mtdblk->pages_per_blk)<<mtdblk->pageshift;
	mtd = mtdblk->mbd.mtd;
	/*
	* First, let's erase the flash block.
	*/
	printk(KERN_INFO "asynch er %lld",pos);

	init_waitqueue_head(&wait_q);
	erase.mtd = mtd;
	erase.callback = erase_callback;
	erase.addr = pos;
	erase.len = mtd->erasesize;
	erase.priv = (u_long)&wait_q;

	
	
	/*
	FIXME: Allow INTERRUPTIBLE. Which means
	not having the wait_queue head on the stack.

	If the wq_head is on the stack, and we
	leave because we got interrupted, then the
	wq_head is no longer there when the
	callback routine tries to wake us up.
	*/
	ret = mtd->erase(mtd, &erase);
	if(!ret)
	{
		/* the waitQ usage is similar to kernel/perfmon.c*/
		add_wait_queue(&wait_q, &wait);
		/* TODO: let there be a timeout here*/
		while (1)
		{
			set_current_state(TASK_UNINTERRUPTIBLE);
			if (erase.state == MTD_ERASE_DONE ||  erase.state == MTD_ERASE_FAILED)
				break;
			schedule();
	
		}
		set_current_state(TASK_RUNNING);
		remove_wait_queue(&wait_q, &wait);	
	}
	
	
	if (erase.state == MTD_ERASE_DONE)
	{
		
		return 0;
	}
	else if(erase.state == MTD_ERASE_FAILED)
	{
		
		printk(KERN_INFO  "myftl:erase failed %lld",blk);
		BUG();
		//return -EIO;
	}
	else
	{
		printk(KERN_INFO  "myftl:erase state %d unk %lld",erase.state,blk);
		BUG();
	}

	return 0;
}



static int sync_erase(struct mtdblk_dev *mtdblk,uint64_t blk)
{
	struct erase_info erase;
	//udelay(1000);
	#if 1
	DECLARE_WAITQUEUE(wait, current);
	wait_queue_head_t wait_q;

	int ret;
	struct mtd_info *mtd;
	uint64_t pos;
	//pos = blk * mtdblk->pages_per_blk*mtdblk->bytes_per_page;
	pos = (blk * mtdblk->pages_per_blk)<<mtdblk->pageshift;
	mtd = mtdblk->mbd.mtd;
	/*
	* First, let's erase the flash block.
	*/
	printk(KERN_INFO "synch er %lld",pos);

	init_waitqueue_head(&wait_q);
	erase.mtd = mtd;
	erase.callback = NULL;
	erase.addr = pos;
	erase.len = mtd->erasesize;
	erase.priv = (u_long)&wait_q;

	
	
	/*
	 *  dont do asynchronous erase; it is confusing with the kthread sleeping
	 */
	ret = mtd->erase(mtd, &erase);
	
	while (1)
	{
		
		if (erase.state == MTD_ERASE_DONE ||  erase.state == MTD_ERASE_FAILED)
			break;
		schedule();

	}
	
	if (erase.state == MTD_ERASE_DONE)
	{
		
		return 0;
	}
	else if(erase.state == MTD_ERASE_FAILED)
	{
		
		printk(KERN_INFO  "myftl:erase failed %lld",blk);
		BUG();
		//return -EIO;
	}
	else
	{
		printk(KERN_INFO  "myftl:erase state %d unk %lld",erase.state,blk);
		BUG();
	}
#endif
	return 0;
}




int blk_isfree(struct mtdblk_dev *mtdblk, uint32_t blkno)
{
#if 0
	#ifdef USE_VIRGO_RESTRICTED
	int bank_num = blkno/4096;
	#else
	int bank_num = blkno/mtdblk->blks_per_bank;
	#endif
#endif
	int bank_num = blkno/mtdblk->hwblks_per_bank;
	
#ifdef EIGHT_BANK_FLASH
	bank_num = bank_num/8;
#endif
	#if 0
	printk(KERN_INFO "isblkfree %d of %d\n", blkno,bank_num);
	#endif
	
	down_read(&(mtdblk->free_map_lock[bank_num]));	
	if(!(test_bit(blkno,mtdblk->free_blk_map)))
	{
		up_read(&(mtdblk->free_map_lock[bank_num]));
		return 1;
	}	
	up_read(&(mtdblk->free_map_lock[bank_num]));	
	return 0;
}
int blk_unfree(struct mtdblk_dev *mtdblk,uint32_t blkno)
{
#if 0
	#ifdef USE_VIRGO_RESTRICTED
	int bank_num = blkno/4096;
	#else
	int bank_num = blkno/mtdblk->blks_per_bank;
	#endif
#endif
	int bank_num = blkno/mtdblk->hwblks_per_bank;
#ifdef EIGHT_BANK_FLASH
	bank_num = bank_num/8;
#endif
#if 0
	printk(KERN_INFO "blkunfree %d of %d\n", blkno,bank_num);
	#endif
	down_write(&(mtdblk->free_map_lock[bank_num]));	
	set_bit(blkno, mtdblk->free_blk_map);
	
	//mtdblk->freeblk_count--;
	atomic_dec(&mtdblk->freeblk_count);
	up_write(&(mtdblk->free_map_lock[bank_num]));	
	
	//printk(KERN_INFO "blkunfree %d ",blkno);
	return 0;
}

static int is_block_bad(struct mtdblk_dev *mtdblk,int ebnum)
{
	struct mtd_info *mtd = mtdblk->mbd.mtd;
	uint64_t  addr = ((uint64_t)ebnum) * mtd->erasesize;
	int ret;
#ifdef APR_DEBUG		

	printk(KERN_INFO "isblkbad %d \n", ebnum);
#endif
	ret = mtdblk->mbd.mtd->block_isbad(mtd, addr);
	if(gc_testing_on != 1)
	{
//		if (ret)
		//	printk(KERN_INFO "FTL: block %d is bad\n", ebnum);
	}
	return ret;
}


int blk_free(struct mtdblk_dev *mtdblk,uint32_t blkno)
{
#if 0
	#ifdef USE_VIRGO_RESTRICTED
	int bank_num = blkno/4096;
	#else
	int bank_num = blkno/mtdblk->blks_per_bank;
	#endif
#endif
	
	int bank_num = blkno/mtdblk->hwblks_per_bank;
#ifdef EIGHT_BANK_FLASH
	bank_num = bank_num/8;
#endif
	down_write(&(mtdblk->free_map_lock[bank_num]));	
	clear_bit(blkno, mtdblk->free_blk_map);
	//mtdblk->freeblk_count++;
	atomic_inc(&mtdblk->freeblk_count);
	
	up_write(&(mtdblk->free_map_lock[bank_num]));	
	return 0;
}

#define ITERATIONS_PER_GC_CALL 10

#define GC_DEBUG_L1 1


int do_gc_revmapt(struct mtdblk_dev *mtdblk,int bank_num,int level)
{
	uint32_t start_blk,end_blk;
	uint32_t min_vpages;
	uint32_t min;
	uint32_t i;
	uint32_t found_fulld_blk;
	uint32_t found = 0;
	uint32_t victim_blk;
	struct mtd_info *mtd;
	int loop_count;
	uint64_t mask;
	struct mtd_oob_ops ops;
	uint8_t *rd_buf,*oob_buf;
	uint32_t old_lpn;
	uint64_t new_phy_page,old_phy_page;
	struct oob_data oobvalues,*oobdata;
	uint32_t changed_ppn[64];
	uint32_t corresp_lpn[64];
	uint32_t n_valid_pages,n_dirty_pages;
	uint32_t new_blkno;
	uint64_t vpages_map;
	int res;
	uint32_t oldblkno,page_in_blk;
	uint32_t iterations;
	int count = 0;
	start_blk = mtdblk->cur_writing[bank_num].first_blk;
	end_blk = mtdblk->cur_writing[bank_num].last_blk;
#ifdef GC_DEBUG_L2	
	printk(KERN_INFO "myftl: do_gc_revmapt L%d on bank %d enter",level,bank_num);
#endif

	/* one thread does GC on a bank. and after one block is collected return. search only for max ITERATIONS_PER_GC_CALL*/
	if(test_and_set_bit(bank_num,gc_bankbitmap))
	{
#ifdef GC_DEBUG_L3
		printk(KERN_INFO "GC already on %d", bank_num);
#endif
		return 0 ;
			
	}
	
	

	
	/* victim gc block should not be 
	* 1) a free block
	* 2) a cur writing block
	* 3) should not become a cur writing block when gc is started
	*  if the block is not free, then it wont become a cur writing block after gc is started.
	*  so check only if it is <non-free and non curWriting>
	*/
	min_vpages = mtdblk->gc_thresh[level];
	//min_vpages = mtdblk->blk_info[start_blk].num_valid_pages.counter;
	min = start_blk;
	found_fulld_blk = 0;
	found = 0;
#if 0
	#ifdef NON_SCHEDULED
	iterations = mtdblk->blks_per_bank/16+1;
	#else
	iterations = mtdblk->blks_per_bank;
	
	#endif
	if(iterations > ITERATIONS_PER_GC_CALL)
	{
		iterations = ITERATIONS_PER_GC_CALL;
	}
#endif
	
	iterations = ITERATIONS_PER_GC_CALL;
	if(mtdblk->cur_writing[bank_num].last_gc_blk > mtdblk->cur_writing[bank_num].last_blk)
	{
		start_blk =  mtdblk->cur_writing[bank_num].first_blk;
	
	}
	else
	{
		start_blk = mtdblk->cur_writing[bank_num].last_gc_blk;
	}
	for(i = start_blk,count=0 ;count < iterations;i++,count++)
	{
		/* what happens when the block was not free when 
		* the next line is called
		* and when it gets to next-next line
		* it becomes free
		*/
		if(i > mtdblk->cur_writing[bank_num].last_blk)
		{
			i = mtdblk->cur_writing[bank_num].first_blk;
		}
		/* skip the bad block for GC*/
		if(is_block_bad(mtdblk,i))
		{
			continue;
		}
		if((!(blk_isfree(mtdblk,i))) && (mtdblk->cur_writing[bank_num].blk != i) && (!(test_and_set_bit(i,gc_map))))
		{
			if(blk_info[i].num_valid_pages.counter <= min_vpages)
			{
				if(blk_info[i].num_valid_pages.counter == 0)
				{
					victim_blk = i;
					/* only erase required */
					/* 2threads should not garbage collect the same block*/
				//	if(test_and_set_bit(victim_blk,gc_map))
				//	{
						/* already gc is going on in this block */
				//		continue;
						//goto search_gc_blk;
				//	}
					
				#ifdef GC_DEBUG_L1
					printk(KERN_INFO "%x: do_gc_revmapt: bank %d erase %d",current->pid,bank_num, victim_blk);		
				#endif
			
					
					sync_erase(mtdblk,victim_blk);
					blk_free(mtdblk,victim_blk);
					atomic_set(&blk_info[victim_blk].num_valid_pages,0);
		
					test_and_clear_bit(victim_blk,gc_map);
		
					atomic_inc(&bank_info[bank_num].perbank_nfree_blks);
					atomic_sub(mtdblk->pages_per_blk, &bank_info[bank_num].perbank_ndirty_pages);
					
					mtdblk->cur_writing[bank_num].last_gc_blk  = i;
					test_and_clear_bit(bank_num,gc_bankbitmap);
					atomic_inc(&num_gcollected);
					
				
				#ifdef GC_DEBUG_L2	
					printk(KERN_INFO "%x: do_gc_revmapt L%d bank %d ret 1 ",current->pid,level,bank_num);	
				#endif
					return 1;
				}
				else
				{
					
					/* copy and erase */
					victim_blk = i;
					#ifdef GC_DEBUG_L1	
					printk(KERN_INFO "%x: do_gc_revmapt L%d cp %ld pages and er blk %d",current->pid,level,(blk_info[victim_blk].num_valid_pages.counter), victim_blk);
					#endif
					
					
				//	if(test_and_set_bit(victim_blk,gc_map))
				//	{
						/* already gc is going on in this block */
				//		continue;
						//goto search_gc_blk;
				//	}
					mtd = mtdblk->mbd.mtd;
				
					rd_buf = vmalloc(mtd->writesize);
					if (!rd_buf)
					{
						printk(KERN_INFO "myftl: vmalloc fail");
						BUG();
						//return -EINTR;
					}
					oob_buf = vmalloc(mtd->oobsize);
					if (!oob_buf)
					{
						printk(KERN_INFO "myftl: vmalloc fail");
						BUG();
						//return -EINTR;
					}
					mask = 1;
					loop_count = 0;

					/*
					* what if we endup copying a 
					* page that is invalidated during gc of (victim_blk)
					* and the map table is over written?
					* Expected:
					* in wsect: page is invalidated and then maptable is changed
					*/
					bitmap_copy(&vpages_map,blk_info[victim_blk].valid_pages_map,64);
					n_valid_pages = 0;
					while(loop_count < mtdblk->pages_per_blk)
					{
						
						if(loop_count > 64)
						{
							printk(KERN_INFO "loopcnt = %d wrong",loop_count);
							BUG();
						}
						
		
						/* vpages map is set at 1 on that bit */
						if(((mask) & vpages_map) == mask)
						{

							old_phy_page = victim_blk*mtdblk->pages_per_blk + loop_count;
			
							ops.mode = MTD_OOB_AUTO;
							ops.datbuf = rd_buf;
							ops.len = mtd->writesize;
							ops.oobbuf = oob_buf;
							ops.ooboffs = 0;	
							ops.ooblen = mtd->oobsize;



							res = mtd->read_oob(mtd,old_phy_page<<mtdblk->pageshift, &ops);
							if(ops.retlen < mtd->writesize)
							{
								printk(KERN_ERR "FTL read failure");
								printk(KERN_ERR " phypage = %ul",old_phy_page);
								BUG();
							}
							oobdata = &oobvalues;
							memcpy(oobdata, oob_buf,sizeof(*oobdata));

							//old_lpn = oobdata->logic_page_num;
							old_lpn = reverse_map_tab[old_phy_page];
							
							if(old_lpn == INVALID_PAGE_NUMBER)
							{
								uint64_t tempmask;
								printk(KERN_INFO "oobdata->logic_page_num INV phy = %ul logic= %ul",old_phy_page,old_lpn);
								printk(KERN_INFO "GC blkno = %d",i);
								printk(KERN_INFO "GC pageno = %d",loop_count);
								printk(KERN_INFO "GC curbank[%d].blk = %d",bank_num,mtdblk->cur_writing[bank_num].blk);
								printk(KERN_INFO "GC validpages = %d",blk_info[i].num_valid_pages.counter);
								
								bitmap_copy(&tempmask, blk_info[i].valid_pages_map,64);
								printk(KERN_INFO "GC validpagemap = %x",tempmask);
								BUG();
							}
							if(old_lpn >= 16777216 || old_lpn < 0)
							{
								uint64_t tempmask;
								printk(KERN_INFO "oobdata->logic_page_num INV phy = %ul logic= %ul",old_phy_page,old_lpn);
								printk(KERN_INFO "GC blkno = %d",i);
								printk(KERN_INFO "GC pageno = %d",loop_count);
								printk(KERN_INFO "GC curbank[%d].blk = %d",bank_num,mtdblk->cur_writing[bank_num].blk);
								printk(KERN_INFO "GC validpages = %d",blk_info[i].num_valid_pages.counter);
								
								bitmap_copy(&tempmask, blk_info[i].valid_pages_map,64);
								printk(KERN_INFO "GC validpagemap = %x",tempmask);
								
								BUG();
							}
							
							
							new_phy_page = get_ppage(mtdblk,bank_num,1);
							
							if(new_phy_page == INVALID_PAGE_NUMBER)
							{	
								printk(KERN_INFO "myftl: ASSERT dogc phyaddr %ul",new_phy_page);
								BUG();
							}
							if(new_phy_page >= 16777216)
							{
								printk(KERN_INFO "myftl: ASSERT dogc   phyaddr %ul",new_phy_page);
								BUG();
							}
							
							if(new_phy_page == INVALID_PAGE_NUMBER)
							{
								printk(KERN_INFO "do_gc: INVALID page returned");
								BUG();
							}
#ifdef GC_DEBUG_L1	
							printk(KERN_INFO "%x: GCcp L%ld from P%ld to P%ld",current->pid,old_lpn,old_phy_page,new_phy_page);
#endif
							ops.mode = MTD_OOB_AUTO;
							ops.ooblen = mtd->oobsize;
							ops.len = mtd->writesize;
							ops.ooboffs = 0;
							ops.datbuf = rd_buf;
							ops.oobbuf = oob_buf;
							res = mtd->write_oob(mtd,new_phy_page<<mtdblk->pageshift, &ops);
							if(ops.retlen != mtd->writesize)
							{

								printk("myftl: gc mtd write fail");
				
								BUG();			
								return -1 ;
							}
							#if defined (BG_C_GC) || defined (BG_UNC_GC)
							atomic_dec(&mtdblk->activity_matrix.num_writes[bank_num]);
							
							#ifdef PLL_GC_DEBUG
							printk(KERN_INFO "%x: [%ld]num_wr-- = %ld",current->pid,bank_num,mtdblk->activity_matrix.num_writes[bank_num].counter);
							#endif
							#endif

							oldblkno = old_phy_page/(mtdblk->pages_per_blk);
							page_in_blk = old_phy_page%(mtdblk->pages_per_blk);
			
							changed_ppn[loop_count] = new_phy_page;
							corresp_lpn[loop_count] = old_lpn;
							/*map_table change*/
							
							
							//down_write(&(map_tabl_lock));
							
							/* this is the scenario when:
							   GC made a copy of our victim blk's  page 'P' to another blk,
							   but in-between 'P' got invalidated by some writesect
							 */
							
												
							if(!(test_bit(loop_count,blk_info[victim_blk].valid_pages_map)))
							{
								
								
								/*map_table[corresp_lpn[loop_count]] = changed_ppn[loop_count];	*/
								while (test_and_set_bit(old_lpn, maptab_bitmap) != 0)
								{
									schedule();
								}
								map_table[old_lpn] =  new_phy_page;
								reverse_map_tab[new_phy_page] = old_lpn;
								reverse_map_tab[old_phy_page] = INVALID_PAGE_NUMBER_32;	
								
								if (test_and_clear_bit(old_lpn, maptab_bitmap) == 0)
								{
									printk(KERN_INFO "mapbitmap cleared wrong");
									BUG();
								}
								n_valid_pages++;
								new_blkno = new_phy_page/(mtdblk->pages_per_blk);
								page_in_blk = new_phy_page%(mtdblk->pages_per_blk);
								/*new_blkno = changed_ppn[loop_count]/(mtdblk->pages_per_blk);*/
								/*page_in_blk = changed_ppn[loop_count]%(mtdblk->pages_per_blk);*/
								test_and_set_bit(page_in_blk,blk_info[new_blkno].valid_pages_map);
							}
							else
							{
								/*
								* invalidating the copied page that is written somewhere
								* just dont set the valid bit
								* and inc the dirty page count for the bank
								*/
								atomic_inc(&bank_info[bank_num].perbank_ndirty_pages);
							}
							//up_write(&(map_tabl_lock));			
			
		
						}
		
						mask = mask <<1;
		
						loop_count++;

					}

					vfree(rd_buf);
					vfree(oob_buf);
			
					
				
					n_dirty_pages = mtdblk->pages_per_blk - n_valid_pages;
					bitmap_zero((blk_info[victim_blk].valid_pages_map),64);
					#ifdef GC_DEBUG_L2	
					printk(KERN_INFO "myftl: do_gc_revmapt copied and now erase %d",victim_blk);
					#endif
					sync_erase(mtdblk,victim_blk);
					blk_free(mtdblk,victim_blk);
					atomic_set(&blk_info[victim_blk].num_valid_pages,0);
	
					test_and_clear_bit(victim_blk,gc_map);
	
					atomic_inc(&bank_info[bank_num].perbank_nfree_blks);
					atomic_sub(n_dirty_pages,&bank_info[bank_num].perbank_ndirty_pages);	
	
					mtdblk->cur_writing[bank_num].last_gc_blk  = i;
	
					test_and_clear_bit(bank_num,gc_bankbitmap);
					atomic_inc(&num_gcollected);
					
					
					
					#ifdef GC_DEBUG_L2
					printk(KERN_INFO "%x: do_gc_revmapt L%d  bank %d ret 1",current->pid,level,bank_num);
					#endif
					return 1;
	
				} /* end else if minvpages not zero */
					
				
				
			} /*end if( blk vpages <= min_vpages) */
		
		} /*	end if( blk notfree && not curwritten &&  not gcollected)*/
		test_and_clear_bit(i,gc_map);
		
	} /* end for(i = start_blk;i < end_blk;i++)*/
	
	mtdblk->cur_writing[bank_num].last_gc_blk  = i;
	if(test_and_clear_bit(bank_num,gc_bankbitmap) == 0)
	{

		printk(KERN_INFO "%x: do_gc_revmapt bank %d alrdy 0",current->pid,bank_num);
		BUG();
	}

#ifdef GC_DEBUG_L2
	printk(KERN_INFO "%x: do_gc_revmapt L%d  bank %d ret 0",current->pid,level,bank_num);
#endif
	return 0;
	
	
}


int do_gc(struct mtdblk_dev *mtdblk,int bank_num,int level)
{
	uint32_t start_blk,end_blk;
	uint32_t min_vpages;
	uint32_t min;
	uint32_t i;
	uint32_t found_fulld_blk;
	uint32_t found = 0;
	uint32_t victim_blk;
	struct mtd_info *mtd;
	int loop_count;
	uint64_t mask;
	struct mtd_oob_ops ops;
	uint8_t *rd_buf,*oob_buf;
	uint32_t old_lpn;
	uint64_t new_phy_page,old_phy_page;
	struct oob_data oobvalues,*oobdata;
	uint32_t changed_ppn[64];
	uint32_t corresp_lpn[64];
	uint32_t n_valid_pages,n_dirty_pages;
	uint32_t new_blkno;
	uint64_t vpages_map;
	int res;
	uint32_t oldblkno,page_in_blk;
	uint32_t iterations;
	int count = 0;
	start_blk = mtdblk->cur_writing[bank_num].first_blk;
	end_blk = mtdblk->cur_writing[bank_num].last_blk;

#ifdef GC_DEBUG_L2	
	printk(KERN_INFO "myftl: do_gc L%d on bank %d enter",level,bank_num);
#endif

	/* one thread does GC on a bank. and after one block is collected return. search only for max ITERATIONS_PER_GC_CALL*/
	if(test_and_set_bit(bank_num,gc_bankbitmap))
	{
					
#ifdef GC_DEBUG_L3
		printk(KERN_INFO "GC already on %d", bank_num);
#endif
		return 0 ;
			
	}
	
	if(level == 0)
	{
		atomic_inc(&num_l0_gcollected);
	}
	else if(level == 1)
	{
		atomic_inc(&num_l1_gcollected);
	}
	else if(level == 2)
	{
		atomic_inc(&num_l2_gcollected);
	}
		

	
	/* victim gc block should not be 
	* 1) a free block
	* 2) a cur writing block
	* 3) should not become a cur writing block when gc is started
	*  if the block is not free, then it wont become a cur writing block after gc is started.
	*  so check only if it is <non-free and non curWriting>
	*/
	min_vpages = mtdblk->gc_thresh[level];
	//min_vpages = mtdblk->blk_info[start_blk].num_valid_pages.counter;
	min = start_blk;
	found_fulld_blk = 0;
	found = 0;
#if 0
#ifdef NON_SCHEDULED
	iterations = mtdblk->blks_per_bank/16+1;
#else
	iterations = mtdblk->blks_per_bank;
	
#endif
	if(iterations > ITERATIONS_PER_GC_CALL)
	{
	iterations = ITERATIONS_PER_GC_CALL;
}
#endif
	
	iterations = ITERATIONS_PER_GC_CALL;
	if(mtdblk->cur_writing[bank_num].last_gc_blk > mtdblk->cur_writing[bank_num].last_blk)
	{
		start_blk =  mtdblk->cur_writing[bank_num].first_blk;
	
	}
	else
	{
		start_blk = mtdblk->cur_writing[bank_num].last_gc_blk;
	}
	for(i = start_blk,count=0 ;count < iterations;i++,count++)
	{
		/* what happens when the block was not free when 
		* the next line is called
		* and when it gets to next-next line
		* it becomes free
		*/
		if(i > mtdblk->cur_writing[bank_num].last_blk)
		{
			i = mtdblk->cur_writing[bank_num].first_blk;
		}
		/* skip the bad block for GC*/
		if(is_block_bad(mtdblk,i))
		{
			continue;
		}
		if((!(blk_isfree(mtdblk,i))) && (mtdblk->cur_writing[bank_num].blk != i) && (!(test_and_set_bit(i,gc_map))))
		{
			
			if(blk_info[i].num_valid_pages.counter <= min_vpages)
			{
				if(blk_info[i].num_valid_pages.counter == 0)
				{
					victim_blk = i;
					/* only erase required */
					/* 2threads should not garbage collect the same block*/
					
					//if(test_and_set_bit(victim_blk,gc_map))
					//{
						/* already gc is going on in this block */
					//	continue;
						//goto search_gc_blk;
					//}
			
#ifdef GC_DEBUG_L1
					printk(KERN_INFO "%x: do_gc: bank %d erase %d",current->pid,bank_num, victim_blk);		
#endif
					sync_erase(mtdblk,victim_blk);
					blk_free(mtdblk,victim_blk);
					atomic_set(&blk_info[victim_blk].num_valid_pages,0);
		
					test_and_clear_bit(victim_blk,gc_map);
		
					atomic_inc(&bank_info[bank_num].perbank_nfree_blks);
					atomic_sub(mtdblk->pages_per_blk, &bank_info[bank_num].perbank_ndirty_pages);
					
					mtdblk->cur_writing[bank_num].last_gc_blk  = i;
					test_and_clear_bit(bank_num,gc_bankbitmap);
					atomic_inc(&num_gcollected);
					atomic_inc(&num_erase_gcollected);
					
				
#ifdef GC_DEBUG_L2	
					printk(KERN_INFO "%x: do_gc L%d bank %d ret 1 ",current->pid,level,bank_num);	
#endif
					return 1;
				}
				else
				{
					atomic_inc(&num_cperase_gcollected);
					/* copy and erase */
					victim_blk = i;
#ifdef GC_DEBUG_L1	
					printk(KERN_INFO "%x: do_gc L%d cp %ld pages and er blk %d",current->pid,level,(blk_info[victim_blk].num_valid_pages.counter), victim_blk);
#endif
					
					
					//if(test_and_set_bit(victim_blk,gc_map))
					//{
						/* already gc is going on in this block */
					//	continue;
						//goto search_gc_blk;
					//}
					mtd = mtdblk->mbd.mtd;
				
					rd_buf = vmalloc(mtd->writesize);
					if (!rd_buf)
					{
						printk(KERN_INFO "myftl: vmalloc fail");
						BUG();
						//return -EINTR;
					}
					oob_buf = vmalloc(mtd->oobsize);
					if (!oob_buf)
					{
						printk(KERN_INFO "myftl: vmalloc fail");
						BUG();
						//return -EINTR;
					}
					mask = 1;
					loop_count = 0;

					/*
					* what if we endup copying a 
					* page that is invalidated during gc of (victim_blk)
					* and the map table is over written?
					* Expected:
					* in wsect: page is invalidated and then maptable is changed
					*/
					bitmap_copy(&vpages_map,blk_info[victim_blk].valid_pages_map,64);
					n_valid_pages = 0;
					while(loop_count < mtdblk->pages_per_blk)
					{
						
						if(loop_count > 64)
						{
							printk(KERN_INFO "loopcnt = %d wrong",loop_count);
							BUG();
						}
						
		
						/* vpages map is set at 1 on that bit */
						if(((mask) & vpages_map) == mask)
						{

							old_phy_page = victim_blk*mtdblk->pages_per_blk + loop_count;
			
							ops.mode = MTD_OOB_AUTO;
							ops.datbuf = rd_buf;
							ops.len = mtd->writesize;
							ops.oobbuf = oob_buf;
							ops.ooboffs = 0;	
							ops.ooblen = mtd->oobsize;



							res = mtd->read_oob(mtd,old_phy_page<<mtdblk->pageshift, &ops);
							if(ops.retlen < mtd->writesize)
							{
								printk(KERN_ERR "FTL read failure");
								printk(KERN_ERR " phypage = %ul",old_phy_page);
								BUG();
							}
							oobdata = &oobvalues;
							memcpy(oobdata, oob_buf,sizeof(*oobdata));

							old_lpn = oobdata->logic_page_num;
							//old_lpn = reverse_map_tab[old_phy_page];
							if(old_lpn == INVALID_PAGE_NUMBER)
							{
								uint64_t tempmask;
								printk(KERN_INFO "oobdata->logic_page_num INV phy = %ul logic= %ul",old_phy_page,old_lpn);
								printk(KERN_INFO "GC blkno = %d",i);
								printk(KERN_INFO "GC pageno = %d",loop_count);
								printk(KERN_INFO "GC curbank[%d].blk = %d",bank_num,mtdblk->cur_writing[bank_num].blk);
								printk(KERN_INFO "GC validpages = %d",blk_info[i].num_valid_pages.counter);
								
								bitmap_copy(&tempmask, blk_info[i].valid_pages_map,64);
								printk(KERN_INFO "GC validpagemap = %x",tempmask);
								BUG();
							}
							if(old_lpn >= 16777216 || old_lpn < 0)
							{
								uint64_t tempmask;
								printk(KERN_INFO "oobdata->logic_page_num INV phy = %ul logic= %ul",old_phy_page,old_lpn);
								printk(KERN_INFO "GC blkno = %d",i);
								printk(KERN_INFO "GC pageno = %d",loop_count);
								printk(KERN_INFO "GC curbank[%d].blk = %d",bank_num,mtdblk->cur_writing[bank_num].blk);
								printk(KERN_INFO "GC validpages = %d",blk_info[i].num_valid_pages.counter);
								
								bitmap_copy(&tempmask, blk_info[i].valid_pages_map,64);
								printk(KERN_INFO "GC validpagemap = %x",tempmask);
								
								BUG();
							}
							new_phy_page = get_ppage(mtdblk,bank_num,1);
							
							if(new_phy_page == INVALID_PAGE_NUMBER)
							{	
								printk(KERN_INFO "myftl: ASSERT dogc phyaddr %ul",new_phy_page);
								
								BUG();
							}
							if(new_phy_page >= 16777216)
							{
								printk(KERN_INFO "myftl: ASSERT dogc   phyaddr %ul",new_phy_page);
								
								BUG();
							}
							
							if(new_phy_page == INVALID_PAGE_NUMBER)
							{
								printk(KERN_INFO "do_gc: INVALID page returned");
								BUG();
							}
#ifdef GC_DEBUG_L1	
							printk(KERN_INFO "%x: GCcp L%ld from P%ld to P%ld",current->pid,old_lpn,old_phy_page,new_phy_page);
#endif
							ops.mode = MTD_OOB_AUTO;
							ops.ooblen = mtd->oobsize;
							ops.len = mtd->writesize;
							ops.ooboffs = 0;
							ops.datbuf = rd_buf;
							ops.oobbuf = oob_buf;
							res = mtd->write_oob(mtd,new_phy_page<<mtdblk->pageshift, &ops);
							if(ops.retlen != mtd->writesize)
							{

								printk("myftl: gc mtd write fail");
				
								BUG();			
								return -1 ;
							}
#if defined (BG_C_GC) || defined (BG_UNC_GC)
							atomic_dec(&mtdblk->activity_matrix.num_writes[bank_num]);
							
#ifdef PLL_GC_DEBUG
							printk(KERN_INFO "%x: [%ld]num_wr-- = %ld",current->pid,bank_num,mtdblk->activity_matrix.num_writes[bank_num].counter);
#endif
#endif

							oldblkno = old_phy_page/(mtdblk->pages_per_blk);
							page_in_blk = old_phy_page%(mtdblk->pages_per_blk);
			
							changed_ppn[loop_count] = new_phy_page;
							corresp_lpn[loop_count] = old_lpn;
							/*map_table change*/
							
							
							//down_write(&(map_tabl_lock));
							
							/* this is the scenario when:
							GC made a copy of our victim blk's  page 'P' to another blk,
							but in-between 'P' got invalidated by some writesect
							*/
							
												
							if(!(test_bit(loop_count,blk_info[victim_blk].valid_pages_map)))
							{
								
								
								/*map_table[corresp_lpn[loop_count]] = changed_ppn[loop_count];	*/
								while (test_and_set_bit(old_lpn, maptab_bitmap) != 0)
								{
									schedule();
								}
								map_table[old_lpn] =  new_phy_page;
								reverse_map_tab[new_phy_page] = old_lpn;
								reverse_map_tab[old_phy_page] = INVALID_PAGE_NUMBER_32;	
								
								if (test_and_clear_bit(old_lpn, maptab_bitmap) == 0)
								{
									printk(KERN_INFO "mapbitmap cleared wrong");
									BUG();
								}
								n_valid_pages++;
								new_blkno = new_phy_page/(mtdblk->pages_per_blk);
								page_in_blk = new_phy_page%(mtdblk->pages_per_blk);
								/*new_blkno = changed_ppn[loop_count]/(mtdblk->pages_per_blk);*/
								/*page_in_blk = changed_ppn[loop_count]%(mtdblk->pages_per_blk);*/
								test_and_set_bit(page_in_blk,blk_info[new_blkno].valid_pages_map);
							}
							else
							{
								/*
								* invalidating the copied page that is written somewhere
								* just dont set the valid bit
								* and inc the dirty page count for the bank
								*/
								atomic_inc(&bank_info[bank_num].perbank_ndirty_pages);
							}
							//up_write(&(map_tabl_lock));			
			
		
						}
		
						mask = mask <<1;
		
						loop_count++;

					}

					vfree(rd_buf);
					vfree(oob_buf);
			
					
				
					n_dirty_pages = mtdblk->pages_per_blk - n_valid_pages;
					bitmap_zero((blk_info[victim_blk].valid_pages_map),64);
#ifdef GC_DEBUG_L2	
					printk(KERN_INFO "myftl: do_gc copied and now erase %d",victim_blk);
#endif
					sync_erase(mtdblk,victim_blk);
					blk_free(mtdblk,victim_blk);
					atomic_set(&blk_info[victim_blk].num_valid_pages,0);
	
					test_and_clear_bit(victim_blk,gc_map);
	
					atomic_inc(&bank_info[bank_num].perbank_nfree_blks);
					atomic_sub(n_dirty_pages,&bank_info[bank_num].perbank_ndirty_pages);	
	
					mtdblk->cur_writing[bank_num].last_gc_blk  = i;
	
					test_and_clear_bit(bank_num,gc_bankbitmap);
					atomic_inc(&num_gcollected);
					
					
					
#ifdef GC_DEBUG_L2
					printk(KERN_INFO "%x: do_gc L%d  bank %d ret 1",current->pid,level,bank_num);
#endif
					return 1;
	
				} /* end else if minvpages not zero */
					
				
				
			} /*end if( blk vpages <= min_vpages) */
		
		} /*	end if( blk notfree && not curwritten &&  not gcollected)*/
		
		test_and_clear_bit(i,gc_map);
		
	} /* end for(i = start_blk;i < end_blk;i++)*/
	
	mtdblk->cur_writing[bank_num].last_gc_blk  = i;
	if(test_and_clear_bit(bank_num,gc_bankbitmap) == 0)
	{
		printk(KERN_INFO "%x: do_gc bank %d alrdy 0",current->pid,bank_num);
		BUG();
	}

#ifdef GC_DEBUG_L2
	printk(KERN_INFO "%x: do_gc L%d  bank %d ret 0",current->pid,level,bank_num);
#endif
	return 0;
	
	
}

#if 0
int do_gc(struct mtdblk_dev *mtdblk,int bank_num,int level)
{
	uint32_t start_blk,end_blk;
	uint32_t min_vpages;
	uint32_t min;
	uint32_t i;
	uint32_t found_fulld_blk;
	uint32_t found = 0;
	uint32_t victim_blk;
	struct mtd_info *mtd;
	int loop_count;
	uint64_t mask;
	struct mtd_oob_ops ops;
	uint8_t *rd_buf,*oob_buf;
	uint32_t old_lpn;
	uint32_t new_phy_page,old_phy_page;
	struct oob_data oobvalues,*oobdata;
	uint32_t changed_ppn[64];
	uint32_t corresp_lpn[64];
	uint32_t n_valid_pages,n_dirty_pages;
	uint32_t new_blkno;
	uint64_t vpages_map;
	int res;
	uint32_t oldblkno,page_in_blk;
	uint32_t iterations;
	int count = 0;
	start_blk = mtdblk->cur_writing[bank_num].first_blk;
	end_blk = mtdblk->cur_writing[bank_num].last_blk;
#if 0
	printk(KERN_INFO "myftl: try do_gc_revmapt LEVEL%d on bank %d",level,bank_num);
#endif

	/* one thread does GC on a bank. and after one block is collected return. search only for max ITERATIONS_PER_GC_CALL*/
	if(test_and_set_bit(bank_num,gc_bankbitmap))
	{
		return ;
			
	}
	printk(KERN_INFO "myftl: do_gc LEVEL%d on bank %d",level,bank_num);
	/* victim gc block should not be 
	* 1) a free block
	* 2) a cur writing block
	* 3) should not become a cur writing block when gc is started
	*  if the block is not free, then it wont become a cur writing block after gc is started.
	*  so check only if it is <non-free and non curWriting>
	*/
	min_vpages = mtdblk->gc_thresh[level];
	//min_vpages = mtdblk->blk_info[start_blk].num_valid_pages.counter;
	min = start_blk;
	found_fulld_blk = 0;
	found = 0;
	
	iterations = mtdblk->blks_per_bank/16+1;
	if(iterations > ITERATIONS_PER_GC_CALL)
	{
		iterations = ITERATIONS_PER_GC_CALL;
	}
	if(mtdblk->cur_writing[bank_num].last_gc_blk > mtdblk->cur_writing[bank_num].last_blk)
	{
		start_blk =  mtdblk->cur_writing[bank_num].first_blk;
	
	}
	else
	{
		start_blk = mtdblk->cur_writing[bank_num].last_gc_blk;
	}
	for(i = start_blk,count=0 ;count < iterations;i++,count++)
	{
		/* what happens when the block was not free when 
		* the next line is called
		* and when it gets to next-next line
		* it becomes free
		*/
		if(i > mtdblk->cur_writing[bank_num].last_blk)
		{
			i = mtdblk->cur_writing[bank_num].first_blk;
		}
		/* skip the bad block for GC*/
		if(is_block_bad(mtdblk,i))
		{
			continue;
		}
		if((!(blk_isfree(mtdblk,i))) && (mtdblk->cur_writing[bank_num].blk != i) && (!(test_bit(i,gc_map))))
		{
			if(blk_info[i].num_valid_pages.counter <= min_vpages)
			{
				if(blk_info[i].num_valid_pages.counter == 0)
				{
					victim_blk = i;
					/* only erase required */
					/* 2threads should not garbage collect the same block*/
					if(test_and_set_bit(victim_blk,gc_map))
					{
						/* already gc is going on in this block */
						continue;
						//goto search_gc_blk;
					}
					printk(KERN_INFO "myftl: do_gc erase %d",victim_blk);
					sync_erase(mtdblk,victim_blk);
					blk_free(mtdblk,victim_blk);
					atomic_set(&blk_info[victim_blk].num_valid_pages,0);
		
					test_and_clear_bit(victim_blk,gc_map);
		
					atomic_inc(&bank_info[bank_num].perbank_nfree_blks);
					atomic_sub(mtdblk->pages_per_blk, &bank_info[bank_num].perbank_ndirty_pages);
					
					mtdblk->cur_writing[bank_num].last_gc_blk  = i;
					test_and_clear_bit(bank_num,gc_bankbitmap);
					atomic_inc(&num_gcollected);
					return;
				}
				else
				{
					
					/* copy and erase */
					victim_blk = i;
					
					printk(KERN_INFO "myftl: do_gc copy %d pages and erase %d",(blk_info[victim_blk].num_valid_pages.counter), victim_blk);
					
					if(test_and_set_bit(victim_blk,gc_map))
					{
						/* already gc is going on in this block */
						continue;
						//goto search_gc_blk;
					}
					mtd = mtdblk->mbd.mtd;
				
					rd_buf = vmalloc(mtd->writesize);
					if (!rd_buf)
					{
						printk(KERN_INFO "myftl: vmalloc fail");
						BUG();
						//return -EINTR;
					}
					oob_buf = vmalloc(mtd->oobsize);
					if (!oob_buf)
					{
						printk(KERN_INFO "myftl: vmalloc fail");
						BUG();
						//return -EINTR;
					}
					mask = 1;
					loop_count = 0;

					/*
					* what if we endup copying a 
					* page that is invalidated during gc of (victim_blk)
					* and the map table is over written?
					* Expected:
					* in wsect: page is invalidated and then maptable is changed
					*/
					bitmap_copy(&vpages_map,blk_info[victim_blk].valid_pages_map,64);
					n_valid_pages = 0;
					while(loop_count < mtdblk->pages_per_blk)
					{
		
						/* vpages map is set at 1 on that bit */
						if(((mask) & vpages_map) == mask)
						{

							old_phy_page = victim_blk*mtdblk->pages_per_blk + loop_count;
			
							ops.mode = MTD_OOB_AUTO;
							ops.datbuf = rd_buf;
							ops.len = mtd->writesize;
							ops.oobbuf = oob_buf;
							ops.ooboffs = 0;	
							ops.ooblen = mtd->oobsize;



							res = mtd->read_oob(mtd,old_phy_page<<mtdblk->pageshift, &ops);
							if(ops.retlen < mtd->writesize)
							{
								printk(KERN_ERR "FTL read failure");
								printk(KERN_ERR " phypage = %ul",old_phy_page);
								BUG();
							}
							oobdata = &oobvalues;
							memcpy(oobdata, oob_buf,sizeof(*oobdata));

							//old_lpn = oobdata->logic_page_num;
							old_lpn = reverse_map_tab[old_phy_page];
							if(old_lpn == INVALID_PAGE_NUMBER)
							{
								printk(KERN_INFO "rev map tab INV phy = %ul logic= %ul",old_phy_page,old_lpn);
								BUG();
							}
							new_phy_page = get_ppage(mtdblk,bank_num,1);
							
							
							if(new_phy_page == INVALID_PAGE_NUMBER)
							{
								printk(KERN_INFO "do_gc: INVALID page returned");
								BUG();
							}
							ops.mode = MTD_OOB_AUTO;
							ops.ooblen = mtd->oobsize;
							ops.len = mtd->writesize;
							ops.ooboffs = 0;
							ops.datbuf = rd_buf;
							ops.oobbuf = oob_buf;
							res = mtd->write_oob(mtd,new_phy_page<<mtdblk->pageshift, &ops);
							if(ops.retlen != mtd->writesize)
							{

								printk("myftl: gc mtd write fail");
				
								BUG();			
								return ;
							}
							#if defined (BG_C_GC) || defined (BG_UNC_GC)
							atomic_dec(&mtdblk->activity_matrix.num_writes[bank_num]);
							
							#ifdef PLL_GC_DEBUG
							printk(KERN_INFO "%x: [%ld]num_wr-- = %ld",current->pid,bank_num,mtdblk->activity_matrix.num_writes[bank_num].counter);
							#endif
							#endif

							oldblkno = old_phy_page/(mtdblk->pages_per_blk);
							page_in_blk = old_phy_page%(mtdblk->pages_per_blk);
			
							changed_ppn[loop_count] = new_phy_page;
							corresp_lpn[loop_count] = old_lpn;
							/*map_table change*/
							
							
							down_write(&(map_tabl_lock));
							
							/* this is the scenario when:
							   GC made a copy of our victim blk's  page 'P' to another blk,
							   but in-between 'P' got invalidated by some writesect
							 */
							
												
							if(!(test_bit(loop_count,blk_info[victim_blk].valid_pages_map)))
							{
								
								
								/*map_table[corresp_lpn[loop_count]] = changed_ppn[loop_count];	*/
								map_table[old_lpn] =  new_phy_page;
								reverse_map_tab[new_phy_page] = old_lpn;
								reverse_map_tab[old_phy_page] = INVALID_PAGE_NUMBER;	
								
								n_valid_pages++;
								new_blkno = new_phy_page/(mtdblk->pages_per_blk);
								page_in_blk = new_phy_page%(mtdblk->pages_per_blk);
								/*new_blkno = changed_ppn[loop_count]/(mtdblk->pages_per_blk);*/
								/*page_in_blk = changed_ppn[loop_count]%(mtdblk->pages_per_blk);*/
								test_and_set_bit(page_in_blk,blk_info[new_blkno].valid_pages_map);
							}
							else
							{
								/*
								* invalidating the copied page that is written somewhere
								* just dont set the valid bit
								* and inc the dirty page count for the bank
								*/
								atomic_inc(&bank_info[bank_num].perbank_ndirty_pages);
							}
							up_write(&(map_tabl_lock));			
			
		
						}
		
						mask = mask <<1;
		
						loop_count++;

					}

					vfree(rd_buf);
					vfree(oob_buf);
			
					
				
					n_dirty_pages = mtdblk->pages_per_blk - n_valid_pages;
					bitmap_zero((blk_info[victim_blk].valid_pages_map),64);
					printk(KERN_INFO "myftl: do_gc copied and now erase %d",victim_blk);
					sync_erase(mtdblk,victim_blk);
					blk_free(mtdblk,victim_blk);
					atomic_set(&blk_info[victim_blk].num_valid_pages,0);
	
					test_and_clear_bit(victim_blk,gc_map);
	
					atomic_inc(&bank_info[bank_num].perbank_nfree_blks);
					atomic_sub(n_dirty_pages,&bank_info[bank_num].perbank_ndirty_pages);	
	
					mtdblk->cur_writing[bank_num].last_gc_blk  = i;
	
					test_and_clear_bit(bank_num,gc_bankbitmap);
					atomic_inc(&num_gcollected);
					return;
	
				} /* end else if minvpages not zero */
					
				
				
			} /*end if( blk vpages <= min_vpages) */
		
		} /*	end if( blk notfree && not curwritten &&  not gcollected)*/
		
	} /* end for(i = start_blk;i < end_blk;i++)*/
	
	mtdblk->cur_writing[bank_num].last_gc_blk  = i;
	test_and_clear_bit(bank_num,gc_bankbitmap);
	return;
	
	
}
#endif

/* acquires one free block */
#if 0
void do_gc(struct mtdblk_dev *mtdblk,int bank_num,int level)
{
	uint32_t start_blk,end_blk;
	uint32_t min_vpages;
	uint32_t min;
	uint32_t i;
	uint32_t found_fulld_blk;
	uint32_t found = 0;
	uint32_t victim_blk;
	struct mtd_info *mtd;
	int loop_count;
	uint64_t mask;
	struct mtd_oob_ops ops;
	uint8_t *rd_buf,*oob_buf;
	uint32_t old_lpn;
	uint32_t new_phy_page,old_phy_page;
	struct oob_data oobvalues,*oobdata;
	uint32_t changed_ppn[64];
	uint32_t corresp_lpn[64];
	uint32_t n_valid_pages,n_dirty_pages;
	uint32_t new_blkno;
	uint64_t vpages_map;
	int res;
	uint32_t oldblkno,page_in_blk;
	uint32_t iterations;
 	int count = 0;
	
	start_blk = mtdblk->cur_writing[bank_num].first_blk;
	end_blk = mtdblk->cur_writing[bank_num].last_blk;
#if 1
	
	printk(KERN_INFO "myftl: do_gc LEVEL%d on bank %d",level,bank_num);

	/* victim gc block should not be 
	 * 1) a free block
	 * 2) a cur writing block
	 * 3) should not become a cur writing block when gc is started
	 *  if the block is not free, then it wont become a cur writing block after gc is started.
	 *  so check only if it is <non-free and non curWriting>
	 */
	min_vpages = mtdblk->gc_thresh[level];
	//min_vpages = mtdblk->blk_info[start_blk].num_valid_pages.counter;
	min = start_blk;
	found_fulld_blk = 0;
	found = 0;
	
	iterations = mtdblk->blks_per_bank/16+1;
	if(iterations > ITERATIONS_PER_GC_CALL)
	{
		iterations = ITERATIONS_PER_GC_CALL;
	}
	
	if(mtdblk->cur_writing[bank_num].last_gc_blk > mtdblk->cur_writing[bank_num].last_blk)
	{
		start_blk =  mtdblk->cur_writing[bank_num].first_blk;
	
	}
	else
	{
		start_blk = mtdblk->cur_writing[bank_num].last_gc_blk;
 	}
	
	for(i = start_blk,count=0 ;count < iterations;i++,count++)
	{
		/* what happens when the block was not free when 
		* the next line is called
		* and when it gets to next-next line
		* it becomes free
		*/
		if(i > end_blk)
		{
			i = start_blk;
 		}
		/* skip the bad block for GC*/
		if(is_block_bad(mtdblk,i)){
			 continue;
		}
		if((!(blk_isfree(mtdblk,i))) && (mtdblk->cur_writing[bank_num].blk != i) && (!(test_bit(i,gc_map))))
		{
			if(blk_info[i].num_valid_pages.counter <= min_vpages)
			{
				if(blk_info[i].num_valid_pages.counter == 0)
				{
					victim_blk = i;
					/* only erase required */
					/* 2threads should not garbage collect the same block*/
					if(test_and_set_bit(victim_blk,gc_map))
					{
						/* already gc is going on in this block */
						continue;
						//goto search_gc_blk;
					}
					printk(KERN_INFO "myftl: do_gc erase %d",victim_blk);
					async_erase(mtdblk,victim_blk);
					blk_free(mtdblk,victim_blk);
					atomic_set(&blk_info[victim_blk].num_valid_pages,0);
		
					test_and_clear_bit(victim_blk,gc_map);
		
					atomic_inc(&bank_info[bank_num].perbank_nfree_blks);
					atomic_sub(mtdblk->pages_per_blk, &bank_info[bank_num].perbank_ndirty_pages);
				}
				else
				{
					
					/* copy and erase */
					victim_blk = i;
					printk(KERN_INFO "myftl: do_gc copy and erase %d",victim_blk);
					if(test_and_set_bit(victim_blk,gc_map))
					{
						/* already gc is going on in this block */
						continue;
						//goto search_gc_blk;
					}
					mtd = mtdblk->mbd.mtd;
				
					rd_buf = vmalloc(mtd->writesize);
					if (!rd_buf)
					{
						printk(KERN_INFO "myftl: vmalloc fail");
						BUG();
						//return -EINTR;
					}
					oob_buf = vmalloc(mtd->oobsize);
					if (!oob_buf)
					{
						printk(KERN_INFO "myftl: vmalloc fail");
						BUG();
						//return -EINTR;
					}
					mask = 1;
					loop_count = 0;

					/*
					* what if we endup copying a 
					* page that is invalidated during gc of (victim_blk)
					* and the map table is over written
					*/
					bitmap_copy(&vpages_map,blk_info[victim_blk].valid_pages_map,64);
					n_valid_pages = 0;
					while(loop_count < mtdblk->pages_per_blk)
					{
		
						/* vpages map is set at 0 on that bit */
						if(((mask) & vpages_map) == mask)
						{

							old_phy_page = victim_blk*mtdblk->pages_per_blk + loop_count;
			
							ops.mode = MTD_OOB_AUTO;
							ops.datbuf = rd_buf;
							ops.len = mtd->writesize;
							ops.oobbuf = oob_buf;
							ops.ooboffs = 0;	
							ops.ooblen = mtd->oobsize;



							res = mtd->read_oob(mtd,old_phy_page<<mtdblk->pageshift, &ops);
							if(ops.retlen < mtd->writesize)
							{
								printk(KERN_ERR "FTL read failure");
								printk(KERN_ERR " phypage = %ld",old_phy_page);
								BUG();
							}
							oobdata = &oobvalues;
							memcpy(oobdata, oob_buf,sizeof(*oobdata));

							old_lpn = oobdata->logic_page_num;
							new_phy_page = get_ppage(mtdblk,bank_num,1);	
							if(new_phy_page == INVALID_PAGE_NUMBER)
							{
								printk(KERN_INFO "do_gc: INVALID page returned");
								BUG();
							}
							ops.mode = MTD_OOB_AUTO;
							ops.ooblen = mtd->oobsize;
							ops.len = mtd->writesize;
							ops.ooboffs = 0;
							ops.datbuf = rd_buf;
							ops.oobbuf = oob_buf;
							res = mtd->write_oob(mtd,new_phy_page<<mtdblk->pageshift, &ops);
							if(ops.retlen != mtd->writesize)
							{

								printk("myftl: gc mtd write fail");
				
								BUG();			
								return ;
							}
			

							oldblkno = old_phy_page/(mtdblk->pages_per_blk);
							page_in_blk = old_phy_page%(mtdblk->pages_per_blk);
			
							changed_ppn[loop_count] = new_phy_page;
							corresp_lpn[loop_count] = old_lpn;
							/*map_table change*/
							
							
							down_write(&(map_tabl_lock));
												
							if(!(test_bit(loop_count,blk_info[victim_blk].valid_pages_map)))
							{
								/*map_table[old_lpn] =  new_phy_page */
								map_table[corresp_lpn[loop_count]] = changed_ppn[loop_count];	
								n_valid_pages++;
								new_blkno = changed_ppn[loop_count]/(mtdblk->pages_per_blk);
								page_in_blk = changed_ppn[loop_count]%(mtdblk->pages_per_blk);
								test_and_set_bit(page_in_blk,blk_info[new_blkno].valid_pages_map);
							}
							else
							{
								/*
								 * invalidating the copied page that is written somewhere
								 * just dont set the valid bit
								 * and inc the dirty page count for the bank
								 */
								atomic_inc(&bank_info[bank_num].perbank_ndirty_pages);
							}
							up_write(&(map_tabl_lock));			
							
		
						}
		
						mask = mask <<1;
		
						loop_count++;

					}

					vfree(rd_buf);
					vfree(oob_buf);
			
					
				
					n_dirty_pages = mtdblk->pages_per_blk - n_valid_pages;
					bitmap_zero((blk_info[victim_blk].valid_pages_map),64);
					async_erase(mtdblk,victim_blk);
					blk_free(mtdblk,victim_blk);
					atomic_set(&blk_info[victim_blk].num_valid_pages,0);
	
					test_and_clear_bit(victim_blk,gc_map);
	
					atomic_inc(&bank_info[bank_num].perbank_nfree_blks);
					atomic_sub(n_dirty_pages,&bank_info[bank_num].perbank_ndirty_pages);	
	
	
				} /* end else if minvpages not zero */
					
				
				
			} /*end if( blk vpages <= min_vpages) */
		
		} /*	end if( blk notfree && not curwritten &&  not gcollected)*/
	} /* end for(i = start_blk;i < end_blk;i++)*/
	
	
	

			
#endif
	
	return;
}
#endif

#if 0
 /* bdi_writeback_task():
  * Handle writeback of dirty data for the device backed by this bdi. Also
  * wakes up periodically and does kupdated style flushing.
 */    
 int bdi_writeback_task(struct bdi_writeback *wb)
{
	unsigned long last_active = jiffies;
	unsigned long wait_jiffies = -1UL;
	long pages_written;
  
	while (!kthread_should_stop()) {
		pages_written = wb_do_writeback(wb, 0);
         
		if (pages_written)
			last_active = jiffies;
		else if (wait_jiffies != -1UL) {
			unsigned long max_idle;
  
                          /*
			* Longest period of inactivity that we tolerate. If we
			* see dirty data again later, the task will get
			* recreated automatically.
			  */
			max_idle = max(5UL * 60 * HZ, wait_jiffies);
			if (time_after(jiffies, max_idle + last_active))
				break;
		}
  
		wait_jiffies = msecs_to_jiffies(dirty_writeback_interval * 10);
		schedule_timeout_interruptible(wait_jiffies);
		try_to_freeze();
	}
  
	return 0;
}              

#endif


#define IN_USE 1
#define NOT_IN_USE 0

/* in milliseconds*/
#define PREFETCH_BUF_HOLDTIME (60000)
//#define PREFETCH_DEBUG	
//#define BUFFLUSHD 1
//#define PER_BUFFER_FLUSH

struct prefetch_thread_info
{
	uint32_t lpn;
	struct mtdblk_dev *mtdblk;
	uint32_t bufidx;
	
};

void give_up_prefetch_buf(struct work_struct *arg)
{
	int buf_num;
	uint8_t *prefetch_buf_ptr;
	struct cache_num_node *node;
	
	struct buf_sched_struct *passed_pfetch_buf =  container_of(arg,struct buf_sched_struct,workq);
	
	buf_num = passed_pfetch_buf->buf_num;
	

	if(buf_num <0 || buf_num > MAX_FTL_PREFETCH_BUFS)
	{
		printk(KERN_ERR "prefetch bufnum = %d wrong in giveup func",buf_num);
	}
	
	if(jiffies   <  p_fetch_info[buf_num].last_touch + msecs_to_jiffies(PREFETCH_BUF_HOLDTIME))
	{
	//	printk(KERN_INFO "%lld cant giveup buf = %d",jiffies,buf_num);		
		schedule_delayed_work(&prefetch_buf_sched[buf_num].workq, msecs_to_jiffies(PREFETCH_BUF_HOLDTIME));
		
	}
	else
	{
		down_write(&(pfetch_buf_lock[buf_num]));
		test_and_clear_bit(pfetch_lookup_tab[buf_num],page_incache_bitmap);
		pfetch_lookup_tab[buf_num] = INVALID_PAGE_NUMBER;
		prefetch_buf_ptr = prefetch_buf[buf_num];
			
		node = kmem_cache_alloc(qnode_cache, GFP_KERNEL);
		//node = kmalloc(sizeof(*node),GFP_KERNEL);
		if (!node)
		{
			printk(KERN_INFO "kmalloc fail \n");	
			BUG();
		}
		node->value = buf_num;
		lfq_node_init_rcu(&node->list);
		rcu_read_lock();
		lockfree_enqueue(&pfetch_bufsq, &node->list);
		rcu_read_unlock();
		
		//prefetch_buf[buf_num] = NULL;
		//vfree(prefetch_buf_ptr);
		up_write(&(pfetch_buf_lock[buf_num]));
		
	}
	
	
}


void buf_flush_thread(struct mtdblk_dev *mtdblk)
{
	unsigned long wait_jiffies = -1UL;
	int buf_num;
	struct cache_num_node *node;
	
	while (!kthread_should_stop()) {
		for(buf_num = 0; buf_num < MAX_FTL_PREFETCH_BUFS;buf_num++)
		{
			if(p_fetch_info[buf_num].state == IN_USE)
			{
				if(jiffies   <  p_fetch_info[buf_num].last_touch + msecs_to_jiffies(PREFETCH_BUF_HOLDTIME))
				{
					/* never mind*/
				}	
				else
				{
					down_write(&(pfetch_buf_lock[buf_num]));
					
					test_and_clear_bit(pfetch_lookup_tab[buf_num],page_incache_bitmap);
					p_fetch_info[buf_num].read_mask =0ULL;
					pfetch_lookup_tab[buf_num] = INVALID_PAGE_NUMBER_32;
					p_fetch_info[buf_num].state = NOT_IN_USE;
					//prefetch_buf[buf_num] = NULL;
					//vfree(prefetch_buf_ptr);
					
					int idx;
					idx = p_fetch_info[buf_num].index_inlist;
					if(idx == -1)
					{
						printk(KERN_INFO "pfetch info idx = -1");
						BUG();
					}
					mtdblk->accesslist[idx] = INVALID_PAGE_NUMBER_32;
					p_fetch_info[buf_num].index_inlist = -1;
					
					up_write(&(pfetch_buf_lock[buf_num]));
					
					node = kmem_cache_alloc(qnode_cache, GFP_KERNEL);
					//node = kmalloc(sizeof(*node),GFP_KERNEL);
					if (!node)
					{
						printk(KERN_INFO "kmalloc fail \n");	
						BUG();
					}
					node->value = buf_num;
					lfq_node_init_rcu(&node->list);
					rcu_read_lock();
					lockfree_enqueue(&pfetch_bufsq, &node->list);
					rcu_read_unlock();
					
					pref_buf_count++;
					printk(KERN_INFO "thrdenq buf count = %d",pref_buf_count);
					
					
						
					
				}
			}
		
		}
		wait_jiffies = msecs_to_jiffies(PREFETCH_BUF_HOLDTIME);
		schedule_timeout_interruptible(wait_jiffies);
	}
	
}


#if 0
void check_and_dopfetch_thrd(void *arg)
{
	int banknum;
	struct mtdblk_dev *mtdblk = ((struct mtdblk_dev *)arg);
	if(prefetch_on == 1)
	{
		read the cur window;
		the request x;
		if x is prefetchable
		
		int OP;
		int cost;
		OP = PFTCH_OP ;
				

		cost = mtdblk->activity_matrix.gc_goingon[next_lpn_bank].counter*cost_op[OP][GC_OP] + mtdblk->activity_matrix.num_writes[next_lpn_bank].counter*cost_op[OP][WR_OP] + mtdblk->activity_matrix.num_reads_pref[next_lpn_bank].counter*cost_op[OP][PFTCH_OP] + mtdblk->activity_matrix.num_reads[next_lpn_bank].counter*cost_op[OP][RD_OP];
				
		if(cost < cost_thresh[next_lpn_bank][OP])
		{
			okay to prefetch;
		}
		
		rcu_read_lock();
		qnode = lockfree_dequeue(&pfetch_bufsq);
		node = container_of(qnode, struct cache_num_node, list);
		rcu_read_unlock();
		
		if(node == NULL)
		{
#if 0
			printk(KERN_ERR "myftl: no prefetch buffer");
#endif
			return;
		}
		
		bufidx = node->value;
					
#ifdef PREFETCH_DEBUG
		printk(KERN_ERR "myftl: can prefetch buffer = %d lpn %lld",bufidx,lpn+i);
#endif
		call_rcu(&node->rcu, free_cache_num_node);	
		prefetch_count++;
		
		while (test_and_set_bit(logic_page_num, maptab_bitmap) != 0)
		{
			schedule();
		}
		phy_page_offs = map_table[logic_page_num];
	//up_read(&(map_tabl_lock));
		if (test_and_clear_bit(logic_page_num, maptab_bitmap) == 0)
		{
			printk(KERN_INFO "mapbitmap cleared wrong");
			BUG();
		}
	
#ifdef USE_VIRGO_RESTRICTED
		num_pages_perbank  =  4096*mtdblk->pages_per_blk;
		bank_num = phy_page_offs/(num_pages_perbank);
#else
		num_pages_perbank  =  mtdblk->blks_per_bank*mtdblk->pages_per_blk;
		bank_num = phy_page_offs/(num_pages_perbank);
#endif

#ifdef EIGHT_BANK_FLASH
		bank_num = bank_num/8;
#endif
	
#ifdef PREFETCH_DEBUG	
		printk(KERN_INFO "myftl: prefetch buffer = %d ",bufidx);
#endif
		int retlen = 0;
		down_write(&(pfetch_buf_lock[bufidx]));
#ifdef PREFETCH_DEBUG
		printk(KERN_INFO "lpn %lld ppn = %lld",logic_page_num,phy_page_offs);
#endif
		mtd->read(mtd,((phy_page_offs<<mtdblk->pageshift)),mtd->writesize, &retlen, prefetch_buf[bufidx]);	
		if(retlen != mtd->writesize)
		{
			printk(KERN_ERR "FTL read failure");
			printk(KERN_ERR " phypage = %lld ",phy_page_offs);

			printk(KERN_ERR " logpage = %lld",logic_page_num);
		}
		pfetch_lookup_tab[bufidx] = logic_page_num;
	
		p_fetch_info[bufidx].last_touch = jiffies;
		p_fetch_info[bufidx].read_mask =0ULL;
		p_fetch_info[bufidx].state = IN_USE;
		up_write(&(pfetch_buf_lock[bufidx]));
		atomic_dec(&mtdblk->activity_matrix.num_reads_pref[bank_num]);	
		
	}
}


#endif
							
							
void prefetch_thread(void *arg)
{
	uint32_t logic_page_num = ((struct prefetch_thread_info *)arg)->lpn;
	struct mtdblk_dev *mtdblk = ((struct prefetch_thread_info *)arg)->mtdblk;
	struct mtd_info *mtd = mtdblk->mbd.mtd;
	
	uint64_t phy_page_offs;
	uint32_t num_pages_perbank;
	uint32_t bank_num;
	uint32_t bufidx = ((struct prefetch_thread_info *)arg)->bufidx;
	size_t retlen = 0;
	//down_read(&(map_tabl_lock));
	while (test_and_set_bit(logic_page_num, maptab_bitmap) != 0)
	{
		schedule();
	}
	phy_page_offs = map_table[logic_page_num];
	//up_read(&(map_tabl_lock));
	if (test_and_clear_bit(logic_page_num, maptab_bitmap) == 0)
	{
		printk(KERN_INFO "mapbitmap cleared wrong");
		BUG();
	}
#if 0
#ifdef USE_VIRGO_RESTRICTED
	num_pages_perbank  =  4096*mtdblk->pages_per_blk;
	bank_num = phy_page_offs/(num_pages_perbank);
#else
	num_pages_perbank  =  mtdblk->blks_per_bank*mtdblk->pages_per_blk;
	bank_num = phy_page_offs/(num_pages_perbank);
#endif
#endif
	bank_num = phy_page_offs/(mtdblk->hwblks_per_bank*mtdblk->pages_per_blk);

#ifdef EIGHT_BANK_FLASH
	bank_num = bank_num/8;
#endif
	
#ifdef PREFETCH_DEBUG	
	printk(KERN_INFO "myftl: prefetch buffer = %d ",bufidx);
#endif
	
	down_write(&(pfetch_buf_lock[bufidx]));
#ifdef PREFETCH_DEBUG
	printk(KERN_INFO "lpn %lld ppn = %lld",logic_page_num,phy_page_offs);
#endif
	retlen = 0;
	mtd->read(mtd,((phy_page_offs<<mtdblk->pageshift)),mtd->writesize, &retlen, prefetch_buf[bufidx]);	
	if(retlen != mtd->writesize)
	{
		printk(KERN_ERR "FTL read failure");
		printk(KERN_ERR " phypage = %lld ",phy_page_offs);

		printk(KERN_ERR " logpage = %ul",logic_page_num);
	}
	pfetch_lookup_tab[bufidx] = logic_page_num;
	
	p_fetch_info[bufidx].last_touch = jiffies;
	p_fetch_info[bufidx].read_mask =0ULL;
	p_fetch_info[bufidx].state = IN_USE;
	
	
	up_write(&(pfetch_buf_lock[bufidx]));
	
	atomic_dec(&mtdblk->activity_matrix.num_reads_pref[bank_num]);	
	
#ifdef PER_BUFFER_FLUSH
	schedule_delayed_work(&prefetch_buf_sched[bufidx].workq, msecs_to_jiffies(PREFETCH_BUF_HOLDTIME));
#endif
	kfree(arg);
	atomic_dec(&num_prefetch_threads);
}


int prefetch_count=0;

void check_can_prefetch(struct mtdblk_dev *mtdblk,uint32_t lpn)
{

	uint32_t ppn;
	int i;
	struct task_struct *prefetch_thrd_struct;
	int bank_num;
	struct prefetch_thread_info *prefetch_thread_arg;
	uint32_t next_lpn_bank;
	uint32_t num_pages_perbank;
	uint32_t bufidx;
	struct lfq_node_rcu *qnode;
	struct cache_num_node *node;
	
	//down_read(&(map_tabl_lock));
	while (test_and_set_bit(lpn, maptab_bitmap) != 0)
	{
		schedule();
	}
	
	ppn = map_table[lpn];
	if (test_and_clear_bit(lpn, maptab_bitmap) == 0)
	{
		printk(KERN_INFO "mapbitmap cleared wrong");
		BUG();
	}
	//down_read(&(map_tabl_lock));
#if 0
	#ifdef USE_VIRGO_RESTRICTED
	num_pages_perbank  =  4096*mtdblk->pages_per_blk;
	bank_num = ppn/(num_pages_perbank);
	#else
	num_pages_perbank  =  mtdblk->blks_per_bank*mtdblk->pages_per_blk;
	bank_num = ppn/(num_pages_perbank);
	#endif
#endif
	
	bank_num = ppn/(mtdblk->hwblks_per_bank*mtdblk->pages_per_blk);
	
#ifdef EIGHT_BANK_FLASH
	bank_num = bank_num/8;
#endif
	
	if(ppn < 0 || ppn >= MAP_TABLE_SIZE)
	{
		return;
	}
	

	if( bank_num  < 0 || bank_num > 64)
	{
		printk(KERN_ERR "bank wrong val = %d",bank_num);
		BUG();
	}
	
	
	if(num_prefetch_threads.counter < 2)
	{
		for(i = 32; i < 64;i++)
		{
			if(!(test_bit(lpn+i,page_incache_bitmap)))
			{
			
				
					
				next_lpn_bank = (map_table[lpn+i])/(num_pages_perbank);
				if( next_lpn_bank  < 0 || next_lpn_bank > 64)
				{
					printk(KERN_ERR "next_lpn_bank wrong val = %d",bank_num);
					BUG();
				}
				//if(next_lpn_bank != bank_num)
				//if(mtdblk->activity_matrix.num_reads[next_lpn_bank].counter == 0)
				int OP;
				int cost;
				OP = PFTCH_OP ;
				

				cost = mtdblk->activity_matrix.gc_goingon[next_lpn_bank].counter*cost_op[OP][GC_OP] + mtdblk->activity_matrix.num_writes[next_lpn_bank].counter*cost_op[OP][WR_OP] + mtdblk->activity_matrix.num_reads_pref[next_lpn_bank].counter*cost_op[OP][PFTCH_OP] + mtdblk->activity_matrix.num_reads[next_lpn_bank].counter*cost_op[OP][RD_OP];
				
				if(cost < cost_thresh[next_lpn_bank][OP])
				{
					
					rcu_read_lock();
					qnode = lockfree_dequeue(&pfetch_bufsq);
					node = container_of(qnode, struct cache_num_node, list);
					rcu_read_unlock();
		
					if(node == NULL)
					{
						#if 0
						printk(KERN_ERR "myftl: no prefetch buffer");
						#endif
						return;
					}
		
					bufidx = node->value;
					
					#ifdef PREFETCH_DEBUG
					printk(KERN_ERR "myftl: can prefetch buffer = %d lpn %lld",bufidx,lpn+i);
					#endif
					call_rcu(&node->rcu, free_cache_num_node);	
					prefetch_count++;
					prefetch_thread_arg = kmalloc(sizeof (struct prefetch_thread_info),GFP_KERNEL);
					if(prefetch_thread_arg == NULL)
					{
						printk(KERN_ERR "kmalloc check_can_prefetch fail");
						BUG();
					}
					prefetch_thread_arg->lpn = lpn+i;
					prefetch_thread_arg->bufidx = bufidx;
					prefetch_thread_arg->mtdblk = mtdblk;
					set_bit(lpn+i,page_incache_bitmap);
					atomic_inc(&mtdblk->activity_matrix.num_reads_pref[next_lpn_bank]);	
					
					atomic_inc(&num_prefetch_threads);
					
					prefetch_thrd_struct  = kthread_run(prefetch_thread, (prefetch_thread_arg),
							"prefetchthrd %ul",lpn+i);
	
					if (IS_ERR(prefetch_thrd_struct)) {
						PTR_ERR(prefetch_thrd_struct);
						BUG();
					}
					
					//return;
				}
				
			}
					
		}
	}

}


void prefetch_access_list(struct mtdblk_dev *mtdblk)
{
	
	int i;
	uint32_t lpn_in_bank;

	struct lfq_node_rcu *qnode;
	struct cache_num_node *node;
	uint64_t phy_page_offs;
	uint32_t num_pages_perbank;
	//uint32_t bank_num;
	uint32_t bufidx;
	int num_inv_entries = 0;
	int cur_idx = 0;
	uint32_t logic_page_num;
	struct mtd_info *mtd = mtdblk->mbd.mtd;
	int possible_pref_buf;
	size_t retlen = 0;
	uint64_t logic_ftl_blk;
	
	
	uint64_t mask;
	uint32_t sect_idx;
	uint32_t shift_val;
	
	
	possible_pref_buf = -1;
	printk(KERN_INFO "prefetch_access_list started ");
	
	while (!kthread_should_stop()) 	{
		

	try_next_page:
	while(1)
	{	
		logic_ftl_blk = mtdblk->accesslist[cur_idx];
		if(logic_ftl_blk == INVALID_PAGE_NUMBER_32)
		{
			num_inv_entries++;
			if(num_inv_entries > 2*MAX_ACCESS_LIST)
			{
				/* goto sleep*/
				set_current_state(TASK_INTERRUPTIBLE);
				schedule();
				set_current_state(TASK_RUNNING);
			}
			cur_idx = (cur_idx+1)%MAX_ACCESS_LIST;	
		}
		else
		{
			break;
		} 
		
	}
	num_inv_entries = 0;
	
	logic_page_num = (logic_ftl_blk<<mtdblk->blkshift)>>mtdblk->pageshift;
	
	shift_val = mtdblk->pageshift -mtdblk->blkshift;
	mask = ~(-1UL<<shift_val);
	sect_idx = logic_ftl_blk&mask;
	
	
	if(!(test_bit(logic_ftl_blk,ftlblk_incache_bitmap)))
	{
		
#if 0
			#ifdef USE_VIRGO_RESTRICTED
				num_pages_perbank  =  4096*mtdblk->pages_per_blk;
			#else
				num_pages_perbank  =  mtdblk->blks_per_bank*mtdblk->pages_per_blk;
			#endif
#endif					
			lpn_in_bank = (map_table[logic_page_num])/(mtdblk->hwblks_per_bank*mtdblk->pages_per_blk);
#ifdef EIGHT_BANK_FLASH
			lpn_in_bank = lpn_in_bank/8;
#endif

			if( lpn_in_bank  < 0 || lpn_in_bank > 64)
			{
				printk(KERN_ERR "next_lpn_bank wrong val = %d",lpn_in_bank);
				BUG();
			}
					
			int OP;
			int cost;
			
			OP = PFTCH_OP ;
					
	
			cost = mtdblk->activity_matrix.gc_goingon[lpn_in_bank].counter*cost_op[OP][GC_OP] + mtdblk->activity_matrix.num_writes[lpn_in_bank].counter*cost_op[OP][WR_OP] + mtdblk->activity_matrix.num_reads_pref[lpn_in_bank].counter*cost_op[OP][PFTCH_OP] + mtdblk->activity_matrix.num_reads[lpn_in_bank].counter*cost_op[OP][RD_OP];
					
		//	if(cost < cost_thresh[lpn_in_bank][OP])
			{
		try_get_buf:
				if(possible_pref_buf  == -1)
				{		
					rcu_read_lock();
					qnode = lockfree_dequeue(&pfetch_bufsq);
					node = container_of(qnode, struct cache_num_node, list);
					rcu_read_unlock();
					
				
					if(node == NULL)
					{
		
						schedule();
						//printk(KERN_INFO "no pref buf");
						goto try_get_buf;
					}
				
					pref_buf_count--;
					printk(KERN_INFO "prftch_deq  buf count = %d",pref_buf_count);
					possible_pref_buf = node->value;
					//bufidx = node->value;
							
		
					call_rcu(&node->rcu, free_cache_num_node);	
				
				}
				prefetch_count++;
				/* 
				 * dont try to understand this!!
				 * first i have checked that this logic page num is not in cache
				 * then i got a buffer
				 * but in the meantime another thread started prefetching
				 * so now another check 
				 * and just goto the point where we have a buffer and 
				 * have to try next page
				 */
				if(test_and_set_bit(logic_ftl_blk,ftlblk_incache_bitmap) != 0)
				{
					goto try_next_page;
				}
				
				while(test_and_set_bit(cur_idx, acl_state) != 0)
				{
					schedule();
				}
				if(mtdblk->accesslist[cur_idx] == logic_ftl_blk)
				{
					set_bit(cur_idx,prefetch_onbitmap);
				}
				else
				{
					if (test_and_clear_bit(cur_idx, acl_state) == 0)
					{
						printk(KERN_INFO "acl_state cleared wrong");
						BUG();
					}
					goto try_next_page;
					
				}
				
				
				if (test_and_clear_bit(cur_idx, acl_state) == 0)
				{
					printk(KERN_INFO "acl_state cleared wrong");
					BUG();
				}
				printk(KERN_INFO "prftch[%d]  logicfblk = %d",cur_idx,logic_ftl_blk);
				bufidx = possible_pref_buf;
				possible_pref_buf = -1;
				
				atomic_inc(&mtdblk->activity_matrix.num_reads_pref[lpn_in_bank]);	
		
		//down_read(&(map_tabl_lock));
				while (test_and_set_bit(logic_page_num, maptab_bitmap) != 0)
				{
					schedule();
				}
				phy_page_offs = map_table[logic_page_num];
		//up_read(&(map_tabl_lock));
				if (test_and_clear_bit(logic_page_num, maptab_bitmap) == 0)
				{
					printk(KERN_INFO "mapbitmap cleared wrong");
					BUG();
				}
		
			
				
				down_write(&(pfetch_buf_lock[bufidx]));
				retlen =0;
				mtd->read(mtd,((phy_page_offs<<mtdblk->pageshift) + (sect_idx*mtdblk->blksize)),mtdblk->blksize, &retlen, prefetch_buf[bufidx]);	
											
				if(retlen != mtdblk->blksize)
				{
					printk(KERN_ERR "FTL read failure");
					printk(KERN_ERR " phypage = %lld ",phy_page_offs);
	
					printk(KERN_ERR " logpage = %lld",logic_page_num);
				}
				pfetch_lookup_tab[bufidx] = logic_ftl_blk;
		
				p_fetch_info[bufidx].last_touch = jiffies;
				p_fetch_info[bufidx].read_mask =0ULL;
				p_fetch_info[bufidx].state = IN_USE;
				p_fetch_info[bufidx].index_inlist = cur_idx;
		
		
				up_write(&(pfetch_buf_lock[bufidx]));
		
				atomic_dec(&mtdblk->activity_matrix.num_reads_pref[lpn_in_bank]);	
				clear_bit(cur_idx,prefetch_onbitmap);
		
			}
			
					
		}
		/* the reader is in the window of the prefetcher. look far ahead.
		 */
		if(mtdblk->accesslist[cur_idx] == -1)
		{
			cur_idx = (cur_idx+10)%MAX_ACCESS_LIST;
		}
		else
		{
			cur_idx = (cur_idx+1)%MAX_ACCESS_LIST;
		}
		schedule();

	}
}


#if 1
						
static int mtdblock_readsect(struct mymtd_blktrans_dev *dev,
			     unsigned long logic_ftl_blk, char *buf)
{
	struct mtdblk_dev *mtdblk = container_of(dev, struct mtdblk_dev, mbd);
	struct mtd_info *mtd = mtdblk->mbd.mtd;
	struct mtd_oob_ops ops;
	uint8_t *rd_buf, *oob_buf;
	int res;
	uint32_t logic_page_num;
	uint32_t offs;
	uint32_t len;
	uint64_t mask;
	uint32_t sect_idx;
	/* needed 64bit as we do some shifting*/
	uint64_t phy_page_offs;
	uint32_t shift_val;
	int i;
	uint32_t cache_buf,found_cache_buf;
	int found = 0;
	int j;
	size_t retlen;
	static int hitcount = 0,fail_hits=0, acl_notthere =0;
	static int num_read = 0;
	struct cache_num_node *node;
	uint32_t num_pages_perbank;
	uint32_t bankno;
	
	
	num_read++;
	num_pages_perbank  =  mtdblk->hwblks_per_bank*mtdblk->pages_per_blk;
	
	logic_page_num = (logic_ftl_blk<<mtdblk->blkshift)>>mtdblk->pageshift;
	bankno = map_table[logic_page_num]/num_pages_perbank;
#ifdef EIGHT_BANK_FLASH
	bankno = bankno/8;
#endif
	
	
	shift_val = mtdblk->pageshift -mtdblk->blkshift;
	mask = ~(-1UL<<shift_val);
	sect_idx = logic_ftl_blk&mask;


#if 0
	printk(KERN_INFO "mtdblock_readsect = %d pgr = %ld sect_idx = %d",logic_ftl_blk,logic_page_num,sect_idx);
	
#endif
//	check_can_prefetch(mtdblk,logic_page_num);
	


buf_lookup_search:
	found = 0;
	for(i = 0; i < MAX_FTL_CACHEBUFS;i++)
	{
		if(buf_lookup_tab[i] == logic_page_num)
		{
			if(found == 1)
			{
				printk(KERN_INFO "myftl: R twice in buflookup %u",logic_page_num);
				printk(KERN_INFO " ");
	
				for(j = 0; j < MAX_FTL_CACHEBUFS ;j++)
				{
					printk("%lld ",buf_lookup_tab[j]);
				}
				BUG();
			}
			found = 1;
			found_cache_buf = i;
			break;
		}
	}
	if(found == 1)
	{
		cache_buf = found_cache_buf;
#if 0
		printk(KERN_INFO "in cache %d",cache_buf);
#endif
		mutex_lock(&(mtdblk->buf_lock[cache_buf]));

		if(buf_lookup_tab[cache_buf] != logic_page_num)
		{
			mutex_unlock(&(mtdblk->buf_lock[cache_buf]));	
			goto buf_lookup_search;
		}
		mask = ((1UL)<<sect_idx);
		//	down_read(&(mtdblk->bufstate_lock[cache_buf]));
		if(((mtdblk->cached_buf[cache_buf].written_mask) & (mask)) == mask)
		{

			printk(KERN_INFO "mask is correct %d",cache_buf);
			memcpy(buf,mtdblk->buf[cache_buf]+sect_idx*mtdblk->blksize,mtdblk->blksize);
	//		up_read(&(mtdblk->bufstate_lock[cache_buf]));

			mutex_unlock(&(mtdblk->buf_lock[cache_buf]));
			
			
			return 0;		
		}
		else
		{
			printk(KERN_INFO "mask is incorrect %d",cache_buf);
			mutex_unlock(&(mtdblk->buf_lock[cache_buf]));
			goto not_inFTLbufs;
		}
	}

	
		
not_inFTLbufs:	
	
	
	
		bankno = map_table[logic_page_num]/num_pages_perbank;
		atomic_inc(&mtdblk->activity_matrix.num_reads[bankno]);	
	
	/* okay!! 
		* not in the cache
	*/
	//down_read(&(map_tabl_lock));
		while (test_and_set_bit(logic_page_num, maptab_bitmap) != 0)
		{
			schedule();
		}
		phy_page_offs = map_table[logic_page_num];
	//up_read(&(map_tabl_lock));
		if (test_and_clear_bit(logic_page_num, maptab_bitmap) == 0)
		{
			printk(KERN_INFO "mapbitmap cleared wrong");
			BUG();
		}

	//if(phy_page_offs == INVALID_PAGE_NUMBER)
		if(phy_page_offs < 0 || phy_page_offs > MAP_TABLE_SIZE)
		{
			memcpy(mtdblk->FFbuf,rd_buf,mtdblk->blksize);	
			printk(KERN_INFO " wrong phy addr %u lpn", phy_page_offs, logic_page_num);
			BUG(); 
		}
		else
		{
#if 0
			printk(KERN_INFO "r maptable[%llu] = %llu",logic_page_num,phy_page_offs);
			rd_buf = vmalloc(mtd->writesize);
			if (!rd_buf){
			printk(KERN_INFO "myftl: not able to alloc mem");
			mtdblk->activity_matrix.num_reads[bankno]--;
			return -EINTR;
		}
#endif
	

	
		retlen = 0;
		
		mtd->read(mtd,((phy_page_offs<<mtdblk->pageshift) + (sect_idx*mtdblk->blksize)),mtdblk->blksize, &retlen, buf);	
		if(retlen != mtdblk->blksize)
		{
			printk(KERN_ERR "FTL read failure");
			printk(KERN_ERR " phypage = %lld secidx = %ld",phy_page_offs,sect_idx);

			printk(KERN_ERR " logpage = %lld",logic_page_num);
		}

		len = mtdblk->blksize;
		offs = 0;
		
#if 0
		oob_buf = vmalloc(mtd->oobsize);
		if (!oob_buf){
				printk(KERN_INFO "myftl: not able to alloc mem");
			mtdblk->activity_matrix.num_reads[bankno]--;
			return -EINTR;
			}
	
			ops.mode = MTD_OOB_AUTO;
			ops.datbuf = rd_buf;
			ops.len = mtdblk->blksize;
			ops.oobbuf = oob_buf;
			ops.ooboffs = 0;	
			ops.ooblen = mtd->oobsize;


			res = mtd->read_oob(mtd,(phy_page_offs<<mtdblk->pageshift + (sect_idx*mtdblk->blksize)), &ops);
			if(ops.retlen < mtdblk->blksize)
			{
				printk(KERN_ERR "FTL read failure");
				printk(KERN_ERR " phypage = %lld",phy_page_offs);
				printk(KERN_ERR " logpage = %lld",logic_page_num);
			}
			vfree(oob_buf);
			vfree(rd_buf);
		//memcpy(buf,rd_buf,mtdblk->blksize);
#endif
	
		
		
		
		len -= mtdblk->blksize;
		offs += mtdblk->blksize;
		//logic_page_num+=1;
		}
		atomic_dec(&mtdblk->activity_matrix.num_reads[bankno]);	
	
	
	
#ifdef MYFTL_DEBUG
		printk(KERN_INFO "mtdblock_readsect done merged = %ld",phy_page_offs);
		printk(KERN_INFO "pre hit %d/%d %d",hitcount,num_read,prefetch_count);	
#endif

	
		return 0;

}
#endif
	




#ifdef  PREFETCH_ACL
						
static int mtdblock_readsect(struct mymtd_blktrans_dev *dev,
						unsigned long logic_ftl_blk, char *buf)
{
	struct mtdblk_dev *mtdblk = container_of(dev, struct mtdblk_dev, mbd);
	struct mtd_info *mtd = mtdblk->mbd.mtd;
	struct mtd_oob_ops ops;
	uint8_t *rd_buf, *oob_buf;
	int res;
	uint32_t logic_page_num;
	uint32_t offs;
	uint32_t len;
	uint64_t mask;
	uint32_t sect_idx;
	/* needed 64bit as we do some shifting*/
	uint64_t phy_page_offs;
	uint32_t shift_val;
	int i;
	uint32_t cache_buf,found_cache_buf;
	int found = 0;
	int j;
	size_t retlen;
	static int hitcount = 0,fail_hits=0, acl_notthere =0;
	static int num_read = 0;
	struct cache_num_node *node;
	uint32_t num_pages_perbank;
	uint32_t bankno;
	
	
	num_read++;
	num_pages_perbank  =  mtdblk->hwblks_per_bank*mtdblk->pages_per_blk;
	
	logic_page_num = (logic_ftl_blk<<mtdblk->blkshift)>>mtdblk->pageshift;
	bankno = map_table[logic_page_num]/num_pages_perbank;
#ifdef EIGHT_BANK_FLASH
	bankno = bankno/8;
#endif
	
	
	shift_val = mtdblk->pageshift -mtdblk->blkshift;
	mask = ~(-1UL<<shift_val);
	sect_idx = logic_ftl_blk&mask;


#if 0
	printk(KERN_INFO "mtdblock_readsect = %d pgr = %ld sect_idx = %d",logic_ftl_blk,logic_page_num,sect_idx);
	
#endif
//	check_can_prefetch(mtdblk,logic_page_num);
	


buf_lookup_search:
	found = 0;
	for(i = 0; i < MAX_FTL_CACHEBUFS;i++)
	{
		if(buf_lookup_tab[i] == logic_page_num)
		{
			if(found == 1)
			{
				printk(KERN_INFO "myftl: R twice in buflookup %u",logic_page_num);
				printk(KERN_INFO " ");
				
				for(j = 0; j < MAX_FTL_CACHEBUFS ;j++)
				{
					printk("%lld ",buf_lookup_tab[j]);
				}
				BUG();
			}
			found = 1;
			found_cache_buf = i;
			break;
		}
	}
	if(found == 1)
	{
		cache_buf = found_cache_buf;
		#if 0
		printk(KERN_INFO "in cache %d",cache_buf);
		#endif
		mutex_lock(&(mtdblk->buf_lock[cache_buf]));
		
		if(buf_lookup_tab[cache_buf] != logic_page_num)
		{
			mutex_unlock(&(mtdblk->buf_lock[cache_buf]));	
			goto buf_lookup_search;
		}
		mask = ((1UL)<<sect_idx);
	//	down_read(&(mtdblk->bufstate_lock[cache_buf]));
		if(((mtdblk->cached_buf[cache_buf].written_mask) & (mask)) == mask)
		{
				
			printk(KERN_INFO "mask is correct %d",cache_buf);
			memcpy(buf,mtdblk->buf[cache_buf]+sect_idx*mtdblk->blksize,mtdblk->blksize);
	//		up_read(&(mtdblk->bufstate_lock[cache_buf]));

			mutex_unlock(&(mtdblk->buf_lock[cache_buf]));
			
			
			return 0;		
		}
		else
		{
			printk(KERN_INFO "mask is incorrect %d",cache_buf);
			mutex_unlock(&(mtdblk->buf_lock[cache_buf]));
			goto not_found;
		}
	}
not_found:
try_prefetch_bufs:	
	
	for(i = 0; i < MAX_FTL_PREFETCH_BUFS;i++)
	{
		if(pfetch_lookup_tab[i] == logic_ftl_blk)
		{
			if(found == 1)
			{
				printk(KERN_INFO "myftl: R twice in buflookup %u",logic_ftl_blk);
				printk(KERN_INFO " ");
			
				for(j = 0; j < MAX_FTL_PREFETCH_BUFS ;j++)
				{
					printk("%lld ",pfetch_lookup_tab[j]);
				}
				BUG();
			}
			found = 1;
			found_cache_buf = i;
			break;
		}
	}	
	
	if(found == 1)
	{
		cache_buf = found_cache_buf;
#if 1
		hitcount++;
		printk(KERN_INFO "pref hit  %ld %ul", hitcount,logic_ftl_blk);
	
#endif
pref_buf_found:
		down_read(&(pfetch_buf_lock[cache_buf]));
	
		if(pfetch_lookup_tab[cache_buf] != logic_ftl_blk)
		{
			up_read(&(pfetch_buf_lock[cache_buf]));
			printk(KERN_INFO "pfetch: all the way back");
			goto buf_lookup_search;
		}
#ifdef PREFETCH_DEBUG
		printk(KERN_INFO "cache_buf = %d lpn %lld ",cache_buf,logic_page_num);
#endif
		memcpy(buf,prefetch_buf[cache_buf],mtdblk->blksize);
		p_fetch_info[cache_buf].last_touch = jiffies;
		
		int idx;
		idx = p_fetch_info[cache_buf].index_inlist;
		if(idx == -1)
		{
			printk(KERN_INFO "pfetch info idx = -1");
			BUG();
		}
		mtdblk->accesslist[idx] = INVALID_PAGE_NUMBER_32;
		p_fetch_info[cache_buf].index_inlist = -1;
		
		//set_bit(sect_idx,&(p_fetch_info[cache_buf].read_mask));
		test_and_clear_bit(pfetch_lookup_tab[cache_buf],ftlblk_incache_bitmap);
		pfetch_lookup_tab[cache_buf] = INVALID_PAGE_NUMBER_32;
		p_fetch_info[cache_buf].read_mask =0ULL;
		p_fetch_info[cache_buf].state = NOT_IN_USE;
			
		printk(KERN_INFO "enqpref buf %ld",cache_buf);
		node = kmem_cache_alloc(qnode_cache, GFP_KERNEL);
		//node = kmalloc(sizeof(*node),GFP_KERNEL);
		
		if (!node)
		{
			printk(KERN_INFO "kmalloc fail \n");	
			BUG();
		}
		node->value = cache_buf;
		lfq_node_init_rcu(&node->list);
		rcu_read_lock();
		lockfree_enqueue(&pfetch_bufsq, &node->list);
		rcu_read_unlock();
		
		pref_buf_count++;
		printk(KERN_INFO "rdenq  buf count = %d",pref_buf_count);
				
		
		up_read(&(pfetch_buf_lock[cache_buf]));
		
		
		
		#ifdef PREFETCH_DEBUG
		printk(KERN_INFO "pre hit %d/%d %d",hitcount,num_read,prefetch_count);
		#endif
		return 0;		

	}
	int acl_found = 0;
	for(i = 0; i < MAX_ACCESS_LIST;i++)
	{
		if(mtdblk->accesslist[i] == logic_ftl_blk)
		{
			while (test_and_set_bit(i, acl_state) != 0)
			{
				schedule();
			}
			
			if(mtdblk->accesslist[i] == logic_ftl_blk)
			{
				mtdblk->accesslist[i] = INVALID_PAGE_NUMBER_32;
			}
			else
			{
				if (test_and_clear_bit(i, acl_state) == 0)
				{
					printk(KERN_INFO "acl_state cleared wrong");
					BUG();
				}
				continue;
			}
			
			if (test_and_clear_bit(i, acl_state) == 0)
			{
				printk(KERN_INFO "acl_state cleared wrong");
				BUG();
			}
			
			
			fail_hits++;
			
			printk(KERN_INFO "prefbuf[%u]  fail%u h%ld  f%ld ", i,logic_ftl_blk, hitcount,fail_hits);
			acl_found = 1;
			
			if(test_bit(i, prefetch_onbitmap))
			{
				while(test_bit(i, prefetch_onbitmap))
				{
					schedule();
				}	
				goto try_prefetch_bufs;
			}
			break;
		}
	}
	
	if(acl_found == 0)
	{
		acl_notthere++;
		printk(KERN_INFO "acl fail %u Af%d h%ld  f%ld ",logic_ftl_blk, acl_notthere,hitcount,fail_hits);
	}
	bankno = map_table[logic_page_num]/num_pages_perbank;
	atomic_inc(&mtdblk->activity_matrix.num_reads[bankno]);	
	
	/* okay!! 
	* not in the cache
	*/
	//down_read(&(map_tabl_lock));
	while (test_and_set_bit(logic_page_num, maptab_bitmap) != 0)
	{
		schedule();
	}
	phy_page_offs = map_table[logic_page_num];
	//up_read(&(map_tabl_lock));
	if (test_and_clear_bit(logic_page_num, maptab_bitmap) == 0)
	{
		printk(KERN_INFO "mapbitmap cleared wrong");
		BUG();
	}

	//if(phy_page_offs == INVALID_PAGE_NUMBER)
	if(phy_page_offs < 0 || phy_page_offs > MAP_TABLE_SIZE)
	{
		memcpy(mtdblk->FFbuf,rd_buf,mtdblk->blksize);	
		printk(KERN_INFO " wrong phy addr %u lpn", phy_page_offs, logic_page_num);
		BUG(); 
	}
	else
	{
		#if 0
		printk(KERN_INFO "r maptable[%llu] = %llu",logic_page_num,phy_page_offs);
		rd_buf = vmalloc(mtd->writesize);
		if (!rd_buf){
			printk(KERN_INFO "myftl: not able to alloc mem");
			mtdblk->activity_matrix.num_reads[bankno]--;
			return -EINTR;
		}
		#endif
	

	
		retlen = 0;
		
		mtd->read(mtd,((phy_page_offs<<mtdblk->pageshift) + (sect_idx*mtdblk->blksize)),mtdblk->blksize, &retlen, buf);	
		if(retlen != mtdblk->blksize)
		{
			printk(KERN_ERR "FTL read failure");
			printk(KERN_ERR " phypage = %lld secidx = %ld",phy_page_offs,sect_idx);
			
			printk(KERN_ERR " logpage = %lld",logic_page_num);
		}
		
		len = mtdblk->blksize;
		offs = 0;
		
		#if 0
		oob_buf = vmalloc(mtd->oobsize);
		if (!oob_buf){
			printk(KERN_INFO "myftl: not able to alloc mem");
			mtdblk->activity_matrix.num_reads[bankno]--;
			return -EINTR;
		}
			
		ops.mode = MTD_OOB_AUTO;
		ops.datbuf = rd_buf;
		ops.len = mtdblk->blksize;
		ops.oobbuf = oob_buf;
		ops.ooboffs = 0;	
		ops.ooblen = mtd->oobsize;
	
	
		res = mtd->read_oob(mtd,(phy_page_offs<<mtdblk->pageshift + (sect_idx*mtdblk->blksize)), &ops);
		if(ops.retlen < mtdblk->blksize)
		{
			printk(KERN_ERR "FTL read failure");
			printk(KERN_ERR " phypage = %lld",phy_page_offs);
			printk(KERN_ERR " logpage = %lld",logic_page_num);
		}
		vfree(oob_buf);
		vfree(rd_buf);
		//memcpy(buf,rd_buf,mtdblk->blksize);
		#endif
	
		
		
		
		len -= mtdblk->blksize;
		offs += mtdblk->blksize;
		//logic_page_num+=1;
	}
	atomic_dec(&mtdblk->activity_matrix.num_reads[bankno]);	
	
	
	
#ifdef MYFTL_DEBUG
	printk(KERN_INFO "mtdblock_readsect done merged = %ld",phy_page_offs);
	printk(KERN_INFO "pre hit %d/%d %d",hitcount,num_read,prefetch_count);	
#endif

	
	return 0;

}
#endif
		
#if 0
						
static int mtdblock_readsect(struct mymtd_blktrans_dev *dev,
						unsigned long logic_ftl_blk, char *buf)
{
	struct mtdblk_dev *mtdblk = container_of(dev, struct mtdblk_dev, mbd);
	struct mtd_info *mtd = mtdblk->mbd.mtd;
	struct mtd_oob_ops ops;
	uint8_t *rd_buf, *oob_buf;
	int res;
	uint32_t logic_page_num;
	uint32_t offs;
	uint32_t len;
	uint64_t mask;
	uint32_t sect_idx;
	uint64_t phy_page_offs;
	uint32_t shift_val;
	int i;
	uint32_t cache_buf,found_cache_buf;
	int found = 0;
	int j;
	struct lfq_node_rcu *qnode;
	struct cache_num_node *node;
	static int no_pfetch_buf_count = 0;
	
	logic_page_num = (logic_ftl_blk<<mtdblk->blkshift)>>mtdblk->pageshift;
	
	shift_val = mtdblk->pageshift -mtdblk->blkshift;
	mask = ~(-1UL<<shift_val);
	sect_idx = logic_ftl_blk&mask;

	check_can_prefetch(logic_page_num);
#if 0
	printk(KERN_INFO "mtdblock_readsect = %d pgr = %ld sect_idx = %d",logic_ftl_blk,logic_page_num,sect_idx);
#endif
buf_lookup_search:
	found = 0;
	for(i = 0; i < MAX_FTL_CACHEBUFS;i++)
	{
		if(buf_lookup_tab[i] == logic_page_num)
		{
			if(found == 1)
			{
				printk(KERN_INFO "myftl: R twice in buflookup %u",logic_page_num);
				printk(KERN_INFO " ");
				
				for(j = 0; j < MAX_FTL_CACHEBUFS ;j++)
				{
					printk("%lld ",buf_lookup_tab[j]);
				}
				BUG();
			}
			found = 1;
			found_cache_buf = i;
			break;
		}
	}
	if(found == 1)
	{
		cache_buf = found_cache_buf;
		#if 0
		printk(KERN_INFO "in cache %d",cache_buf);
		#endif
		mutex_lock(&(mtdblk->buf_lock[cache_buf]));
		
		if(buf_lookup_tab[cache_buf] != logic_page_num)
		{
			mutex_unlock(&(mtdblk->buf_lock[cache_buf]));	
			goto buf_lookup_search;
		}
		mask = ((1UL)<<sect_idx);

		if(((mtdblk->cached_buf[cache_buf].written_mask) & (mask)) == mask)
		{
				
			#if 0	
			printk(KERN_INFO "mask is correct %d",cache_buf);
			#endif
			memcpy(buf,mtdblk->buf[cache_buf]+sect_idx*mtdblk->blksize,mtdblk->blksize);

			mutex_unlock(&(mtdblk->buf_lock[cache_buf]));
			return 0;		
		}
		else
		{
			#if 0
			printk(KERN_INFO "mask is incorrect %d",cache_buf);
			#endif
			mutex_unlock(&(mtdblk->buf_lock[cache_buf]));
			goto try_prefetchBufs;
		}
	}
	
try_prefetchBufs:
	found = 0;
	
	for(i = 0; i < MAX_FTL_PREFETCH_BUFS;i++)
	{
		if(pfetch_lookup_tab[i] == logic_page_num)
		{
			if(found == 1)
			{
				printk(KERN_INFO "myftl: R twice in buflookup %u",logic_page_num);
				printk(KERN_INFO " ");
				
				for(j = 0; j < MAX_FTL_PREFETCH_BUFS ;j++)
				{
					printk("%lld ",pfetch_lookup_tab[j]);
				}
				BUG();
			}
			found = 1;
			found_cache_buf = i;
			break;
		}
	}
	if(found == 1)
	{
		cache_buf = found_cache_buf;
		#if 0
		printk(KERN_INFO "in cache %d",cache_buf);
		#endif
		down_read(&(pfetch_buf_lock[cache_buf]));
		
		if(pfetch_lookup_tab[cache_buf] != logic_page_num)
		{
			up_read(&(pfetch_buf_lock[cache_buf]));
			goto buf_lookup_search;
		}
		
		
		memcpy(buf,prefetch_buf[cache_buf]+sect_idx*mtdblk->blksize,mtdblk->blksize);
		p_fetch_info[cache_buf].last_touch = jiffies;
		up_read(&(pfetch_buf_lock[cache_buf]));
		return 0;		

	}
not_found:
	/* okay!! 
	* not in the cache
	*/
	
	

		
	down_read(&(map_tabl_lock));
	phy_page_offs = map_table[logic_page_num];
	up_read(&(map_tabl_lock));
	#if 0
	printk(KERN_INFO "r maptable[%llu] = %llu",logic_page_num,phy_page_offs);
	#endif	
		
	int pfetch_select;
	int pfetch_buf_num;
	
	pfetch_select = 0;
	rcu_read_lock();
	qnode = lockfree_dequeue(&pfetch_bufsq);
	node = container_of(qnode, struct cache_num_node, list);
	rcu_read_unlock();
	
	if(node != NULL)
	{
		pfetch_buf_num = node->value;
		call_rcu(&node->rcu, free_cache_num_node);	
		pfetch_select = 1;
	}
	
	
	
	if(pfetch_select == 0)
	{
		no_pfetch_buf_count++;
		printk(KERN_INFO "myftl: no_pref_buf = %lld",no_pfetch_buf_count);
		rd_buf = vmalloc(mtd->writesize);
		if (!rd_buf){
			printk(KERN_INFO "myftl: not able to alloc mem");
			return -EINTR;
		}
		oob_buf = vmalloc(mtd->oobsize);
		if (!oob_buf){
			printk(KERN_INFO "myftl: not able to alloc mem");
			return -EINTR;
		}
		
		len = mtdblk->blksize;
		offs = 0;
	
		{	
			ops.mode = MTD_OOB_AUTO;
			ops.datbuf = rd_buf;
			ops.len = mtd->writesize;
			ops.oobbuf = oob_buf;
			ops.ooboffs = 0;	
			ops.ooblen = mtd->oobsize;
	

			res = mtd->read_oob(mtd,phy_page_offs<<mtdblk->pageshift, &ops);
			if(ops.retlen < mtd->writesize)
			{
				printk(KERN_ERR "FTL read failure");
				printk(KERN_ERR " phypage = %lld",phy_page_offs);
				printk(KERN_ERR " logpage = %lld",logic_page_num);
			}


			memcpy(buf,rd_buf+sect_idx*mtdblk->blksize,mtdblk->blksize);
			len -= mtd->writesize;
			offs += mtd->writesize;
			//logic_page_num+=1;
		
		}
	
	
		
		vfree(rd_buf);
		vfree(oob_buf);
	}
	else
	{
		down_write(&(pfetch_buf_lock[pfetch_buf_num]));
		
		prefetch_buf[pfetch_buf_num] =  vmalloc(mtd->writesize);
		
		if (!prefetch_buf[pfetch_buf_num]){
			printk(KERN_INFO "myftl: not able to alloc mem");
			return -EINTR;
		}
		oob_buf = vmalloc(mtd->oobsize);
		if (!oob_buf){
			printk(KERN_INFO "myftl: not able to alloc mem");
			return -EINTR;
		}
		
		len = mtdblk->blksize;
		offs = 0;
		{	
			ops.mode = MTD_OOB_AUTO;
			ops.datbuf = prefetch_buf[pfetch_buf_num];
			ops.len = mtd->writesize;
			ops.oobbuf = oob_buf;
			ops.ooboffs = 0;	
			ops.ooblen = mtd->oobsize;
			res = mtd->read_oob(mtd,phy_page_offs<<mtdblk->pageshift, &ops);
			if(ops.retlen < mtd->writesize)
			{
				printk(KERN_ERR "FTL read failure");
				printk(KERN_ERR " phypage = %lld",phy_page_offs);
				printk(KERN_ERR " logpage = %lld",logic_page_num);
			}
			memcpy(buf,prefetch_buf[pfetch_buf_num]+sect_idx*mtdblk->blksize,mtdblk->blksize);
			len -= mtd->writesize;
			offs += mtd->writesize;
			//logic_page_num+=1;
		}
		
		p_fetch_info[pfetch_buf_num].last_touch = jiffies;
		pfetch_lookup_tab[pfetch_buf_num] = logic_page_num;
		up_write(&(pfetch_buf_lock[pfetch_buf_num]));
		
		/* give up a prefetch buffer every x secs*/
		schedule_delayed_work(&prefetch_buf_sched[pfetch_buf_num].workq, msecs_to_jiffies(PREFETCH_BUF_HOLDTIME));
		
		vfree(oob_buf);
	}
	
#ifdef MYFTL_DEBUG
	printk(KERN_INFO "mtdblock_readsect done merged = %ld",phy_page_offs);
#endif
			
	return 0;

}
#endif
						
		
/*
 * should be called with mtdblk->bufstate_lock[buf_idx] read version held
 */ 		
int all_sects_dirty(struct mtdblk_dev *mtdblk, int buf_idx)
{
	int i;
	int num_bits = mtdblk->cache_size/mtdblk->blksize;
	//down_read(&(mtdblk->bufstate_lock[buf_idx])); 
	unsigned long mask =  mtdblk->cached_buf[buf_idx].written_mask;
	//up_read(&(mtdblk->bufstate_lock[buf_idx])); 
	for(i = 0; i < num_bits;i++)
	{
		if((mask & 1UL) == 1)
		{
			mask = mask >>1;
			
		}
		else
		{
			return 0;
		}
		
	}
	return 1;
}






static uint32_t exper_alloc_block(struct mtdblk_dev *mtdblk, int cur_wr_index)
{
	uint32_t temp;
	uint32_t search_from;
	
	static unsigned long temp_rand =0;
	

	
	
	if(cur_wr_index == RAND_SEL)
	{
		/* Try to get a semi random initial value. */
		temp_rand++;
		//get_random_bytes(&temp_rand, sizeof(temp_rand));
		cur_wr_index = temp_rand%(mtdblk->num_cur_wr_blks);
		temp_rand = cur_wr_index;
	}
	

	
		
		
	//down_read(&(mtdblk->cur_wr_state[cur_wr_index]));
	if(mtdblk->cur_writing[cur_wr_index].blk == -1)
	{
		search_from = mtdblk->cur_writing[cur_wr_index].first_blk;
	}
	else
	{
		search_from = mtdblk->cur_writing[cur_wr_index].blk;
	}
	//up_read(&(mtdblk->cur_wr_state[cur_wr_index]));
	
	temp=search_from+1;
	if(temp > mtdblk->cur_writing[cur_wr_index].last_blk)
	{
		temp = mtdblk->cur_writing[cur_wr_index].first_blk;
	}
	
	for(;temp != search_from;)
	{
		
		if(blk_isfree(mtdblk,temp))
		{
			if(is_block_bad(mtdblk,temp))
			{
				goto continue_loop;
			}
			else
			{

				printk(KERN_INFO "[%d %d] searchfrom = %d temp = %d",mtdblk->cur_writing[cur_wr_index].first_blk,mtdblk->cur_writing[cur_wr_index].last_blk,search_from,temp);
				blk_unfree(mtdblk,temp);
			//	down_write(&(mtdblk->cur_wr_state[cur_wr_index]));
			//	mtdblk->cur_writing[cur_wr_index].blk = temp;
			//	up_write(&(mtdblk->cur_wr_state[cur_wr_index]));

				atomic_dec(&bank_info[cur_wr_index].perbank_nfree_blks);
				return temp;
			}
		}
	continue_loop:
		temp = temp+1;
		if(temp > mtdblk->cur_writing[cur_wr_index].last_blk)
		{
			temp = mtdblk->cur_writing[cur_wr_index].first_blk;
		}
			
		
	}

	
	return INVALID;
}


//#ifdef NON_SCHEDULED
#ifdef NONBG_GC
static uint64_t get_ppage(struct mtdblk_dev *mtdblk, int cur_wr_index,int from_gc_context)
{
	
	uint32_t ret_page_num;
	uint32_t next_blk;
	uint8_t tried;
#if 0
	int temp;
	int newalloc = 0;
	uint32_t cur_blk;
	int found = 0;
#endif
	static unsigned long temp_rand =0;
	static unsigned long selected_bank = 0;
	
	uint32_t blkno,page_in_blk;
	if(cur_wr_index == RAND_SEL)
	{
		//get_random_bytes(&temp_rand, sizeof(temp_rand));
		cur_wr_index = temp_rand%(mtdblk->num_cur_wr_blks);
		cur_wr_index = (selected_bank+1)%numpllbanks;
		selected_bank = cur_wr_index;
	}
			
	if(cur_wr_index < 0 || cur_wr_index >= numpllbanks)
	{
		printk(KERN_INFO "cur_wrindex = %d wrong",cur_wr_index);
		BUG();
	}
	
#ifdef APR_DEBUG	

	if(from_gc_context)
	{
		printk(KERN_INFO "%x: get_ppage[GC] %d ",current->pid,cur_wr_index);
	}
	else
	{
		printk(KERN_INFO "%x: get_ppage[W]  %d",current->pid,cur_wr_index);
	}
#endif
	down_write(&(mtdblk->cur_wr_state[cur_wr_index]));
	
	if((mtdblk->cur_writing[cur_wr_index].state == STATE_CLEAN) || (mtdblk->cur_writing[cur_wr_index].last_wrpage ==(mtdblk->pages_per_blk-1)))
	{
		next_blk = INVALID;
		tried = 0;
		
		if(!from_gc_context)
		{
			if(bank_info[cur_wr_index].perbank_nfree_blks.counter - mtdblk->reserved_blks_per_bank <= 0)
			{
				next_blk = INVALID;
			}
			else
			{
				tried++;
				next_blk = exper_alloc_block(mtdblk,cur_wr_index);
			}	
		}
		else
		{
			next_blk = exper_alloc_block(mtdblk,cur_wr_index);
							
		}
#ifdef APR_DEBUG	
		printk(KERN_INFO "%x: get_ppage allocblk = %d  ",current->pid,next_blk);
#endif
		if(next_blk == INVALID)
		{
			up_write(&(mtdblk->cur_wr_state[cur_wr_index]));
			ret_page_num = INVALID_PAGE_NUMBER_32;
			
		}
		else
		{
			mtdblk->cur_writing[cur_wr_index].blk = next_blk;
			mtdblk->cur_writing[cur_wr_index].last_wrpage = 0;
			mtdblk->cur_writing[cur_wr_index].state = STATE_DIRTY;
		
			ret_page_num = mtdblk->cur_writing[cur_wr_index].blk* mtdblk->pages_per_blk + mtdblk->cur_writing[cur_wr_index].last_wrpage;
		
			up_write(&(mtdblk->cur_wr_state[cur_wr_index]));
		}
		blkno = mtdblk->cur_writing[cur_wr_index].blk;
		page_in_blk = mtdblk->cur_writing[cur_wr_index].last_wrpage;
	
		//printk(KERN_INFO "bank = %d freeblks = %ld",cur_wr_index,bank_info[cur_wr_index].perbank_nfree_blks.counter);
#if 1
		
		if(!from_gc_context)
		{
			if(gc_testing_on == 1)
			{
			
				
				if(bank_info[cur_wr_index].perbank_nfree_blks.counter < mtdblk->blks_per_bank/8)
				{
				//	printk(KERN_INFO "L2 bank = %d freeblks = %ld",cur_wr_index,bank_info[cur_wr_index].perbank_nfree_blks.counter);
					do_gc_revmapt(mtdblk,cur_wr_index,GC_LEVEL2);
				}
				else if(bank_info[cur_wr_index].perbank_nfree_blks.counter < mtdblk->blks_per_bank/4)
				{
					//printk(KERN_INFO "L1 bank = %d freeblks = %ld",cur_wr_index,bank_info[cur_wr_index].perbank_nfree_blks.counter);
					do_gc_revmapt(mtdblk,cur_wr_index,GC_LEVEL1);
				}
			
				else if(bank_info[cur_wr_index].perbank_nfree_blks.counter < mtdblk->blks_per_bank/2)
				{
					//printk(KERN_INFO "L0 bank = %d freeblks = %ld",cur_wr_index,bank_info[cur_wr_index].perbank_nfree_blks.counter);
					do_gc_revmapt(mtdblk,cur_wr_index,GC_LEVEL0);
				}
				else
				{
						//printk(KERN_INFO "NOGC bank = %d freeblks = %ld",cur_wr_index,bank_info[cur_wr_index].perbank_nfree_blks.counter);
				}
			}
			else
			{
				if(bank_info[cur_wr_index].perbank_nfree_blks.counter < mtdblk->blks_per_bank/8)
					do_gc(mtdblk,cur_wr_index,GC_LEVEL2);
				else if(bank_info[cur_wr_index].perbank_nfree_blks.counter < mtdblk->blks_per_bank/4)
					do_gc(mtdblk,cur_wr_index,GC_LEVEL1);
				else if(bank_info[cur_wr_index].perbank_nfree_blks.counter < mtdblk->blks_per_bank/2)
					do_gc(mtdblk,cur_wr_index,GC_LEVEL0);
			}
		}
#endif
		 
		
		

		return ret_page_num;
	}
	else
	{
		
		mtdblk->cur_writing[cur_wr_index].last_wrpage++;
		
		
		if(mtdblk->cur_writing[cur_wr_index].last_wrpage >= mtdblk->pages_per_blk)
		{
			printk(KERN_INFO "last_wr_page is wrong");
			BUG();
		}
				  
	
		ret_page_num = mtdblk->cur_writing[cur_wr_index].blk* mtdblk->pages_per_blk + mtdblk->cur_writing[cur_wr_index].last_wrpage;
		
		up_write(&(mtdblk->cur_wr_state[cur_wr_index]));
		
		blkno = mtdblk->cur_writing[cur_wr_index].blk;
		page_in_blk = mtdblk->cur_writing[cur_wr_index].last_wrpage;
#if 1
		
		if(!from_gc_context)
		{
			if(gc_testing_on == 1)
			{
					
				if(bank_info[cur_wr_index].perbank_nfree_blks.counter < mtdblk->blks_per_bank/8)
				{
				//	printk(KERN_INFO "L2 bank = %d freeblks = %ld",cur_wr_index,bank_info[cur_wr_index].perbank_nfree_blks.counter);
					do_gc_revmapt(mtdblk,cur_wr_index,GC_LEVEL2);
				}
				else if(bank_info[cur_wr_index].perbank_nfree_blks.counter < mtdblk->blks_per_bank/4)
				{
					//printk(KERN_INFO "L1 bank = %d freeblks = %ld",cur_wr_index,bank_info[cur_wr_index].perbank_nfree_blks.counter);
					do_gc_revmapt(mtdblk,cur_wr_index,GC_LEVEL1);
				}
				
				else if(bank_info[cur_wr_index].perbank_nfree_blks.counter < mtdblk->blks_per_bank/2)
				{
					//printk(KERN_INFO "L0 bank = %d freeblks = %ld",cur_wr_index,bank_info[cur_wr_index].perbank_nfree_blks.counter);
					do_gc_revmapt(mtdblk,cur_wr_index,GC_LEVEL0);
				}
				else
				{
						//printk(KERN_INFO "NOGC bank = %d freeblks = %ld",cur_wr_index,bank_info[cur_wr_index].perbank_nfree_blks.counter);
				}
			}
			else
			{
				if(bank_info[cur_wr_index].perbank_nfree_blks.counter < mtdblk->blks_per_bank/8)
					do_gc(mtdblk,cur_wr_index,GC_LEVEL2);
				else if(bank_info[cur_wr_index].perbank_nfree_blks.counter < mtdblk->blks_per_bank/4)
					do_gc(mtdblk,cur_wr_index,GC_LEVEL1);
				else if(bank_info[cur_wr_index].perbank_nfree_blks.counter < mtdblk->blks_per_bank/2)
					do_gc(mtdblk,cur_wr_index,GC_LEVEL0);
			}
		}
#endif
		
		return ret_page_num;
	}
			
	
	
}
#endif

#ifdef BG_UNC_GC

static uint64_t get_ppage(struct mtdblk_dev *mtdblk, int cur_wr_index,int from_gc_context)
{
	
	uint32_t ret_page_num;
	uint32_t next_blk;
	uint8_t tried;
#if 0
	int temp;
	int newalloc = 0;
	uint32_t cur_blk;
	int found = 0;
#endif
	static unsigned long temp_rand =0;
	static uint32_t selected_bank=0;
	uint32_t startbanknum;
	uint32_t banknum;
	
	uint32_t blkno,page_in_blk;
	int min_cost_bank;
	int min_cost;
	int cost;
	int OP;
	int selected;
	int i;
	
	
	if(cur_wr_index == RAND_SEL)
	{
		startbanknum = (selected_bank)%numpllbanks;
		banknum=(startbanknum+1)%numpllbanks;
	
		
		
		selected_bank = banknum;
		cur_wr_index = selected_bank;
		//get_random_bytes(&temp_rand, sizeof(temp_rand));
		//cur_wr_index = temp_rand%(mtdblk->num_cur_wr_blks);
	}
	atomic_inc(&mtdblk->activity_matrix.num_writes[cur_wr_index]);
#ifdef PLL_GC_DEBUG
	
	printk(KERN_INFO "selected bank = %d",cur_wr_index);		
#endif
	down_write(&(mtdblk->cur_wr_state[cur_wr_index]));
	
	if((mtdblk->cur_writing[cur_wr_index].state == STATE_CLEAN) || (mtdblk->cur_writing[cur_wr_index].last_wrpage ==(mtdblk->pages_per_blk-1)))
	{
		next_blk = INVALID;
		tried = 0;
		
		if(!from_gc_context)
		{
			if(bank_info[cur_wr_index].perbank_nfree_blks.counter - mtdblk->reserved_blks_per_bank <= 0)
			{
				next_blk = INVALID;
			}
			else
			{
				tried++;
				next_blk = exper_alloc_block(mtdblk,cur_wr_index);
			}	
		}
		else
		{
			next_blk = exper_alloc_block(mtdblk,cur_wr_index);
							
		}
		
		
		
		if(next_blk == INVALID)
		{
			up_write(&(mtdblk->cur_wr_state[cur_wr_index]));
			atomic_dec(&mtdblk->activity_matrix.num_writes[cur_wr_index]);
			ret_page_num = INVALID_PAGE_NUMBER_32;
		}
		else
		{
			mtdblk->cur_writing[cur_wr_index].blk = next_blk;
			mtdblk->cur_writing[cur_wr_index].last_wrpage = 0;
			mtdblk->cur_writing[cur_wr_index].state = STATE_DIRTY;
		
			ret_page_num = mtdblk->cur_writing[cur_wr_index].blk* mtdblk->pages_per_blk + mtdblk->cur_writing[cur_wr_index].last_wrpage;
		
			up_write(&(mtdblk->cur_wr_state[cur_wr_index]));
		}
		
		
		blkno = mtdblk->cur_writing[cur_wr_index].blk;
		page_in_blk = mtdblk->cur_writing[cur_wr_index].last_wrpage;
	
		//printk(KERN_INFO "bank = %d freeblks = %ld",cur_wr_index,bank_info[cur_wr_index].perbank_nfree_blks.counter);
#if 1
		if(bank_info[cur_wr_index].perbank_nfree_blks.counter < mtdblk->blks_per_bank/2)
		{
			
	
#ifdef ADAPTIVE_GC
			
			//if(task_is_stopped(mtdblk->ftlgc_thrd[0]))
			{
				/*Returns 1 if the process was woken up, 0 if it was already
				* running.*/
				wake_up_process(mtdblk->ftlgc_thrd[0]);
					
			}
#else
				for(i = 0; i < NUM_GC_THREAD;i++)
				{
					if(wake_up_process(mtdblk->ftlgc_thrd[i]) == 1)
					{
						atomic_inc(&num_gc_wakeups);
					}
				}
#endif
		}
	
	//	atomic_inc(&mtdblk->activity_matrix.num_writes[cur_wr_index]);
	
#ifdef PLL_GC_DEBUG
		printk(KERN_INFO "%x: [%ld]num_wr++ = %ld",current->pid,cur_wr_index,mtdblk->activity_matrix.num_writes[cur_wr_index].counter);
#endif
			
#endif
		return ret_page_num;
	}
	else
	{
		
		mtdblk->cur_writing[cur_wr_index].last_wrpage++;
		
		
		if(mtdblk->cur_writing[cur_wr_index].last_wrpage >= mtdblk->pages_per_blk)
		{
			printk(KERN_INFO "last_wr_page is wrong");
			BUG();
		}
				  
	
		ret_page_num = mtdblk->cur_writing[cur_wr_index].blk* mtdblk->pages_per_blk + mtdblk->cur_writing[cur_wr_index].last_wrpage;
		
		up_write(&(mtdblk->cur_wr_state[cur_wr_index]));
		
		blkno = mtdblk->cur_writing[cur_wr_index].blk;
		page_in_blk = mtdblk->cur_writing[cur_wr_index].last_wrpage;

				
#if 1			
		if(bank_info[cur_wr_index].perbank_nfree_blks.counter < mtdblk->blks_per_bank/2)
		{
	
	
	
#ifdef ADAPTIVE_GC
	
			//if(task_is_stopped(mtdblk->ftlgc_thrd[0]))
			{
				/*Returns 1 if the process was woken up, 0 if it was already
				* running.*/
				wake_up_process(mtdblk->ftlgc_thrd[0]);
					
			}
#else
				for(i = 0; i < NUM_GC_THREAD;i++)
					wake_up_process(mtdblk->ftlgc_thrd[i]);
#endif
		}
			
		
		//atomic_inc(&mtdblk->activity_matrix.num_writes[cur_wr_index]);
		
#ifdef PLL_GC_DEBUG
		printk(KERN_INFO "%x: [%ld]num_wr++ = %ld",current->pid,cur_wr_index,mtdblk->activity_matrix.num_writes[cur_wr_index].counter);
#endif
#endif
		
		return ret_page_num;
	}

}
#endif

struct gc_threadinfo
{
	struct mtdblk_dev *mtdblk;
	int banknum;
};

void check_and_dogc_thrd(void *arg)
{
	int banknum;
	struct mtdblk_dev *mtdblk;
	int iteration_useless;
	int useless_iterations;
	int can_sleep, must_sleep;
	int gained_blks;
	int has_one_active_writer;
	int loopcount=0;
	int have_to, had_it, no_sleep;
	int i;
	int prev_range = -1;
	int num_times_in_same_range = 0;
	int threadnum;
	int cangcthrds = 1;
	
	int min_free_blks_bank = -1;
	int min_free_blks_amount;
	int numbitset;
	mtdblk = ((struct gcthread_arg_data *)arg)->mtdblk_ptr;
	threadnum = ((struct gcthread_arg_data *)arg)->thrdnum;
	
	min_free_blks_amount = mtdblk->blks_per_bank;
	
	printk(KERN_INFO "check_and_dogc_thrd %d started  %ld time",threadnum,loopcount);
	test_and_set_bit(threadnum, mtdblk->gc_active_map);
	while(mtdblk->init_not_done == 1)
	{
		schedule();
	}
	while (!kthread_should_stop()) 	{
	
		have_to = 0; had_it = 0; no_sleep = 0; can_sleep = 1; must_sleep = 0;
		#ifdef APR_DEBUG
		printk(KERN_INFO "check_and_dogc_thrd %ld time",loopcount);
		#endif
		iteration_useless = 1;
		for(banknum = 0;  banknum < numpllbanks; banknum++)
		{
			gained_blks = 0;
			
			if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/2)
			{
				if(threadnum == 0)
				{ 
					if(min_free_blks_amount < bank_info[banknum].perbank_nfree_blks.counter)
					{
						min_free_blks_bank = banknum;
						min_free_blks_amount = bank_info[banknum].perbank_nfree_blks.counter;
					}
				}
				if(mtdblk->activity_matrix.num_writes[banknum].counter > VIRGO_NUM_MAX_REQ_Q)
				{
					printk(KERN_INFO "num wr %ld > possible",mtdblk->activity_matrix.num_writes[banknum].counter);
					BUG();
				}
				have_to = 1;
				
				int OP;
				int cost;
				OP = GC_OP ;
				

				//cost = mtdblk->activity_matrix.gc_goingon[banknum].counter*cost_op[OP][GC_OP] + mtdblk->activity_matrix.num_writes[banknum].counter*cost_op[OP][WR_OP] + mtdblk->activity_matrix.num_reads_pref[banknum].counter*cost_op[OP][PFTCH_OP] + mtdblk->activity_matrix.num_reads[banknum].counter*cost_op[OP][RD_OP];
				
				//if(cost < cost_thresh[banknum][OP])
				if((numbitset= bitmap_weight(mtdblk->mbd.active_iokthread, 64)) < (numpllbanks/2))
				{ 
					if(mtdblk->activity_matrix.num_writes[banknum].counter == 0)			
					{
					atomic_inc(&mtdblk->activity_matrix.gc_goingon[banknum]);	
				
					had_it = 1;
					if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/8)
					{
						if(gc_testing_on == 1)
						{
							gained_blks = do_gc_revmapt(mtdblk,banknum,GC_LEVEL2);
						}
						else 
						{
							gained_blks = do_gc(mtdblk,banknum,GC_LEVEL2);
						}
					}
					else if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/4)
					{
						if(gc_testing_on == 1)
						{
							gained_blks = do_gc_revmapt(mtdblk,banknum,GC_LEVEL1);
						}
						else
						{
							gained_blks = do_gc(mtdblk,banknum,GC_LEVEL1);
						}
					}
					else if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/2)
					{
						if(gc_testing_on == 1)
						{
							gained_blks = do_gc_revmapt(mtdblk,banknum,GC_LEVEL0);
						}
						else
						{
							gained_blks = do_gc(mtdblk,banknum,GC_LEVEL0);
						}
					}
					atomic_dec(&mtdblk->activity_matrix.gc_goingon[banknum]);	
					
					}
				}
				else
				{
					atomic_inc(&mtdblk->activity_matrix.gc_goingon[banknum]);	
				
					had_it = 1;
					if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/8)
					{
						if(gc_testing_on == 1)
						{
							gained_blks = do_gc_revmapt(mtdblk,banknum,GC_LEVEL2);
						}
						else 
						{
							gained_blks = do_gc(mtdblk,banknum,GC_LEVEL2);
						}
					}
					else if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/4)
					{
						if(gc_testing_on == 1)
						{
							gained_blks = do_gc_revmapt(mtdblk,banknum,GC_LEVEL1);
						}
						else
						{
							gained_blks = do_gc(mtdblk,banknum,GC_LEVEL1);
						}
					}
					else if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/2)
					{
						if(gc_testing_on == 1)
						{
							gained_blks = do_gc_revmapt(mtdblk,banknum,GC_LEVEL0);
						}
						else
						{
							gained_blks = do_gc(mtdblk,banknum,GC_LEVEL0);
						}
					}
					atomic_dec(&mtdblk->activity_matrix.gc_goingon[banknum]);	
					
				}
				
			
			}
			if(gained_blks > 0)
			{
				iteration_useless = 0;
				useless_iterations = 0;
			}
			if(can_sleep  == 1)
			{
				/* we have to Garbage collection
				 * and did we had it done?*/
				if(have_to == 1)
				{
					if(had_it == 1)
					{
						/* yes!! done*/
						have_to = 0;
						had_it = 0;
					}
					else
					{
						/* no!! not done*/
						no_sleep = 1;
						can_sleep = 0;
					}
				}
			}
			
			
			
		}
		if(iteration_useless != 1)
		{
			if(threadnum == 0)
			{
				scheduled_for_gc[0] = min_free_blks_bank;
			}
		}
		if(iteration_useless == 1)
		{
			if(threadnum == 0)
			{
				scheduled_for_gc[0] = -1;
			}
			useless_iterations++;
			//if(useless_iterations > (mtdblk->blks_per_bank/ITERATIONS_PER_GC_CALL))
			if(useless_iterations > (mtdblk->blks_per_bank/ITERATIONS_PER_GC_CALL))
			{
			//	printk(KERN_INFO "jif %u lastwr %u",jiffies,mtdblk->last_wr_time);
				if(jiffies_to_msecs(jiffies - mtdblk->last_wr_time) > 5000)
				{
					must_sleep = 1;
					useless_iterations = 0;
				}
			}
#if 0
			if(useless_iterations > (mtdblk->blks_per_bank))
			{
				has_one_active_writer = 0;
				for(i = 0; i < numpllbanks;i++)
				{
					if(mtdblk->activity_matrix.num_writes[i].counter != 0)
					{
						has_one_active_writer = 1;
						break;
					}
				}
				if(!has_one_active_writer)
				{
					must_sleep = 1;
				
				}
				useless_iterations = 0;
			}
#endif
				
					
		}
			 
		
		
#ifdef ADAPTIVE_GC
		
	
	int wakeupnum;
	int numiothreads,numgcthreads;
	int threshgcthrds;
	
	numbitset = bitmap_weight(mtdblk->mbd.active_iokthread, 64);
	numiothreads = numbitset;
	if(numiothreads == 0)
	{
		threshgcthrds = 8;
		if(prev_range == 0)
		num_times_in_same_range++;
		else
		prev_range = 0;
	}
	else if(numiothreads >= 1 &&  numiothreads < 10)
	{
		threshgcthrds = 4;
		if(prev_range == 1)
		num_times_in_same_range++;
		else
		prev_range = 1;
	}
	else
	//else if(numiothreads > 10)
	{
		threshgcthrds = 1;
		if(prev_range == 3)
		num_times_in_same_range++;
		else
		 prev_range = 3;
	}
	/*cutshort GC phase*/
	
	
#ifdef GC_DEBUG_ADAPTIVE
	printk(KERN_INFO "GC numbitsset %d ",numbitset);
#endif
	
	if(num_times_in_same_range > 3)
	{
#ifdef GC_DEBUG_ADAPTIVE
		printk(" rf to %d",prev_range);
#endif
		num_times_in_same_range = 0;
		cangcthrds = threshgcthrds;
		
		
	}
	
	
	
	numbitset = bitmap_weight(mtdblk->gc_active_map, 64);
	numgcthreads = numbitset;
	
	//if(activenumgcthread.counter < cangcthrds)
	if(numgcthreads < cangcthrds)
	{
		/* the master thread is the one that will wakeup other threads*/
		if(threadnum == 0)
		{
		/* if the iterations were useless , no need to restart new threads*/
		if(must_sleep != 1)
		{
			/* this is a rough number of threads to wakeup*/
			wakeupnum = cangcthrds - numgcthreads;
			if(wakeupnum < 0 || wakeupnum > NUM_GC_THREAD)
			{
				printk(KERN_INFO "wakeupnum wrong canggc = (%d) numbitset = %d wakupnum = (%d)",cangcthrds,numgcthreads,wakeupnum);
				BUG();
			}
			i = 0;
			//while(activenumgcthread.counter < cangcthrds)
			while (wakeupnum > 0)
			{
				if(i >= NUM_GC_THREAD)
				{
				//	printk(KERN_INFO "iterations (%d) > maxGCth(%d) and activethreads = %ld cangcthreads = %d",i,NUM_GC_THREAD,activenumgcthread.counter,cangcthrds);
					//BUG();
					break;
				}
				
				
				//if(task_is_stopped(mtdblk->ftlgc_thrd[i]))
				{
					/*Returns 1 if the process was woken up, 0 if it was already
					* running.*/
					if(wake_up_process(mtdblk->ftlgc_thrd[i]) == 1)
					{
						//atomic_inc(&activenumgcthread);
					//	printk(KERN_INFO "GC %d wakes up %d th thread ",threadnum,i);
						wakeupnum--;
					}
				}
#if 0
				if(activenumgcthread.counter < 0 || activenumgcthread.counter > NUM_GC_THREAD)
				{
					printk(KERN_INFO "active GC threads(%d) > (%d)",activenumgcthread.counter,NUM_GC_THREAD);
					BUG();
					numbitset = bitmap_weight(mtdblk->gc_active_map, 64);
					atomic_set(&activenumgcthread,numbitset);
				}
#endif
				
				i++;
			
			}
		}
		}
		
	}
	//else if(activenumgcthread.counter > cangcthrds)
	else if(numgcthreads > cangcthrds)
	{
		if(threadnum != 0)
		{
			must_sleep = 1;
		}
	
	}
	

	

#endif
		//if((!no_sleep) || can_sleep == 1 || must_sleep == 1)
	if(must_sleep == 1)
	{
		
		//printk(KERN_INFO "GC[%d] to sleep",threadnum);
		set_current_state(TASK_INTERRUPTIBLE);
		test_and_clear_bit(threadnum, mtdblk->gc_active_map);
		#ifdef ADAPTIVE_GC

#if 0
		atomic_dec(&activenumgcthread);
		if(activenumgcthread.counter < 0 || activenumgcthread.counter > NUM_GC_THREAD)
		{
			
			printk(KERN_INFO "active GC threads(%d) wr",activenumgcthread.counter);
			BUG();
			numbitset = bitmap_weight(mtdblk->gc_active_map, 64);
			atomic_set(&activenumgcthread,numbitset);
		}
#endif
		#endif
		schedule();
		set_current_state(TASK_RUNNING);
		test_and_set_bit(threadnum, mtdblk->gc_active_map);
		
		
	//	printk(KERN_INFO "GC[%d] woken up",threadnum);
		must_sleep = 0;
	}
	loopcount++;
	
	}
}

#ifdef BG_UNC_GC
void do_naive_gc_thrd(void *arg)
{
	int banknum;
	struct mtdblk_dev *mtdblk;
	int iteration_useless;
	int useless_iterations;
	int can_sleep, must_sleep;
	int gained_blks;
	int has_one_active_writer;
	int loopcount=0;
	int have_to, had_it, no_sleep;
	int i;
	int threadnum;
	int prev_range = -1;
	int num_times_in_same_range = 0;
	int cangcthrds = 1;
	
	mtdblk = ((struct gcthread_arg_data *)arg)->mtdblk_ptr;
	threadnum = ((struct gcthread_arg_data *)arg)->thrdnum;
	
	printk(KERN_INFO "do_naive_gc_thrd %d started  %ld time",threadnum,loopcount);
	test_and_set_bit(threadnum, mtdblk->gc_active_map);
	while(mtdblk->init_not_done == 1)
	{
		schedule();
	}
	
	
	while (!kthread_should_stop()) 	{
	
		have_to = 0; had_it = 0; no_sleep = 0; can_sleep = 1; must_sleep = 0;
#ifdef APR_DEBUG
		printk(KERN_INFO "do_naive_gc_thrd %ld time",loopcount);
#endif
		iteration_useless = 1;
		for(banknum = 0;  banknum < numpllbanks; banknum++)
		{
			gained_blks = 0;
			if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/2)
			{
				if(mtdblk->activity_matrix.num_writes[banknum].counter > VIRGO_NUM_MAX_REQ_Q)
				{
					printk(KERN_INFO "num wr %ld > possible",mtdblk->activity_matrix.num_writes[banknum].counter);
					BUG();
				}
				have_to = 1;
				
				int OP;
				int cost;
				OP = GC_OP ;
				

				//cost = mtdblk->activity_matrix.gc_goingon[banknum].counter*cost_op[OP][GC_OP] + mtdblk->activity_matrix.num_writes[banknum].counter*cost_op[OP][WR_OP] + mtdblk->activity_matrix.num_reads_pref[banknum].counter*cost_op[OP][PFTCH_OP] + mtdblk->activity_matrix.num_reads[banknum].counter*cost_op[OP][RD_OP];
				
				//if(cost < cost_thresh[banknum][OP])
				//if(mtdblk->activity_matrix.num_writes[banknum].counter == 0)
				{ 
				
					

					atomic_inc(&mtdblk->activity_matrix.gc_goingon[banknum]);	
#ifdef PLL_GC_DEBUG
					//printk(KERN_INFO "GC++ bank %d",banknum);
#endif
					had_it = 1;
					
					
					
					if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/8)
					{
						if(gc_testing_on == 1)
						{
							gained_blks = do_gc_revmapt(mtdblk,banknum,GC_LEVEL2);
						}
						else 
						{
							gained_blks = do_gc(mtdblk,banknum,GC_LEVEL2);
						}
					}
					else if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/4)
					{
						if(gc_testing_on == 1)
						{
							gained_blks = do_gc_revmapt(mtdblk,banknum,GC_LEVEL1);
						}
						else
						{
							gained_blks = do_gc(mtdblk,banknum,GC_LEVEL1);
						}
					}
					else if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/2)
					{
						if(gc_testing_on == 1)
						{
							gained_blks = do_gc_revmapt(mtdblk,banknum,GC_LEVEL0);
						}
						else
						{
							gained_blks = do_gc(mtdblk,banknum,GC_LEVEL0);
						}
					}
					
			
					atomic_dec(&mtdblk->activity_matrix.gc_goingon[banknum]);	
#ifdef PLL_GC_DEBUG
					//printk(KERN_INFO "GC-- bank %d",banknum);			
#endif
				}
				
			}
			if(gained_blks > 0)
			{
				iteration_useless = 0;
				useless_iterations = 0;
			}
			if(can_sleep  == 1)
			{
				/* we have to Garbage collection
				* and did we had it done?*/
				if(have_to == 1)
				{
					if(had_it == 1)
					{
						/* yes!! done*/
						have_to = 0;
						had_it = 0;
					}
					else
					{
						/* no!! not done*/
						no_sleep = 1;
						can_sleep = 0;
					}
				}
			}
			
			
			
		}
		if(iteration_useless == 1)
		{
			useless_iterations++;
			//if(useless_iterations > (mtdblk->blks_per_bank/ITERATIONS_PER_GC_CALL))
			if(useless_iterations > (mtdblk->blks_per_bank/ITERATIONS_PER_GC_CALL))
			{
			//	printk(KERN_INFO "jif %u lastwr %u",jiffies,mtdblk->last_wr_time);
				if(jiffies_to_msecs(jiffies - mtdblk->last_wr_time) > 5000)
				{
					must_sleep = 1;
					useless_iterations = 0;
				}
			}

				
					
		}
		
#ifdef ADAPTIVE_GC
		
	/*cutshort GC phase*/
	int numbitset;
	int wakeupnum;
	int numiothreads,numgcthreads;
	int threshgcthrds;
	
	numbitset = bitmap_weight(mtdblk->mbd.active_iokthread, 64);
	numiothreads = numbitset;
	if(numiothreads == 0)
	{
		threshgcthrds = 8;
		if(prev_range == 0)
		num_times_in_same_range++;
		else
		prev_range = 0;
	}
	else if(numiothreads >= 1 &&  numiothreads < 10)
	{
		threshgcthrds = 4;
		if(prev_range == 1)
		num_times_in_same_range++;
		else
		prev_range = 1;
	}
	else
	//else if(numiothreads > 10)
	{
		threshgcthrds = 1;
		if(prev_range == 3)
		num_times_in_same_range++;
		else
		 prev_range = 3;
	}
#ifdef GC_DEBUG_ADAPTIVE
	printk(KERN_INFO "GC numbitsset %d ",numbitset);
#endif
	if(num_times_in_same_range > 3)
	{
#ifdef GC_DEBUG_ADAPTIVE
		printk(" rf to %d",prev_range);
#endif
		num_times_in_same_range = 0;
		cangcthrds = threshgcthrds;
		
		
	}
	
	
	numbitset = bitmap_weight(mtdblk->gc_active_map, 64);
	numgcthreads = numbitset;
	
	//if(activenumgcthread.counter < cangcthrds)
	if(numgcthreads < cangcthrds)
	{
		/* the master thread is the one that will wakeup other threads*/
		if(threadnum == 0)
		{
		/* if the iterations were useless , no need to restart new threads*/
		if(must_sleep != 1)
		{
			/* this is a rough number of threads to wakeup*/
			wakeupnum = cangcthrds - numgcthreads;
			if(wakeupnum < 0 || wakeupnum > NUM_GC_THREAD)
			{
				printk(KERN_INFO "wakeupnum wrong canggc = (%d) numbitset = %d wakupnum = (%d)",cangcthrds,numgcthreads,wakeupnum);
				BUG();
			}
			i = 0;
			//while(activenumgcthread.counter < cangcthrds)
			while (wakeupnum > 0)
			{
				if(i >= NUM_GC_THREAD)
				{
				//	printk(KERN_INFO "iterations (%d) > maxGCth(%d) and activethreads = %ld cangcthreads = %d",i,NUM_GC_THREAD,activenumgcthread.counter,cangcthrds);
					//BUG();
					break;
				}
				
				
				//if(task_is_stopped(mtdblk->ftlgc_thrd[i]))
				{
					/*Returns 1 if the process was woken up, 0 if it was already
					* running.*/
					if(wake_up_process(mtdblk->ftlgc_thrd[i]) == 1)
					{
						//atomic_inc(&activenumgcthread);
					//	printk(KERN_INFO "GC %d wakes up %d th thread ",threadnum,i);
						wakeupnum--;
					}
				}
#if 0
				if(activenumgcthread.counter < 0 || activenumgcthread.counter > NUM_GC_THREAD)
				{
					printk(KERN_INFO "active GC threads(%d) > (%d)",activenumgcthread.counter,NUM_GC_THREAD);
					BUG();
					numbitset = bitmap_weight(mtdblk->gc_active_map, 64);
					atomic_set(&activenumgcthread,numbitset);
				}
#endif
				
				i++;
			
			}
		}
		}
		
	}
	//else if(activenumgcthread.counter > cangcthrds)
	else if(numgcthreads > cangcthrds)
	{
		if(threadnum != 0)
		{
			must_sleep = 1;
		}
	
	}
	

	

#endif
		
		//if((!no_sleep) || can_sleep == 1 || must_sleep == 1)
		if(must_sleep == 1)
		{
			
#ifdef GC_DEBUG_ADAPTIVE
			printk(KERN_INFO "GC[%d] to sleep",threadnum);
#endif
			set_current_state(TASK_INTERRUPTIBLE);
			test_and_clear_bit(threadnum, mtdblk->gc_active_map);
			schedule();
			set_current_state(TASK_RUNNING);
			test_and_set_bit(threadnum, mtdblk->gc_active_map);
			
#ifdef GC_DEBUG_ADAPTIVE		
			printk(KERN_INFO "GC[%d] woken up",threadnum);
#endif
			must_sleep = 0;
		}
		loopcount++;
	
	}
}
#endif


void gc_thread(void *arg)
{
	struct mtdblk_dev *mtdblk = ((struct gc_threadinfo *)arg)->mtdblk;
	
	int banknum = ((struct gc_threadinfo *)arg)->banknum;
	
	if(gc_testing_on == 1)
	{
		if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/8)
		{
			
			do_gc_revmapt(mtdblk,banknum,GC_LEVEL2);
		}
		else if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/4)
		{
			
			do_gc_revmapt(mtdblk,banknum,GC_LEVEL1);
		}
		else if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/2)
		{
			
			do_gc_revmapt(mtdblk,banknum,GC_LEVEL0);
		}
		else
		{
			
		}
	}
	else
	{
		if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/8)
			do_gc(mtdblk,banknum,GC_LEVEL2);
		else if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/4)
			do_gc(mtdblk,banknum,GC_LEVEL1);
		else if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/2)
			do_gc(mtdblk,banknum,GC_LEVEL0);
	}
	atomic_dec(&mtdblk->activity_matrix.gc_goingon[banknum]);	
	atomic_dec(&num_gc_threads);
	
	kfree(arg);
}
//#if  !defined (NON_SCHEDULED)  && !defined (UNC_GC) 
#ifdef BG_C_GC
static uint64_t get_ppage(struct mtdblk_dev *mtdblk, int cur_wr_index,int from_gc_context)
{
	
	uint32_t ret_page_num;
	uint32_t next_blk;
	uint8_t tried;
#if 0
	int temp;
	int newalloc = 0;
	uint32_t cur_blk;
	int found = 0;
#endif
	static unsigned long temp_rand =0;
	static uint32_t selected_bank=0;
	uint32_t startbanknum;
	uint32_t banknum;
	
	uint32_t blkno,page_in_blk;
	int min_cost_bank;
	int min_cost;
	int cost;
	int OP;
	int selected;
	int i;
	
	
	if(cur_wr_index == RAND_SEL)
	{
		startbanknum = (selected_bank)%numpllbanks;
		banknum=(startbanknum+1)%numpllbanks;
		//cost = mtdblk->activity_matrix.gc_goingon[banknum].counter*cost_op[OP][GC_OP] + mtdblk->activity_matrix.num_writes[banknum].counter*cost_op[OP][WR_OP] + mtdblk->activity_matrix.num_reads_pref[banknum].counter*cost_op[OP][PFTCH_OP] + mtdblk->activity_matrix.num_reads[banknum].counter*cost_op[OP][RD_OP];
		
	//	min_cost = mtdblk->activity_matrix.num_writes[banknum].counter;
		//min_cost_bank = banknum;
		selected = 0;
		
		for(;  banknum != startbanknum;)
		{
			
		
#if 0
			
			OP = WR_OP ;
				

			cost = mtdblk->activity_matrix.gc_goingon[banknum].counter*cost_op[OP][GC_OP] + mtdblk->activity_matrix.num_writes[banknum].counter*cost_op[OP][WR_OP] + mtdblk->activity_matrix.num_reads_pref[banknum].counter*cost_op[OP][PFTCH_OP] + mtdblk->activity_matrix.num_reads[banknum].counter*cost_op[OP][RD_OP];
			
				
			if(cost < cost_thresh[banknum][OP])
			{ 
			
				selected = 1;
				break;
			}
			if(cost < min_cost)
			{
				min_cost = cost;
				min_cost_bank = banknum;
			}
#endif
			if(mtdblk->activity_matrix.gc_goingon[banknum].counter == 0 )
			{
				if(scheduled_for_gc[0] != banknum)
				{
					//if(mtdblk->activity_matrix.num_writes[banknum].counter== 0)
					{
						selected = 1;
						break;
					}
				}
			}
			//if(mtdblk->activity_matrix.num_writes[banknum].counter < min_cost)
			{
				//min_cost = mtdblk->activity_matrix.num_writes[banknum].counter;
				//min_cost_bank = banknum;
			}
			banknum = (banknum +1)%numpllbanks;
			
		}
		if(selected == 1)
		{
			selected_bank = banknum;
			
		}
		else
		{
			//selected_bank = min_cost_bank;
			//printk(KERN_INFO " no selected bank");
			get_random_bytes(&temp_rand, sizeof(temp_rand));
			selected_bank = temp_rand%(numpllbanks);
			//BUG();
			
		}
		cur_wr_index = selected_bank;
		//get_random_bytes(&temp_rand, sizeof(temp_rand));
		//cur_wr_index = temp_rand%(mtdblk->num_cur_wr_blks);
	}
	
	atomic_inc(&mtdblk->activity_matrix.num_writes[cur_wr_index]);
	
	#ifdef PLL_GC_DEBUG
	
	printk(KERN_INFO "selected bank = %d",cur_wr_index);		
	#endif
	down_write(&(mtdblk->cur_wr_state[cur_wr_index]));
	
	if((mtdblk->cur_writing[cur_wr_index].state == STATE_CLEAN) || (mtdblk->cur_writing[cur_wr_index].last_wrpage ==(mtdblk->pages_per_blk-1)))
	{
		next_blk = INVALID;
		tried = 0;
		
		
		if(!from_gc_context)
		{
			if(bank_info[cur_wr_index].perbank_nfree_blks.counter - mtdblk->reserved_blks_per_bank <= 0)
			{
				next_blk = INVALID;
			}
			else
			{
				tried++;
				next_blk = exper_alloc_block(mtdblk,cur_wr_index);
			}	
		}
		else
		{
			next_blk = exper_alloc_block(mtdblk,cur_wr_index);
							
		}
		
			
		
		if(next_blk == INVALID)
		{
			up_write(&(mtdblk->cur_wr_state[cur_wr_index]));
			atomic_dec(&mtdblk->activity_matrix.num_writes[cur_wr_index]);
			return INVALID_PAGE_NUMBER_32;
		}
		mtdblk->cur_writing[cur_wr_index].blk = next_blk;
		mtdblk->cur_writing[cur_wr_index].last_wrpage = 0;
		mtdblk->cur_writing[cur_wr_index].state = STATE_DIRTY;
		
		ret_page_num = mtdblk->cur_writing[cur_wr_index].blk* mtdblk->pages_per_blk + mtdblk->cur_writing[cur_wr_index].last_wrpage;
		
		up_write(&(mtdblk->cur_wr_state[cur_wr_index]));
		
		
		
		blkno = mtdblk->cur_writing[cur_wr_index].blk;
		page_in_blk = mtdblk->cur_writing[cur_wr_index].last_wrpage;
	
		//printk(KERN_INFO "bank = %d freeblks = %ld",cur_wr_index,bank_info[cur_wr_index].perbank_nfree_blks.counter);
#if 1
		if(bank_info[cur_wr_index].perbank_nfree_blks.counter < mtdblk->blks_per_bank/2)
		{
			//if(task_is_stopped(mtdblk->ftlgc_thrd))
	
#ifdef ADAPTIVE_GC
			
			//if(task_is_stopped(mtdblk->ftlgc_thrd[0]))
			{
			//	printk(KERN_INFO "waking up GCthread 0");
				/*Returns 1 if the process was woken up, 0 if it was already
				* running.*/
				wake_up_process(mtdblk->ftlgc_thrd[0]);
				
			}
#else
			for(i = 0; i < NUM_GC_THREAD;i++)
				wake_up_process(mtdblk->ftlgc_thrd[i]);
#endif
		}
	
	//	atomic_inc(&mtdblk->activity_matrix.num_writes[cur_wr_index]);
		
	#ifdef PLL_GC_DEBUG
		printk(KERN_INFO "%x: [%ld]num_wr++ = %ld",current->pid,cur_wr_index,mtdblk->activity_matrix.num_writes[cur_wr_index].counter);
	#endif
			
#endif
		return ret_page_num;
	}
	else
	{
		
		mtdblk->cur_writing[cur_wr_index].last_wrpage++;
		
		
		if(mtdblk->cur_writing[cur_wr_index].last_wrpage >= mtdblk->pages_per_blk)
		{
			printk(KERN_INFO "last_wr_page is wrong");
			BUG();
		}
				  
	
		ret_page_num = mtdblk->cur_writing[cur_wr_index].blk* mtdblk->pages_per_blk + mtdblk->cur_writing[cur_wr_index].last_wrpage;
		
		up_write(&(mtdblk->cur_wr_state[cur_wr_index]));
		
		blkno = mtdblk->cur_writing[cur_wr_index].blk;
		page_in_blk = mtdblk->cur_writing[cur_wr_index].last_wrpage;

				
#if 1			
		if(bank_info[cur_wr_index].perbank_nfree_blks.counter < mtdblk->blks_per_bank/2)
		{
		
#ifdef ADAPTIVE_GC
			
				
			//if(task_is_stopped(mtdblk->ftlgc_thrd[0]))
			{
			//	printk(KERN_INFO "waking up GCthread 0");
				/*Returns 1 if the process was woken up, 0 if it was already
				* running.*/
				wake_up_process(mtdblk->ftlgc_thrd[0]);
					
			}
#else
			for(i = 0; i < NUM_GC_THREAD;i++)
				wake_up_process(mtdblk->ftlgc_thrd[i]);
#endif
		}
			
		
		//atomic_inc(&mtdblk->activity_matrix.num_writes[cur_wr_index]);
		
		#ifdef PLL_GC_DEBUG
		printk(KERN_INFO "%x: [%ld]num_wr++ = %ld",current->pid,cur_wr_index,mtdblk->activity_matrix.num_writes[cur_wr_index].counter);
		#endif
#endif
		
		return ret_page_num;
	}

}
#endif



#ifdef SCHEDULED_EVERYTIME
static uint64_t get_ppage(struct mtdblk_dev *mtdblk, int cur_wr_index,int from_gc_context)
{
	
	uint32_t ret_page_num;
	uint32_t next_blk;
	uint8_t tried;
#if 0
	int temp;
	int newalloc = 0;
	uint32_t cur_blk;
	int found = 0;
#endif
	static unsigned long temp_rand =0;
	static uint32_t selected_bank=0;
	uint32_t startbanknum;
	uint32_t banknum;
	
	uint32_t blkno,page_in_blk;
	struct gc_threadinfo *gc_thread_arg;
	struct task_struct *gc_thrdtsk_struct;
	
	if(cur_wr_index == RAND_SEL)
	{
		startbanknum = (selected_bank)%numpllbanks;
		banknum=(startbanknum+1)%numpllbanks;
		for(;  banknum != startbanknum;)
		{
			
			if(!(mtdblk->activity_matrix.gc_goingon[banknum].counter))
			{
				break;
			}
			banknum = (banknum +1)%numpllbanks;
			
		}
		selected_bank = banknum;
		cur_wr_index = selected_bank;
		//get_random_bytes(&temp_rand, sizeof(temp_rand));
		//cur_wr_index = temp_rand%(mtdblk->num_cur_wr_blks);
	}
			
	down_write(&(mtdblk->cur_wr_state[cur_wr_index]));
	
	if((mtdblk->cur_writing[cur_wr_index].state == STATE_CLEAN) || (mtdblk->cur_writing[cur_wr_index].last_wrpage ==(mtdblk->pages_per_blk-1)))
	{
		next_blk = INVALID;
		tried = 0;
		
		{
			tried++;
			next_blk = exper_alloc_block(mtdblk,cur_wr_index);
							
		}
		
		if(next_blk == INVALID)
		{
			up_write(&(mtdblk->cur_wr_state[cur_wr_index]));
			return INVALID_PAGE_NUMBER;
		}
		mtdblk->cur_writing[cur_wr_index].blk = next_blk;
		mtdblk->cur_writing[cur_wr_index].last_wrpage = 0;
		mtdblk->cur_writing[cur_wr_index].state = STATE_DIRTY;
		
		ret_page_num = mtdblk->cur_writing[cur_wr_index].blk* mtdblk->pages_per_blk + mtdblk->cur_writing[cur_wr_index].last_wrpage;
		
		up_write(&(mtdblk->cur_wr_state[cur_wr_index]));
		
		
		
		blkno = mtdblk->cur_writing[cur_wr_index].blk;
		page_in_blk = mtdblk->cur_writing[cur_wr_index].last_wrpage;
	
		//printk(KERN_INFO "bank = %d freeblks = %ld",cur_wr_index,bank_info[cur_wr_index].perbank_nfree_blks.counter);
#if 1
		if(!from_gc_context)
		{
			if(num_gc_threads.counter < 2)
			{
				/* if a bank needs GC trigger it*/	
				startbanknum = (selected_bank)%numpllbanks;
				banknum=(startbanknum+1)%numpllbanks;
				for(;  banknum != startbanknum;)
				{
				
					if(banknum != cur_wr_index)
					{
						if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/2)
						{
//#ifdef ASSERT_ON
#if 1					
							if(mtdblk->activity_matrix.num_writes[banknum].counter > VIRGO_NUM_MAX_REQ_Q)
							{
								printk(KERN_INFO "num wr %ld > possible",mtdblk->activity_matrix.num_writes[banknum].counter);
								BUG();
							}
#endif
							if((!(mtdblk->activity_matrix.gc_goingon[banknum].counter))  && (mtdblk->activity_matrix.num_writes[banknum].counter < 3))
							{
								atomic_inc(&mtdblk->activity_matrix.gc_goingon[banknum]);	
								
								atomic_inc(&num_gc_threads);
								#if 0
			
								gc_thread_arg = kmalloc(sizeof(struct gc_threadinfo),GFP_KERNEL);
								if(gc_thread_arg == NULL)
								{
									printk(KERN_ERR "kmalloc ckpt_thread_arg fail");
									BUG();
								}
								
			
								gc_thread_arg->mtdblk = mtdblk;
								gc_thread_arg->banknum = banknum;
			
			
					
								gc_thrdtsk_struct  = kthread_run(gc_thread, (gc_thread_arg),
										"GCthrd %ld",banknum);
								if (IS_ERR(gc_thrdtsk_struct)) {
									PTR_ERR(gc_thrdtsk_struct);
									BUG();
								}
			
								#else
								if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/8)
								{
									
									do_gc_revmapt(mtdblk,banknum,GC_LEVEL2);
								}
								else if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/4)
								{
									
									do_gc_revmapt(mtdblk,banknum,GC_LEVEL1);
								}
								else if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/2)
								{
									
									do_gc_revmapt(mtdblk,banknum,GC_LEVEL0);
								}
								atomic_dec(&mtdblk->activity_matrix.gc_goingon[banknum]);	
								atomic_dec(&num_gc_threads);
								#endif
								break;
								
							}
					}
				}
				banknum = (banknum +1)%numpllbanks;
		
		
			}
		}
 		atomic_inc(&mtdblk->activity_matrix.num_writes[cur_wr_index]);
 		#ifdef PLL_GC_DEBUG
		printk(KERN_INFO "%x: [%ld]num_wr++ = %ld",current->pid,cur_wr_index,mtdblk->activity_matrix.num_writes[cur_wr_index].counter);
		#endif
		}
			
#endif
	
		 
		
		

		return ret_page_num;
	}
	else
	{
		
		mtdblk->cur_writing[cur_wr_index].last_wrpage++;
		
		
		if(mtdblk->cur_writing[cur_wr_index].last_wrpage >= mtdblk->pages_per_blk)
		{
			printk(KERN_INFO "last_wr_page is wrong");
			BUG();
		}
				  
	
		ret_page_num = mtdblk->cur_writing[cur_wr_index].blk* mtdblk->pages_per_blk + mtdblk->cur_writing[cur_wr_index].last_wrpage;
		
		up_write(&(mtdblk->cur_wr_state[cur_wr_index]));
		
		blkno = mtdblk->cur_writing[cur_wr_index].blk;
		page_in_blk = mtdblk->cur_writing[cur_wr_index].last_wrpage;
#if 1
		if(!from_gc_context)
		{
			if(num_gc_threads.counter < 2)
			{
				startbanknum = (selected_bank)%numpllbanks;
				banknum=(startbanknum+1)%numpllbanks;
				for(;  banknum != startbanknum;)
				{
		
					if(banknum != cur_wr_index)
					{
						if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/2)
						{
							if((!(mtdblk->activity_matrix.gc_goingon[banknum].counter))  && mtdblk->activity_matrix.num_writes[banknum].counter < 3)
							{
								atomic_inc(&mtdblk->activity_matrix.gc_goingon[banknum]);	
								
								
								atomic_inc(&num_gc_threads);
								#if 0
								gc_thread_arg = kmalloc(sizeof (struct gc_threadinfo),GFP_KERNEL);
								if(gc_thread_arg == NULL)
								{
									printk(KERN_ERR "kmalloc ckpt_thread_arg fail");
									BUG();
								}
								
			
								gc_thread_arg->mtdblk = mtdblk;
								gc_thread_arg->banknum = banknum;
			
			
					
								gc_thrdtsk_struct  = kthread_run(gc_thread, (gc_thread_arg),
										"GCthrd %ld",banknum);
								if (IS_ERR(gc_thrdtsk_struct)) {
									PTR_ERR(gc_thrdtsk_struct);
									BUG();
								}
			
								#else
								if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/8)
								{
									
									do_gc_revmapt(mtdblk,banknum,GC_LEVEL2);
								}
								else if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/4)
								{
									
									do_gc_revmapt(mtdblk,banknum,GC_LEVEL1);
								}
								else if(bank_info[banknum].perbank_nfree_blks.counter < mtdblk->blks_per_bank/2)
								{
									
									do_gc_revmapt(mtdblk,banknum,GC_LEVEL0);
								}
								
								atomic_dec(&mtdblk->activity_matrix.gc_goingon[banknum]);	
								atomic_dec(&num_gc_threads);
								#endif
								break;
								
							}
						}
					}
				banknum = (banknum +1)%numpllbanks;
			}
		
		}
		
		atomic_inc(&mtdblk->activity_matrix.num_writes[cur_wr_index]);
		#ifdef PLL_GC_DEBUG
		printk(KERN_INFO "%x: [%ld]num_wr++ = %ld",current->pid,cur_wr_index,mtdblk->activity_matrix.num_writes[cur_wr_index].counter);
		#endif
		}
		
#endif
		
		return ret_page_num;
	}
			
	
	
}

#endif

struct spare_buf_node {
	struct lfq_node_rcu list;
	struct rcu_head rcu;
	void *bufptr;
};


void free_sparebuf_node(struct rcu_head *head)
{
	struct spare_buf_node *node =
			container_of(head,  struct spare_buf_node, rcu);
	kfree(node);
}	

void *get_spare_buf()
{
	struct spare_buf_node *node;
	struct lfq_node_rcu *qnode;
	void *ret_buf;
	ret_buf = NULL;
	
	
	rcu_read_lock();
	qnode = lockfree_dequeue(&spare_bufQ);
	node = container_of(qnode, struct spare_buf_node, list);
	rcu_read_unlock();
	
	if(node != NULL)
	{
		ret_buf = node->bufptr;
		
	}
	else
	{
		printk(KERN_ERR "myftl: get_spare_buf alloc fail");
		BUG();
	}
	call_rcu(&node->rcu, free_sparebuf_node);	
	return ret_buf;
}



void put_spare_buf(void *ptr)
{
	struct spare_buf_node *node;
	
	
	node = kmalloc(sizeof(struct spare_buf_node),GFP_KERNEL);
				
	if (!node)
	{
		printk(KERN_ERR "myftl: sparebuf alloc fail");
		BUG();
	}
	node->bufptr = ptr;
	lfq_node_init_rcu(&node->list);
	rcu_read_lock();
	lockfree_enqueue(&spare_bufQ, &node->list);
	rcu_read_unlock();
}

void  put_spare_oobbuf(void *ptr)
{
	struct spare_buf_node *node;
	node = kmalloc(sizeof(struct spare_buf_node),GFP_KERNEL);
				
	if (!node)
	{
		printk(KERN_ERR "myftl: sparebuf alloc fail");
		BUG();
	}
	node->bufptr = ptr;
	lfq_node_init_rcu(&node->list);
	rcu_read_lock();
	lockfree_enqueue(&spare_oobbufQ, &node->list);
	rcu_read_unlock();
}

void *get_spare_oobbuf()
{
	struct spare_buf_node *node;
	struct lfq_node_rcu *qnode;
	void *ret_buf;
	ret_buf = NULL;
	
	rcu_read_lock();
	qnode = lockfree_dequeue(&spare_oobbufQ);
	node = container_of(qnode, struct spare_buf_node, list);
	rcu_read_unlock();
	
	
	if(node != NULL)
	{
		ret_buf = node->bufptr;
		
	}
	else
	{
		printk(KERN_ERR "myftl: getspareoobuf alloc fail");
		BUG();
	}
	call_rcu(&node->rcu, free_sparebuf_node);	
	return ret_buf;
	
	
}
		 

//#define JAN15_DEBUG 1

#define EMPTY_BUF 0
#define HALF_FULLBUF 1
#define FULL_BUF 2
#define NOT_SELECTED (-1)

#ifdef LRU

static int mtdblock_writesect(struct mymtd_blktrans_dev *dev,
			      uint64_t logic_ftl_blk, char *buf)	
{
	struct mtdblk_dev *mtdblk = container_of(dev, struct mtdblk_dev, mbd);
	struct mtd_info *mtd = mtdblk->mbd.mtd;
	
	uint64_t logic_page_num;
	
	uint8_t *new_temp_buf;
	size_t retlen;
	uint32_t page_shift;
	uint32_t  cache_buf,found_cache_buf;
	
	uint32_t sect_idx;
	uint32_t shift_val;
		
	uint64_t mask;
	uint64_t phy_addr;
	uint64_t bumped_lpn;
	uint64_t new_temp_buf_wmask;
	

	int flush = 0;
#if 0
	int stuck_lock1 = 0, stuck_lock2 = 0;
#endif
	int stuck = 0;
	int stuck_lock3 = 0;
	int search_success = 0;
	int i;
	int j;
	
	uint64_t phy_page_offs,old_phy_page_offs;
	uint8_t *rd_buf, *oob_buf,*new_oob_buf;
	uint32_t size_copied;	
	struct mtd_oob_ops ops;
	int res;
	struct oob_data oobvalues,*oobdata;
	struct cache_buf_list *cachebuf_ptr;
	struct cache_buf_list *qnode;		
			
	int retval;

	
	int selected_buf;
	static int unsigned countrmw = 0;

	
	mtdblk->last_wr_time = jiffies;
	
	shift_val = mtdblk->pageshift -mtdblk->blkshift;
	mask = ~(-1ULL<<shift_val);
	sect_idx = logic_ftl_blk&mask;
	
	logic_page_num = (logic_ftl_blk<<mtdblk->blkshift)>>mtdblk->pageshift;
	
#ifdef JAN15_DEBUG	
	printk(KERN_INFO "%x: wsect = %lld pgw = %lld sect_idx = %d",current->pid,logic_ftl_blk,logic_page_num,sect_idx);
	
#endif	
search_lookup_Tab:
	search_success  = 0;
	
	//mutex_lock(&mtdblk->buf_lookup_tab_mutex);
	for(i = 0; i < MAX_FTL_CACHEBUFS;i++)
	{
		if(buf_lookup_tab[i] == logic_page_num)
		{
			if(search_success == 1)
			{
				printk(KERN_INFO "%x: twice in buflookuptab %u",current->pid,logic_page_num);
				
				printk(KERN_INFO " ");

				for(j = 0; j < MAX_FTL_CACHEBUFS ;j++)
				{
					printk("%d ",buf_lookup_tab[j]);
				}
				
				BUG();
			}
			search_success = 1;
			found_cache_buf = i;
		}
	}
	//mutex_unlock(&mtdblk->buf_lookup_tab_mutex);
	
	
	
	if(search_success == 1)
	{
		cache_buf = found_cache_buf;
		mutex_lock(&(mtdblk->buf_lock[cache_buf]));
		if(mtdblk->cached_buf[cache_buf].flush_in_progress.counter)
		{
			printk(KERN_INFO "BUG: flush in progress while write");
			BUG();
		}
		atomic_inc(&mtdblk->cached_buf[cache_buf].writes_in_progress);
		
		memcpy((mtdblk->buf[cache_buf]+(sect_idx*mtdblk->blksize)),buf,mtdblk->blksize);
		set_bit(sect_idx,&(mtdblk->cached_buf[cache_buf].written_mask));


		if(mtdblk->cached_buf[cache_buf].cache_state == STATE_DIRTY && mtdblk->cached_buf[cache_buf].written_mask == mtdblk->cache_fullmask)
		{
			if(!mtdblk->cached_buf[cache_buf].flush_in_progress.counter)
			{
		
			
				mtdblk->cached_buf[cache_buf].cache_state = STATE_FULL;
				/*move to full_list*/

				
				cachebuf_ptr = cache_list_ptr[cache_buf];
				spin_lock(&dirty_bufs_list.lock);
				spin_lock(&fdirty_bufs_list.lock);
				list_lru_del(&dirty_bufs_list,&cachebuf_ptr->list);
				list_lru_add(&fdirty_bufs_list, &cachebuf_ptr->list);
				spin_unlock(&fdirty_bufs_list.lock);
				spin_unlock(&dirty_bufs_list.lock);
			
			}
			else
			{
				printk(KERN_INFO "BUG: flush2 in progress while write");
				BUG();
			}
		}
		mtdblk->cached_buf[cache_buf].last_touch = jiffies;
		atomic_dec(&mtdblk->cached_buf[cache_buf].writes_in_progress);
		mutex_unlock(&(mtdblk->buf_lock[cache_buf]));
	
	}
	else
	{
		/*set(lpn.buf_alloc_in_progress)*/
		/* test_and_set_bit returns 1 to try again ; 0 to continue*/
		if(test_and_set_bit(logic_page_num,page_bitmap))
		{
			schedule();
			printk(KERN_INFO " allocbuf in progress ; all the way back");
			goto search_lookup_Tab;
		}
		else
		{
			/* what the crap!!*/
			for(i = 0; i < MAX_FTL_CACHEBUFS;i++)
			{
				if(buf_lookup_tab[i] == logic_page_num)
				{
					printk(KERN_INFO "pessimistic_search pass; all the way back1");
					goto search_lookup_Tab;
				}
			}		
			
			/* 
			* buffer allocation
			* 1. try to get an empty buffer
			* 2. not possible, try to get a Full dirty buffer
			* 3. not possible, try to get a half dirty buffer
			*/
			
look_for_buf:
			selected_buf = NOT_SELECTED;
			/* try the empty buf*/
			spin_lock(&empty_bufs_list.lock);
			qnode = list_lru_deqhd(&empty_bufs_list);
			spin_unlock(&empty_bufs_list.lock);
			if(qnode != NULL)
			{
				cache_buf = qnode->value;
				selected_buf = EMPTY_BUF;
			}
			
			/* try the full buf*/
			if(selected_buf == NOT_SELECTED)
			{
				spin_lock(&fdirty_bufs_list.lock);
				qnode = list_lru_deqhd(&fdirty_bufs_list);
				spin_unlock(&fdirty_bufs_list.lock);
				if(qnode != NULL)
				{
					cache_buf = qnode->value;
					selected_buf = FULL_BUF;
				}
				
			}
			if(selected_buf == NOT_SELECTED)
			{
				mutex_lock(&(mtdblk->select_buf_lock));
				atomic_inc(&mtdblk->cache_assign_count);
				cache_buf = mtdblk->cache_assign_count.counter%MAX_FTL_CACHEBUFS;	
				mutex_unlock(&(mtdblk->select_buf_lock));
				selected_buf = HALF_FULLBUF;
#if 0
				qnode = list_lru_deqhd(&dirty_bufs_list);
				
				if(qnode != NULL)
				{
					
					
					cache_buf = qnode->value;
					selected_buf = HALF_FULLBUF;
				}
#endif			

				
			}
				
			if(cache_buf < 0 || cache_buf >= MAX_FTL_CACHEBUFS)
			{
				printk(KERN_INFO "myftl: cachebuf [%d] out of range",cache_buf);	
				BUG();
			}
		
			/* okay now try one of 3 paths, empty, FULLBUF or HALFBUF */
			if(selected_buf == EMPTY_BUF)
			{
	
				if(!(mtdblk->cached_buf[cache_buf].cache_state == STATE_EMPTY))
				{
					printk(KERN_INFO "Dequeued Buf not empty");
					BUG();
				}
				
				mutex_lock(&(mtdblk->buf_lock[cache_buf]));	
				
				mutex_lock(&mtdblk->buf_lookup_tab_mutex);
				buf_lookup_tab[cache_buf] = logic_page_num;
				mutex_unlock(&mtdblk->buf_lookup_tab_mutex);
					
				mtdblk->cached_buf[cache_buf].cache_state = STATE_DIRTY;
				mtdblk->cached_buf[cache_buf].written_mask = 0ULL;
				
				atomic_inc(&mtdblk->cached_buf[cache_buf].writes_in_progress);
				
				memcpy((mtdblk->buf[cache_buf]+(sect_idx*mtdblk->blksize)),buf,mtdblk->blksize);
				set_bit(sect_idx,&(mtdblk->cached_buf[cache_buf].written_mask));
	
				if(mtdblk->cached_buf[cache_buf].cache_state == STATE_DIRTY && mtdblk->cached_buf[cache_buf].written_mask == mtdblk->cache_fullmask)
				{
					if(!mtdblk->cached_buf[cache_buf].flush_in_progress.counter)
					{
			
				
						mtdblk->cached_buf[cache_buf].cache_state = STATE_FULL;
						/*move to full_list*/
	
						cachebuf_ptr = cache_list_ptr[cache_buf];
						spin_lock(&fdirty_bufs_list.lock);
						list_lru_add(&fdirty_bufs_list, &cachebuf_ptr->list);
						spin_unlock(&fdirty_bufs_list.lock);
							
					}
				}
				mtdblk->cached_buf[cache_buf].last_touch = jiffies;
				atomic_dec(&mtdblk->cached_buf[cache_buf].writes_in_progress);
				
				mutex_unlock(&(mtdblk->buf_lock[cache_buf]));
													
			}
			else if(selected_buf == FULL_BUF)
			{
				/* FIFO mechanism*/
	
						
				if(mtdblk->cached_buf[cache_buf].written_mask != mtdblk->cache_fullmask)
				{
					printk(KERN_INFO "Deqd Full buf not full");
					//BUG();
					goto look_for_buf;
					
				}
				
				mutex_lock(&(mtdblk->buf_lock[cache_buf]));
				/* 
				* check for writes in progress
				* set flush  in progress
				* change buffers , change map table
				*/
				atomic_inc(&mtdblk->cached_buf[cache_buf].flush_in_progress);
				
				stuck = 0;
				while(mtdblk->cached_buf[cache_buf].writes_in_progress.counter)
				{
					if(stuck_lock3%10000 == 0)
					{
						printk(KERN_INFO "myftl: stuck_lockup3 %d %d %x",cache_buf,mtdblk->cached_buf[cache_buf].writes_in_progress.counter,current->pid);
					}
					stuck_lock3++;
					stuck = 1;
			
			
					schedule();
				}
				if(stuck != 0)
				{
					printk(KERN_INFO "myftl: out of stuck_lockup3 %d %d %x",cache_buf,mtdblk->cached_buf[cache_buf].writes_in_progress.counter,current->pid);
				}
				
				new_temp_buf = mtdblk->buf[cache_buf];
				//mtdblk->buf[cache_buf] = vmalloc(mtdblk->cache_size);	
				mtdblk->buf[cache_buf] = get_spare_buf();		
				if(mtdblk->buf[cache_buf] == NULL)
				{
					printk(KERN_INFO "vmalloc fail");
					BUG();
				}
				
				new_temp_buf_wmask = mtdblk->cached_buf[cache_buf].written_mask;
				bumped_lpn = buf_lookup_tab[cache_buf];
				
				
				mutex_lock(&mtdblk->buf_lookup_tab_mutex);
				buf_lookup_tab[cache_buf] = logic_page_num;
				mutex_unlock(&mtdblk->buf_lookup_tab_mutex);
	
					
				mtdblk->cached_buf[cache_buf].written_mask = 0ULL;
				mtdblk->cached_buf[cache_buf].cache_state = STATE_DIRTY;
			
				flush = 1;
				
				atomic_dec(&mtdblk->cached_buf[cache_buf].flush_in_progress);
				
				atomic_inc(&mtdblk->cached_buf[cache_buf].writes_in_progress);
				memcpy((mtdblk->buf[cache_buf]+(sect_idx*mtdblk->blksize)),buf,mtdblk->blksize);
				set_bit(sect_idx,&(mtdblk->cached_buf[cache_buf].written_mask));
	
				if(mtdblk->cached_buf[cache_buf].cache_state == STATE_DIRTY && mtdblk->cached_buf[cache_buf].written_mask == mtdblk->cache_fullmask)
				{
					if(!mtdblk->cached_buf[cache_buf].flush_in_progress.counter)
					{
			
				
						mtdblk->cached_buf[cache_buf].cache_state = STATE_FULL;
								/*move to full_list*/
	
						cachebuf_ptr = cache_list_ptr[cache_buf];
						spin_lock(&fdirty_bufs_list.lock);
						list_lru_add(&fdirty_bufs_list, &cachebuf_ptr->list);
						spin_unlock(&fdirty_bufs_list.lock);
				
							
				
					}
				}
				
				atomic_dec(&mtdblk->cached_buf[cache_buf].writes_in_progress);
				mutex_unlock(&(mtdblk->buf_lock[cache_buf]));
					
						
			}			
			else if(selected_buf == HALF_FULLBUF)
			{
	
				
				
				mutex_lock(&(mtdblk->buf_lock[cache_buf]));				
				/* 
				* check for writes in progress
				* set flush  in progress
				* change buffers , change map table
				*/
				
				atomic_inc(&mtdblk->cached_buf[cache_buf].flush_in_progress);
				stuck = 0;
				while(mtdblk->cached_buf[cache_buf].writes_in_progress.counter)
				{
					if(stuck_lock3%10000 == 0)
					{
						printk(KERN_INFO "myftl: stuck_lockup3 %d %d %x",cache_buf,mtdblk->cached_buf[cache_buf].writes_in_progress.counter,current->pid);
					}
					stuck_lock3++;
					stuck = 1;
			
			
					schedule();
				}
				if(stuck != 0)
				{
					printk(KERN_INFO "myftl: out of stuck_lockup3 %d %d %x",cache_buf,mtdblk->cached_buf[cache_buf].writes_in_progress.counter,current->pid);
				}
	
				
				new_temp_buf = mtdblk->buf[cache_buf];
				new_temp_buf_wmask = mtdblk->cached_buf[cache_buf].written_mask;
				bumped_lpn = buf_lookup_tab[cache_buf];
					
				//mtdblk->buf[cache_buf] = vmalloc(mtdblk->cache_size);	
				mtdblk->buf[cache_buf] = get_spare_buf();
				if(mtdblk->buf[cache_buf] == NULL)
				{
					printk(KERN_INFO "sparebuf alloc fail");
					BUG();
				}		
				
				mtdblk->cached_buf[cache_buf].written_mask = 0;
				mtdblk->cached_buf[cache_buf].cache_state = STATE_DIRTY;
				
				mutex_lock(&mtdblk->buf_lookup_tab_mutex);
				buf_lookup_tab[cache_buf] = logic_page_num;
				mutex_unlock(&mtdblk->buf_lookup_tab_mutex);
	
				flush = 1;
				atomic_dec(&mtdblk->cached_buf[cache_buf].flush_in_progress);
				
				atomic_inc(&mtdblk->cached_buf[cache_buf].writes_in_progress);
				memcpy((mtdblk->buf[cache_buf]+(sect_idx*mtdblk->blksize)),buf,mtdblk->blksize);
				set_bit(sect_idx,&(mtdblk->cached_buf[cache_buf].written_mask));
	
				if(mtdblk->cached_buf[cache_buf].cache_state == STATE_DIRTY && mtdblk->cached_buf[cache_buf].written_mask == mtdblk->cache_fullmask)
				{
					if(!mtdblk->cached_buf[cache_buf].flush_in_progress.counter)
					{
	
	
						mtdblk->cached_buf[cache_buf].cache_state = STATE_FULL;
						
						/*move to full_list*/
						cachebuf_ptr = cache_list_ptr[cache_buf];
						spin_lock(&fdirty_bufs_list.lock);
						list_lru_add(&fdirty_bufs_list, &cachebuf_ptr->list);
						spin_unlock(&fdirty_bufs_list.lock);
						
				
					}
				}
				mtdblk->cached_buf[cache_buf].last_touch = jiffies;
				atomic_dec(&mtdblk->cached_buf[cache_buf].writes_in_progress);
				mutex_unlock(&(mtdblk->buf_lock[cache_buf]));
										
			}
			else 
			{
				printk("Selected buf neither empty full or half");
				BUG();
						
			}
		
		
		test_and_clear_bit(logic_page_num,page_bitmap);
	
	}/* else (test and set)*/
		
		
	}/*else (search_success == 1)*/
	
	/* at this point only need to protect the map table correctly */
	
	if(flush == 1)
	{

		if(new_temp_buf_wmask != mtdblk->cache_fullmask)
		{

			countrmw++;
			/* not all sectors here are new
			 * do merge with flash
			 * and write to new location
			 */
#if 1
			printk(KERN_INFO "%x: read modify %d count = %u",current->pid, cache_buf,countrmw);
			printk(KERN_INFO "cache_buf mask = %llx",new_temp_buf_wmask);
			if(countrmw > 64)
			{
				printk(KERN_INFO "okay stop here");
				BUG();
			}
			
#endif

			down_read(&(map_tabl_lock));
			phy_page_offs = map_table[bumped_lpn];
			up_read(&(map_tabl_lock));
			if(phy_page_offs == INVALID_PAGE_NUMBER)
			{
				printk(KERN_INFO "myftl: wsect %ul %ul",bumped_lpn,phy_page_offs);
				BUG();
			}
	
#ifdef ASSERT_ON
			if((bumped_lpn > (mtdblk->pages_per_blk * mtdblk->num_blks)) || (phy_page_offs > (mtdblk->pages_per_blk * mtdblk->num_blks)))
			{
				printk(KERN_INFO "myftl: ASSERT flush_toflash logic_page_offs %ul %ul  > %ul",bumped_lpn,phy_page_offs,(mtdblk->pages_per_blk * mtdblk->num_blks));
				BUG();
			}
#endif
		
			//rd_buf = vmalloc(mtd->writesize);
			rd_buf = get_spare_buf();
			if (!rd_buf)
			{
				printk(KERN_INFO "myftl: vmalloc fail");
				BUG();

			}
			//oob_buf = vmalloc(mtd->oobsize);
			oob_buf = get_spare_oobbuf();
			if (!oob_buf)
			{
				printk(KERN_INFO "myftl: vmalloc fail");
				BUG();

			}

			/* should be optimised to read in 4KB
			 * should be optimised to store the read value 
			 */
	
			ops.mode = MTD_OOB_AUTO;
			ops.datbuf = rd_buf;
			ops.len = mtd->writesize;
			ops.oobbuf = oob_buf;
			ops.ooboffs = 0;	
			ops.ooblen = mtd->oobsize;


			res = mtd->read_oob(mtd,phy_page_offs<<mtdblk->pageshift, &ops);
			if(ops.retlen < mtd->writesize)
			{
				printk(KERN_ERR "myftl: merge_with_flash read failure");
				return -1;
			}


			mask = 1;
			size_copied = 0;
			sect_idx = 0;

			while(size_copied < mtdblk->cache_size)
			{
				if(((mask) & (new_temp_buf_wmask)) == 0)
				{

					memcpy(new_temp_buf +sect_idx*mtdblk->blksize,rd_buf+sect_idx*mtdblk->blksize,mtdblk->blksize);	

				}
				mask = mask <<1;
				sect_idx++;
				size_copied += mtdblk->blksize;

			}
			put_spare_buf(rd_buf);
			put_spare_oobbuf(oob_buf);
			//vfree(rd_buf);
			//vfree(oob_buf);
		}


		tried = 0; phy_addr = INVALID_PAGE_NUMBER;
		while(tried < (numpllbanks*2) && phy_addr == INVALID_PAGE_NUMBER)
		{
			phy_addr = get_ppage(mtdblk,RAND_SEL,0);
			tried++;
		}
#ifndef USE_VIRGO_RESTRICTED
#ifndef EIGHT_BANK_FLASH
		if(phy_addr >= (mtdblk->pages_per_blk * mtdblk->num_blks))
		{
			printk(KERN_INFO "myftl: ASSERT new_writesect phyaddr %ul >= %ul",phy_addr,(mtdblk->pages_per_blk * mtdblk->num_blks));
			BUG();
		}
#endif
#endif


		if(phy_addr == INVALID_PAGE_NUMBER)
		{
			printk(KERN_INFO "myftl: ASSERT new_writesect phyaddr %ul",phy_addr);
			BUG();
		}
		


		if(phy_addr != INVALID_PAGE_NUMBER){
		/*physical page write to medium. do we need a lock here?*/

			page_shift = mtdblk->pageshift;
	
			//new_oob_buf = vmalloc(mtd->oobsize);
	
			new_oob_buf = get_spare_oobbuf();
			if (!new_oob_buf)
			{
				printk(KERN_INFO "myftl deinit: vmalloc fail");
				BUG();
					
			}
	
			oobdata = &oobvalues;
	
			atomic_inc(&mtdblk->seq_num);
			oobdata->seq_number = mtdblk->seq_num.counter;
			oobdata->logic_page_num = bumped_lpn;
			oobdata->blk_type = DATA_BLK;
			memcpy(new_oob_buf,oobdata,sizeof(*oobdata));
				
		
			ops.mode = MTD_OOB_AUTO;
			ops.ooblen = mtd->oobsize;
			ops.len = mtd->writesize;
			ops.retlen = 0;
			ops.oobretlen = 0;
			ops.ooboffs = 0;
			ops.datbuf = new_temp_buf;
			ops.oobbuf = new_oob_buf;
			retval = 1;
		
			retval = mtd->write_oob(mtd,(phy_addr<<page_shift), &ops);
				
	//		mtd->write(mtd,(phy_addr<<page_shift),mtdblk->cache_size,&retlen,new_temp_buf);
	
			//if(retval != 0)
			if(ops.retlen != mtd->writesize)
			{
	
				printk("myftl: mtd write %llx  %ld %d %d fail",phy_addr<<page_shift,phy_addr,sizeof(*oobdata),ops.retlen);
				vfree(new_temp_buf);
				BUG();			
				return -1;
			}
	
	
	
			uint32_t oldblkno,page_in_blk;
			uint32_t newblkno,bank_num;
	
			old_phy_page_offs = map_table[bumped_lpn];
			oldblkno = old_phy_page_offs/(mtdblk->pages_per_blk);
			page_in_blk = old_phy_page_offs%(mtdblk->pages_per_blk);
			
			bank_num = oldblkno/mtdblk->hwblks_per_bank;
#if 0
			#ifdef USE_VIRGO_RESTRICTED
	 		bank_num = oldblkno/4096;
			#else
			bank_num = oldblkno/mtdblk->blks_per_bank;
			#endif
#endif
#ifdef EIGHT_BANK_FLASH
			bank_num = bank_num/8;
#endif
			test_and_clear_bit(page_in_blk,blk_info[oldblkno].valid_pages_map);
			atomic_dec(&blk_info[oldblkno].num_valid_pages);
			atomic_inc(&bank_info[bank_num].perbank_ndirty_pages);
	
			down_write(&(map_tabl_lock));
			map_table[bumped_lpn] =  phy_addr;
			up_write(&(map_tabl_lock));
	
			newblkno = phy_addr/(mtdblk->pages_per_blk);
			page_in_blk = phy_addr%(mtdblk->pages_per_blk);
			test_and_set_bit(page_in_blk,blk_info[newblkno].valid_pages_map);
			atomic_inc(&blk_info[newblkno].num_valid_pages);
	
			put_spare_buf(new_temp_buf);
			put_spare_oobbuf(new_oob_buf);
			//vfree(new_temp_buf);
			//vfree(new_oob_buf);
				
		}
		else
		{
			printk(KERN_INFO "myftl: ASSERT new_writesect phyaddr %ul",phy_addr);
			BUG();
		}


			
	}
#if 0
	printk(KERN_INFO "writesect done");
#endif
	return 0;
}						 
	
#endif


#define BUF_HOLDTIME (60000)
void wbuf_flush_thread(struct mtdblk_dev *mtdblk)
{
	uint64_t wait_jiffies = -1ULL;
	int buf_num;
	
#ifdef copied
	uint64_t logic_page_num;
	size_t retlen;
	
	uint32_t shift_val;
	int selected_buf;
	struct lfq_node_rcu *qnode;
	struct cache_num_node *node;
	
#endif
	uint8_t *new_temp_buf;
	uint64_t new_temp_buf_wmask;
	struct oob_data oobvalues,*oobdata;
	uint8_t *rd_buf, *oob_buf,*new_oob_buf;
	uint64_t bumped_lpn;
	int res;
	uint32_t sect_idx;
	/* needed 64bit as we do some shifting*/
	uint64_t phy_addr;
	uint32_t size_copied;		
	uint64_t mask;
	uint64_t phy_page_offs,old_phy_page_offs;
	uint32_t page_shift = mtdblk->pageshift;
	struct mtd_oob_ops ops;
	struct mtd_info *mtd = mtdblk->mbd.mtd;
	int retval;
	
	
	struct cache_num_node *node;
	
	while (!kthread_should_stop()) {
		for(buf_num = 0; buf_num < MAX_FTL_CACHEBUFS;buf_num++)
		{
			if(mtdblk->cached_buf[buf_num].cache_state != STATE_EMPTY)
			{
				if(jiffies   <  mtdblk->cached_buf[buf_num].last_touch  + msecs_to_jiffies(BUF_HOLDTIME))
				{
					/* never mind*/
				}	
				else
				{
					
							
		
				/* what are we doing here?
					* 1. should we merge the buffer with flash for the bumped)_lpn
					; if so read from the flash maptab[bumpedlpn]; 
					; what if this lpn was a new write
					; and merge;
					*  2. get a phy page for the writing the buffer of bumpedlpn
					; what happens if there is no phypage
					*  3. write to  the phypage of flash, the newtempbuf
					*  4. get the oldphypageoffs of maptab[bumpedlpn]
					; what happens if this is a new  write
					; make the oldpage dirty, change vpagesbitmap, numfreepagecount;
					*   5. maptab[bumpedlpn] = newphypage
					*   	; modify vpagebitmap, numfreepagecount
					* 
				*/
					mutex_lock(&(mtdblk->buf_lock[buf_num]));
					new_temp_buf = mtdblk->buf[buf_num];
			
					mtdblk->buf[buf_num] = get_spare_buf();		
					if(mtdblk->buf[buf_num] == NULL)
					{
						printk(KERN_INFO "vmalloc fail");
						BUG();
					}
			
					new_temp_buf_wmask = mtdblk->cached_buf[buf_num].written_mask;
					bumped_lpn = buf_lookup_tab[buf_num];
			
					bumped_lpn = buf_lookup_tab[buf_num];	
					mutex_lock(&mtdblk->buf_lookup_tab_mutex);
					buf_lookup_tab[buf_num] = INVALID_PAGE_NUMBER_32;
					mutex_unlock(&mtdblk->buf_lookup_tab_mutex);
#if 0
					printk(KERN_INFO "%x: [%d]buflkuptab=%u ",current->pid,cache_buf,logic_page_num);
#endif
				
					mtdblk->cached_buf[buf_num].written_mask = 0ULL;
					mtdblk->cached_buf[buf_num].cache_state = STATE_EMPTY;
					mtdblk->cached_buf[buf_num].logic_page = INVALID_PAGE_NUMBER_32;
					mtdblk->cached_buf[buf_num].last_touch = jiffies;
					mutex_unlock(&(mtdblk->buf_lock[buf_num]));
					
					
					
					
					
					
				
				
				
					if(new_temp_buf_wmask != mtdblk->cache_fullmask)
					{
					
					
					
					
					/* not all sectors here are new
						* do merge with flash
						* and write to new location
					*/
		
		
					//down_read(&(map_tabl_lock));
						while (test_and_set_bit(bumped_lpn, maptab_bitmap) != 0)
						{
							schedule();
						}
						phy_page_offs = map_table[bumped_lpn];
					//up_read(&(map_tabl_lock));
						if (test_and_clear_bit(bumped_lpn, maptab_bitmap) == 0)
						{
							printk(KERN_INFO "mapbitmap cleared wrong");
							BUG();
						}
						if(gc_testing_on == 1)
						{
					
							if(phy_page_offs == INVALID_PAGE_NUMBER)
							{
								printk(KERN_INFO "myftl: wsect %ul %ul",bumped_lpn,phy_page_offs);
								goto the_write_part;
							}
						}
						else
						{
							if(phy_page_offs == INVALID_PAGE_NUMBER)
							{
								printk(KERN_INFO "myftl: wsect %ul %ul",bumped_lpn,phy_page_offs);
								BUG();
							}
						}
			
			
#ifdef ASSERT_ON
#ifndef USE_VIRGO_RESTRICTED
#ifndef EIGHT_BANK_FLASH
						if((bumped_lpn > (mtdblk->pages_per_blk * mtdblk->num_blks)) || (phy_page_offs > (mtdblk->pages_per_blk * mtdblk->num_blks)))
						{
							printk(KERN_INFO "myftl: ASSERT flush_toflash logic_page_offs %ul %ul  > %ul",bumped_lpn,phy_page_offs,(mtdblk->pages_per_blk * mtdblk->num_blks));
							BUG();
						}
#endif
#endif
#endif
		
		
				
		
				
					//rd_buf = vmalloc(mtd->writesize);
						rd_buf = get_spare_buf();
						if (!rd_buf)
						{
							printk(KERN_INFO "myftl: vmalloc fail");
							BUG();
			//return -EINTR;
						}
					//oob_buf = vmalloc(mtd->oobsize);
						oob_buf = get_spare_oobbuf();
						if (!oob_buf)
						{
							printk(KERN_INFO "myftl: vmalloc fail");
							BUG();
			//return -EINTR;
						}
			
			
						ops.mode = MTD_OOB_AUTO;
						ops.datbuf = rd_buf;
						ops.len = mtd->writesize;
						ops.oobbuf = oob_buf;
						ops.ooboffs = 0;	
						ops.ooblen = mtd->oobsize;
		
		
						res = mtd->read_oob(mtd,phy_page_offs<<mtdblk->pageshift, &ops);
						if(ops.retlen < mtd->writesize)
						{
							printk(KERN_ERR "myftl: merge_with_flash read failure");
							return -1;
						}
			
			
						mask = 1;
						size_copied = 0;
						sect_idx = 0;
		
						while(size_copied < mtdblk->cache_size)
						{
							if(((mask) & (new_temp_buf_wmask)) == 0)
							{
			
								memcpy(new_temp_buf +sect_idx*mtdblk->blksize,rd_buf+sect_idx*mtdblk->blksize,mtdblk->blksize);	
			
							}
							mask = mask <<1;
							sect_idx++;
							size_copied += mtdblk->blksize;
			
						}
						put_spare_buf(rd_buf);
						put_spare_oobbuf(oob_buf);
					//vfree(rd_buf);
					//vfree(oob_buf);
					}
		the_write_part:
				;
		int tried = 0; phy_addr = INVALID_PAGE_NUMBER;
		while(tried < (numpllbanks*2) && phy_addr == INVALID_PAGE_NUMBER)
		//		while(tried < 128 && phy_addr == INVALID_PAGE_NUMBER)
		{
			phy_addr = get_ppage(mtdblk,RAND_SEL,0);
			tried++;
		}
		
				
		
#ifndef USE_VIRGO_RESTRICTED
#ifndef EIGHT_BANK_FLASH
		if(phy_addr >= (mtdblk->pages_per_blk * mtdblk->num_blks))
		{
			printk(KERN_INFO "myftl: ASSERT new_writesect phyaddr %ul >= %ul",phy_addr,(mtdblk->pages_per_blk * mtdblk->num_blks));
			BUG();
		}
#endif
#endif	
		if(phy_addr == INVALID_PAGE_NUMBER)
		{
			printk(KERN_INFO "myftl: ASSERT new_writesect phyaddr %ul",phy_addr);
			BUG();
		}
		if(phy_addr >= 16777216)
		{
			printk(KERN_INFO "myftl: ASSERT new_writesect wr phyaddr %ul",phy_addr);
			BUG();
		}
				
		
		uint32_t oldblkno,page_in_blk;
		uint32_t newblkno,bank_num;
		int banknum;
				
		if(phy_addr != INVALID_PAGE_NUMBER){
			/*physical page write to medium. do we need a lock here?*/
		
			page_shift = mtdblk->pageshift;
					
			banknum = phy_addr/(mtdblk->pages_per_blk*mtdblk->hwblks_per_bank);
		
		
		
#ifdef EIGHT_BANK_FLASH
			banknum = banknum/8;
#endif
					
					
					//new_oob_buf = vmalloc(mtd->oobsize);
			new_oob_buf = get_spare_oobbuf();
				
			if (!new_oob_buf)
			{
				printk(KERN_INFO "myftl deinit: vmalloc fail");
				BUG();
						//return -EINTR;
			}
		
			oobdata = &oobvalues;
					
			atomic_inc(&mtdblk->seq_num);
			oobdata->seq_number = mtdblk->seq_num.counter;
			oobdata->logic_page_num = bumped_lpn;
			oobdata->blk_type = DATA_BLK;
			memcpy(new_oob_buf,oobdata,sizeof(*oobdata));
					
		
					
					
			if(mtdblk->activity_matrix.gc_goingon[banknum].counter == 1)
			{
		
				atomic_inc(&gc_on_writes_collisions);
			}
					
					
			ops.mode = MTD_OOB_AUTO;
			ops.ooblen = mtd->oobsize;
			ops.len = mtd->writesize;
			ops.retlen = 0;
			ops.oobretlen = 0;
			ops.ooboffs = 0;
			ops.datbuf = new_temp_buf;
			ops.oobbuf = new_oob_buf;
			retval = 1;
				
			retval = mtd->write_oob(mtd,(phy_addr<<page_shift), &ops);
					
		
			if(ops.retlen != mtd->writesize)
			{
		
				printk("myftl: mtd write %llx  %ld %d %d fail",phy_addr<<page_shift,phy_addr,sizeof(*oobdata),ops.retlen);
				vfree(new_temp_buf);
				BUG();			
				return -1;
			}
					
					
		
					
				
				
			if(bumped_lpn >= 16777216)
			{
				printk(KERN_INFO "myftl: ASSERT bumped_lpn wr %ul",bumped_lpn);
				BUG();
			}
			old_phy_page_offs = map_table[bumped_lpn];
			if(gc_testing_on == 0)
			{
				if(old_phy_page_offs >= 16777216)
				{
					printk(KERN_INFO "myftl: ASSERT old_phy_page_offs wr %ul",old_phy_page_offs);
					BUG();
				}
				oldblkno = old_phy_page_offs/(mtdblk->pages_per_blk);
				page_in_blk = old_phy_page_offs%(mtdblk->pages_per_blk);
				if(page_in_blk >= 64)
				{
					printk(KERN_INFO "myftl: ASSERT page_in_blk wr %ul",page_in_blk);
					BUG();
				}
				if(oldblkno >= 262144)
				{
					printk(KERN_INFO "myftl: ASSERT oldblkno wr %ul",oldblkno);
					BUG();
				}
				bank_num = oldblkno/mtdblk->hwblks_per_bank;
		
#ifdef EIGHT_BANK_FLASH
				bank_num = bank_num/8;
#endif
					
				test_and_clear_bit(page_in_blk,blk_info[oldblkno].valid_pages_map);
				atomic_dec(&blk_info[oldblkno].num_valid_pages);
				atomic_inc(&bank_info[bank_num].perbank_ndirty_pages);
			}
			else 
			{
				if(old_phy_page_offs == INVALID_PAGE_NUMBER_32)
				{
					printk(KERN_INFO "myftl: ASSERT old_phy_page_offs wr %ul",old_phy_page_offs);
					goto 	modifymaptab;
				}
				if(old_phy_page_offs >= 16777216)
				{
					printk(KERN_INFO "myftl: ASSERT old_phy_page_offs wr %ul",old_phy_page_offs);
					BUG();
				}
				oldblkno = old_phy_page_offs/(mtdblk->pages_per_blk);
				page_in_blk = old_phy_page_offs%(mtdblk->pages_per_blk);
				if(page_in_blk >= 64)
				{
					printk(KERN_INFO "myftl: ASSERT page_in_blk wr %ul",page_in_blk);
					BUG();
				}
				if(oldblkno >= 262144)
				{
					printk(KERN_INFO "myftl: ASSERT oldblkno wr %ul",oldblkno);
					BUG();
				}
				bank_num = oldblkno/mtdblk->hwblks_per_bank;
		
#ifdef EIGHT_BANK_FLASH
				bank_num = bank_num/8;
#endif
					
				test_and_clear_bit(page_in_blk,blk_info[oldblkno].valid_pages_map);
				atomic_dec(&blk_info[oldblkno].num_valid_pages);
				atomic_inc(&bank_info[bank_num].perbank_ndirty_pages);
			}
					
			modifymaptab:		
					//down_write(&(map_tabl_lock));
					while (test_and_set_bit(bumped_lpn, maptab_bitmap) != 0)
					{
						schedule();
					}
					map_table[bumped_lpn] =  phy_addr;
					
		
					if(gc_testing_on ==1)
						reverse_map_tab[phy_addr] = bumped_lpn;
					
		
					
					//up_write(&(map_tabl_lock));
					if (test_and_clear_bit(bumped_lpn, maptab_bitmap) == 0)
					{
						printk(KERN_INFO "mapbitmap cleared wrong");
						BUG();
					}
					
					if(phy_addr >= 16777216)
					{
						printk(KERN_INFO "myftl: ASSERT phy_addr wr %ul",old_phy_page_offs);
						BUG();
					}
		
					newblkno = phy_addr/(mtdblk->pages_per_blk);
					page_in_blk = phy_addr%(mtdblk->pages_per_blk);
					
					if(page_in_blk >= 64)
					{
						printk(KERN_INFO "myftl: ASSERT page_in_blk wr %ul",page_in_blk);
						BUG();
					}
					if(newblkno  >= 262144)
					{
						printk(KERN_INFO "myftl: ASSERT newblkno  wr %ul",oldblkno);
						BUG();
					}
					test_and_set_bit(page_in_blk,blk_info[newblkno].valid_pages_map);
					atomic_inc(&blk_info[newblkno].num_valid_pages);
					
					put_spare_buf(new_temp_buf);
					put_spare_oobbuf(new_oob_buf);
					//vfree(new_temp_buf);
					//vfree(new_oob_buf);
					
		
					banknum = phy_addr/(mtdblk->pages_per_blk*mtdblk->hwblks_per_bank);
		
		
		
#ifdef EIGHT_BANK_FLASH
					banknum = banknum/8;
#endif
		
					
		
					//#ifndef NON_SCHEDULED
					
#if defined (BG_C_GC) || defined (BG_UNC_GC)
					atomic_dec(&mtdblk->activity_matrix.num_writes[banknum]);
					
#ifdef PLL_GC_DEBUG
					printk(KERN_INFO "%x: [%ld]num_wr-- = %ld",current->pid,banknum,mtdblk->activity_matrix.num_writes[banknum].counter);
#endif
#endif
					
		}
		else
		{
			printk(KERN_INFO "myftl: ASSERT new_writesect phyaddr %ul",phy_addr);
			BUG();
		}
		

		
	
					
		/* now add to the empty buffers */
		node = kmem_cache_alloc(qnode_cache, GFP_KERNEL);
					//node = kmalloc(sizeof(*node),GFP_KERNEL);
		if (!node)
		{
			printk(KERN_INFO "kmalloc fail \n");	
			BUG();
		}
		node->value = buf_num;
		lfq_node_init_rcu(&node->list);
		rcu_read_lock();
		lockfree_enqueue(&empty_bufsq, &node->list);
		rcu_read_unlock();
					
		
		printk(KERN_INFO "thrdenq E buf  %d",buf_num);
					
					
						
					
		}
			}
		
		}
		wait_jiffies = msecs_to_jiffies(BUF_HOLDTIME);
		schedule_timeout(wait_jiffies);
	}
	
}


#define DEBUG_IO_LOAD 1


#if 1
static int mtdblock_writesect(struct mymtd_blktrans_dev *dev,
			      uint64_t logic_ftl_blk, char *buf)	
{
	struct mtdblk_dev *mtdblk = container_of(dev, struct mtdblk_dev, mbd);
	struct mtd_info *mtd = mtdblk->mbd.mtd;
	
	uint64_t logic_page_num;
	
	uint8_t *new_temp_buf;
	size_t retlen;
	uint32_t page_shift;
	uint32_t  cache_buf,found_cache_buf;
	
	uint32_t sect_idx;
	uint32_t shift_val;
		
	uint64_t mask;
	/* needed 64bit as we do some shifting*/
	uint64_t phy_addr;
	uint64_t bumped_lpn;
	uint64_t new_temp_buf_wmask;
	

	int flush = 0;
#if 0
	int stuck_lock1 = 0, stuck_lock2 = 0;
#endif
	int stuck = 0;
	int stuck_lock3 = 0;
	int search_success = 0;
	int i;
	int j;
	
	uint64_t phy_page_offs,old_phy_page_offs;
	uint8_t *rd_buf, *oob_buf,*new_oob_buf;
	uint32_t size_copied;	
	struct mtd_oob_ops ops;
	int res;
	struct oob_data oobvalues,*oobdata;
			
			
	int retval;

	
	int selected_buf;
	static int unsigned countrmw = 0;
	
#ifdef DEBUG_IO_LOAD
	static int countthewrites = 0;
#endif
	struct lfq_node_rcu *qnode;
	struct cache_num_node *node;
	
	
	
	
	
	mtdblk->last_wr_time = jiffies;
	/* check number of active kernel threads */
	/* if number of kernel threads less than 8*
	 * try to increase the number of GC thread by 2
	 * if number of kernl threads less than 4*
	 * try to increase the number of GC thred by 4
	 * if one kernel thread
	 * GC thread = 6
	 */
#ifdef ADAPTIVE_GC
	/*ramping upGC phase*/
#if 0
	int numbitset,cangcthrds;
	numbitset = bitmap_weight(dev->active_iokthread, 64);
	if(numbitset >= 0 && numbitset <= 4)
	{
		cangcthrds = 6;
	}
	else if(numbitset >= 5 && numbitset <= 10)
	{
		cangcthrds = 4;
	}
	else if(numbitset > 10)
	{
		cangcthrds = 2;
	}
	
	printk(KERN_INFO "numbitsset %d ",numbitset);

	i = 0;
	while(activenumgcthread.counter < cangcthrds)
	{
		if(i >= NUM_GC_THREAD)
		{
			printk(KERN_INFO "to wake up (%d) > (%d)",i,NUM_GC_THREAD);
			BUG();
		}
		wake_up_process(mtdblk->ftlgc_thrd[i]);
		
		atomic_inc(&activenumgcthread);
		if(activenumgcthread.counter > NUM_GC_THREAD)
		{
			printk(KERN_INFO "active GC threads(%d) > (%d)",activenumgcthread.counter,NUM_GC_THREAD);
			BUG();
		}
		i++;
		
	}
#endif
#endif
	
	shift_val = mtdblk->pageshift -mtdblk->blkshift;
	mask = ~(-1ULL<<shift_val);
	sect_idx = logic_ftl_blk&mask;
	
	logic_page_num = (logic_ftl_blk<<mtdblk->blkshift)>>mtdblk->pageshift;
	
#ifdef APR_DEBUG	

	printk(KERN_INFO "%x: wsect = %lld pgw = %lld sect_idx = %d",current->pid,logic_ftl_blk,logic_page_num,sect_idx);
	
#endif	
search_lookup_Tab:
	search_success  = 0;
	
	//mutex_lock(&mtdblk->buf_lookup_tab_mutex);
	for(i = 0; i < MAX_FTL_CACHEBUFS;i++)
	{
		if(buf_lookup_tab[i] == logic_page_num)
		{
			if(search_success == 1)
			{
				printk(KERN_INFO "%x: twice in buflookuptab %u",current->pid,logic_page_num);
				
				printk(KERN_INFO " ");

				for(j = 0; j < MAX_FTL_CACHEBUFS ;j++)
				{
					printk("%d ",buf_lookup_tab[j]);
				}
				
				BUG();
			}
			search_success = 1;
			found_cache_buf = i;
		}
	}
	//mutex_unlock(&mtdblk->buf_lookup_tab_mutex);
	

	
	if(search_success == 1)
	{
		cache_buf = found_cache_buf;
		mutex_lock(&(mtdblk->buf_lock[cache_buf]));
		
		if(buf_lookup_tab[cache_buf] != logic_page_num)
		{
			mutex_unlock(&(mtdblk->buf_lock[cache_buf]));	
			printk(KERN_INFO "w: buf wrong allthe way back");
			goto search_lookup_Tab;
		}
		
#ifdef APR_DEBUG	
		printk(KERN_INFO "%x: wsect = %lld inbuf = %d",current->pid,logic_ftl_blk,cache_buf);
	
#endif	
		if(mtdblk->cached_buf[cache_buf].flush_in_progress.counter)
		{
			printk(KERN_INFO "BUG: flush in progress while write");
			BUG();
		}
		atomic_inc(&mtdblk->cached_buf[cache_buf].writes_in_progress);
		
		memcpy((mtdblk->buf[cache_buf]+(sect_idx*mtdblk->blksize)),buf,mtdblk->blksize);
		set_bit(sect_idx,&(mtdblk->cached_buf[cache_buf].written_mask));

		if(mtdblk->cached_buf[cache_buf].cache_state == STATE_DIRTY && mtdblk->cached_buf[cache_buf].written_mask == mtdblk->cache_fullmask)
		{
			if(!mtdblk->cached_buf[cache_buf].flush_in_progress.counter)
			{
		
			
				mtdblk->cached_buf[cache_buf].cache_state = STATE_FULL;
				/*move to full_list*/
#ifdef JAN15_DEBUG
				printk(KERN_INFO "cache %d inbuf FULL",cache_buf);
				
#endif
			
				node = kmem_cache_alloc(qnode_cache, GFP_KERNEL);
				//node = kmalloc(sizeof(*node),GFP_KERNEL);
				if (!node)
				{
					printk(KERN_INFO "kmalloc fail \n");	
					BUG();
				}
				node->value = cache_buf;
				lfq_node_init_rcu(&node->list);
				rcu_read_lock();
				lockfree_enqueue(&full_bufsq, &node->list);
				rcu_read_unlock();
			
			}
			else
			{
 				printk(KERN_INFO "BUG: flush2 in progress while write");
				BUG();
			}
		}
		mtdblk->cached_buf[cache_buf].last_touch = jiffies;
		atomic_dec(&mtdblk->cached_buf[cache_buf].writes_in_progress);
		mutex_unlock(&(mtdblk->buf_lock[cache_buf]));
	
	}
	else
	{
		
		/*set(lpn.buf_alloc_in_progress)*/
		/* test_and_set_bit returns 1 to try again ; 0 to continue*/
		if(test_and_set_bit(logic_page_num,page_bitmap))
		{
			schedule();
			printk(KERN_INFO " allocbuf in progress ; all the way back");
			goto search_lookup_Tab;
		}
		else
		{
			/* what the crap!!*/
			for(i = 0; i < MAX_FTL_CACHEBUFS;i++)
			{
				if(buf_lookup_tab[i] == logic_page_num)
				{
					printk(KERN_INFO "pessimistic_search pass; all the way back1");
					udelay(1);
					goto search_lookup_Tab;
				}
			}
					

			
			/* 
			* buffer allocation
			* 1. try to get an empty buffer
			* 2. not possible, try to get a Full dirty buffer
			* 3. not possible, try to get a half dirty buffer
			*/
			
look_for_buf:
		
		/* no way 2 threads should select the same buffer*/	
			
		selected_buf = NOT_SELECTED;
		/* try the empty buf*/
		rcu_read_lock();
		qnode = lockfree_dequeue(&empty_bufsq);
		node = container_of(qnode, struct cache_num_node, list);
		rcu_read_unlock();
		if(node != NULL)
		{
			cache_buf = node->value;
			call_rcu(&node->rcu, free_cache_num_node);	
			selected_buf = EMPTY_BUF;
		}
		/* try the full buf*/
		if(selected_buf == NOT_SELECTED)
		{
			rcu_read_lock();
			qnode = lockfree_dequeue(&full_bufsq);
			node = container_of(qnode, struct cache_num_node, list);
			rcu_read_unlock();
			
			if(node != NULL)
			{
				cache_buf = node->value;
				call_rcu(&node->rcu, free_cache_num_node);	
				selected_buf = FULL_BUF;
			}
			
		}
		if(selected_buf == NOT_SELECTED)
		{
			mutex_lock(&(mtdblk->select_buf_lock));
			atomic_inc(&mtdblk->cache_assign_count);
			cache_buf = mtdblk->cache_assign_count.counter%MAX_FTL_CACHEBUFS;	
			mutex_unlock(&(mtdblk->select_buf_lock));
#if 0
			printk(KERN_INFO "%x: w get half dirty buffer %d",current->pid,cache_buf);
#endif
			selected_buf = HALF_FULLBUF;
		}
			
		if(cache_buf < 0 || cache_buf >= MAX_FTL_CACHEBUFS)
		{
			printk(KERN_INFO "myftl: cachebuf [%d] out of range",cache_buf);	
			BUG();
		}
		
#ifdef APR_DEBUG	
		printk(KERN_INFO "%x: wsect = %lld notinbuf sel = %d",current->pid,logic_ftl_blk,cache_buf);
	
#endif	
		
		/* okay now try one of 3 paths, empty, FULLBUF or HALFBUF */
		if(selected_buf == EMPTY_BUF)
		{
#ifdef JAN15_DEBUG	
			printk(KERN_INFO "%x: empty buffer %d",current->pid,cache_buf);
			udelay(1);
#endif
			if(!(mtdblk->cached_buf[cache_buf].cache_state == STATE_EMPTY))
			{
				printk(KERN_INFO "Dequeued Buf not empty");
				BUG();
			}
			
			mutex_lock(&(mtdblk->buf_lock[cache_buf]));	
			
			mutex_lock(&mtdblk->buf_lookup_tab_mutex);
			buf_lookup_tab[cache_buf] = logic_page_num;
			mutex_unlock(&mtdblk->buf_lookup_tab_mutex);
				
			mtdblk->cached_buf[cache_buf].cache_state = STATE_DIRTY;
			mtdblk->cached_buf[cache_buf].written_mask = 0ULL;
			
			atomic_inc(&mtdblk->cached_buf[cache_buf].writes_in_progress);
			
			memcpy((mtdblk->buf[cache_buf]+(sect_idx*mtdblk->blksize)),buf,mtdblk->blksize);
			set_bit(sect_idx,&(mtdblk->cached_buf[cache_buf].written_mask));

			if(mtdblk->cached_buf[cache_buf].cache_state == STATE_DIRTY && mtdblk->cached_buf[cache_buf].written_mask == mtdblk->cache_fullmask)
			{
				if(!mtdblk->cached_buf[cache_buf].flush_in_progress.counter)
				{
		
			
					mtdblk->cached_buf[cache_buf].cache_state = STATE_FULL;
					/*move to full_list*/
#ifdef JAN15_DEBUG
					printk(KERN_INFO "cachebuf %d FULL",cache_buf);
					
#endif
			
					node = kmem_cache_alloc(qnode_cache, GFP_KERNEL);
					//node = kmalloc(sizeof(*node),GFP_KERNEL);
					if (!node)
					{
						printk(KERN_INFO "kmalloc fail \n");	
						BUG();
					}
					node->value = cache_buf;
					lfq_node_init_rcu(&node->list);
					rcu_read_lock();
					lockfree_enqueue(&full_bufsq, &node->list);
					rcu_read_unlock();
			
				}
			}
			mtdblk->cached_buf[cache_buf].last_touch = jiffies;
			atomic_dec(&mtdblk->cached_buf[cache_buf].writes_in_progress);
			
			mutex_unlock(&(mtdblk->buf_lock[cache_buf]));
												
		}
		else if(selected_buf == FULL_BUF)
		{
			/* FIFO mechanism*/
#ifdef JAN15_DEBUG
			printk(KERN_INFO "%x: w get fdirty buffer %d",current->pid,cache_buf);
			
#endif

					
			if(mtdblk->cached_buf[cache_buf].written_mask != mtdblk->cache_fullmask)
			{
				printk(KERN_INFO "Deqd Full buf not full");
				//BUG();
				goto look_for_buf;
				
			}
			
			mutex_lock(&(mtdblk->buf_lock[cache_buf]));
			/* 
			* check for writes in progress
			* set flush  in progress
			* change buffers , change map table
			*/
			atomic_inc(&mtdblk->cached_buf[cache_buf].flush_in_progress);
			
			stuck = 0;
			while(mtdblk->cached_buf[cache_buf].writes_in_progress.counter)
			{
				if(stuck_lock3%10000 == 0)
				{
					printk(KERN_INFO "myftl: stuck_lockup3 %d %d %x",cache_buf,mtdblk->cached_buf[cache_buf].writes_in_progress.counter,current->pid);
				}
				stuck_lock3++;
				stuck = 1;
		
		
				schedule();
			}
			if(stuck != 0)
			{
				printk(KERN_INFO "myftl: out of stuck_lockup3 %d %d %x",cache_buf,mtdblk->cached_buf[cache_buf].writes_in_progress.counter,current->pid);
			}
			
			new_temp_buf = mtdblk->buf[cache_buf];
			//mtdblk->buf[cache_buf] = vmalloc(mtdblk->cache_size);
			mtdblk->buf[cache_buf] = get_spare_buf();		
			if(mtdblk->buf[cache_buf] == NULL)
			{
				printk(KERN_INFO "vmalloc fail");
				BUG();
			}
			
			new_temp_buf_wmask = mtdblk->cached_buf[cache_buf].written_mask;
			bumped_lpn = buf_lookup_tab[cache_buf];
			
			
			mutex_lock(&mtdblk->buf_lookup_tab_mutex);
			buf_lookup_tab[cache_buf] = logic_page_num;
			mutex_unlock(&mtdblk->buf_lookup_tab_mutex);
#if 0
			printk(KERN_INFO "%x: [%d]buflkuptab=%u ",current->pid,cache_buf,logic_page_num);
#endif
				
			mtdblk->cached_buf[cache_buf].written_mask = 0ULL;
			mtdblk->cached_buf[cache_buf].cache_state = STATE_DIRTY;
		
			flush = 1;
			
			atomic_dec(&mtdblk->cached_buf[cache_buf].flush_in_progress);
			
			atomic_inc(&mtdblk->cached_buf[cache_buf].writes_in_progress);
			memcpy((mtdblk->buf[cache_buf]+(sect_idx*mtdblk->blksize)),buf,mtdblk->blksize);
			set_bit(sect_idx,&(mtdblk->cached_buf[cache_buf].written_mask));

			if(mtdblk->cached_buf[cache_buf].cache_state == STATE_DIRTY && mtdblk->cached_buf[cache_buf].written_mask == mtdblk->cache_fullmask)
			{
				if(!mtdblk->cached_buf[cache_buf].flush_in_progress.counter)
				{
		
			
					mtdblk->cached_buf[cache_buf].cache_state = STATE_FULL;
					/*move to full_list*/
#ifdef JAN15_DEBUG
					printk(KERN_INFO "cache %d buf FULL",cache_buf);
					
#endif
			
					node = kmem_cache_alloc(qnode_cache, GFP_KERNEL);
					//node = kmalloc(sizeof(*node),GFP_KERNEL);
					if (!node)
					{
						printk(KERN_INFO "kmalloc fail \n");	
						BUG();
					}
					node->value = cache_buf;
					lfq_node_init_rcu(&node->list);
					rcu_read_lock();
					lockfree_enqueue(&full_bufsq, &node->list);
					rcu_read_unlock();
			
				}
			}
			mtdblk->cached_buf[cache_buf].last_touch = jiffies;
			atomic_dec(&mtdblk->cached_buf[cache_buf].writes_in_progress);
			mutex_unlock(&(mtdblk->buf_lock[cache_buf]));
				
					
		}			
		else if(selected_buf == HALF_FULLBUF)
		{
#ifdef JAN15_DEBUG
			printk(KERN_INFO "%x: w get hdirty buffer %d",current->pid,cache_buf);
			
#endif	
			
			
			mutex_lock(&(mtdblk->buf_lock[cache_buf]));				
			/* 
			* check for writes in progress
			* set flush  in progress
			* change buffers , change map table
			*/
			
			atomic_inc(&mtdblk->cached_buf[cache_buf].flush_in_progress);
			stuck = 0;
			while(mtdblk->cached_buf[cache_buf].writes_in_progress.counter)
			{
				if(stuck_lock3%10000 == 0)
				{
					printk(KERN_INFO "myftl: stuck_lockup3 %d %d %x",cache_buf,mtdblk->cached_buf[cache_buf].writes_in_progress.counter,current->pid);
				}
				stuck_lock3++;
				stuck = 1;
		
		
				schedule();
			}
			if(stuck != 0)
			{
				printk(KERN_INFO "myftl: out of stuck_lockup3 %d %d %x",cache_buf,mtdblk->cached_buf[cache_buf].writes_in_progress.counter,current->pid);
			}

			
 			new_temp_buf = mtdblk->buf[cache_buf];
			new_temp_buf_wmask = mtdblk->cached_buf[cache_buf].written_mask;
			bumped_lpn = buf_lookup_tab[cache_buf];
				
			//mtdblk->buf[cache_buf] = vmalloc(mtdblk->cache_size);	
			mtdblk->buf[cache_buf] = get_spare_buf();
			if(mtdblk->buf[cache_buf] == NULL)
			{
				printk(KERN_INFO "myftl: mem alloc failure");
				BUG();
			}
			mtdblk->cached_buf[cache_buf].written_mask = 0;
			mtdblk->cached_buf[cache_buf].cache_state = STATE_DIRTY;
			
			mutex_lock(&mtdblk->buf_lookup_tab_mutex);
			buf_lookup_tab[cache_buf] = logic_page_num;
			mutex_unlock(&mtdblk->buf_lookup_tab_mutex);
#if 0
			printk(KERN_INFO "%x: [%d]buflkuptab=%u ",current->pid,cache_buf,logic_page_num);
#endif
			flush = 1;
			atomic_dec(&mtdblk->cached_buf[cache_buf].flush_in_progress);
			
			atomic_inc(&mtdblk->cached_buf[cache_buf].writes_in_progress);
			memcpy((mtdblk->buf[cache_buf]+(sect_idx*mtdblk->blksize)),buf,mtdblk->blksize);
			set_bit(sect_idx,&(mtdblk->cached_buf[cache_buf].written_mask));

			if(mtdblk->cached_buf[cache_buf].cache_state == STATE_DIRTY && mtdblk->cached_buf[cache_buf].written_mask == mtdblk->cache_fullmask)
			{
				if(!mtdblk->cached_buf[cache_buf].flush_in_progress.counter)
				{
		
			
					mtdblk->cached_buf[cache_buf].cache_state = STATE_FULL;
					/*move to full_list*/
#ifdef JAN15_DEBUG
					printk(KERN_INFO "cache %d  buf FULL",cache_buf);
					udelay(1);
#endif
			
					node = kmem_cache_alloc(qnode_cache, GFP_KERNEL);
					//node = kmalloc(sizeof(*node),GFP_KERNEL);
					if (!node)
					{
						printk(KERN_INFO "kmalloc fail \n");	
						BUG();
					}
					node->value = cache_buf;
					lfq_node_init_rcu(&node->list);
					rcu_read_lock();
					lockfree_enqueue(&full_bufsq, &node->list);
					rcu_read_unlock();
			
				}
			}
			mtdblk->cached_buf[cache_buf].last_touch = jiffies;
			atomic_dec(&mtdblk->cached_buf[cache_buf].writes_in_progress);
			mutex_unlock(&(mtdblk->buf_lock[cache_buf]));
									
		}
		else 
		{
			printk("Selected buf neither empty full or half");
			BUG();
					
		}
	
		
		test_and_clear_bit(logic_page_num,page_bitmap);
	
	}/* else (test and set)*/
		
		
	}/*else (search_success == 1)*/
	
	
	
	
	/* at this point only need to protect the map table correctly */
	
	if(flush == 1)
	{

#ifdef APR_DEBUG	
//#if 1
	
		printk(KERN_INFO "%x: flush %d  bumped lpn = %u for %u ",current->pid,cache_buf,bumped_lpn,logic_page_num);
#endif
		/* what are we doing here?
		 * 1. should we merge the buffer with flash for the bumped)_lpn
		 	; if so read from the flash maptab[bumpedlpn]; 
		 	; what if this lpn was a new write
		 	; and merge;
		 *  2. get a phy page for the writing the buffer of bumpedlpn
		 	; what happens if there is no phypage
		 *  3. write to  the phypage of flash, the newtempbuf
		 *  4. get the oldphypageoffs of maptab[bumpedlpn]
		 	; what happens if this is a new  write
		 	; make the oldpage dirty, change vpagesbitmap, numfreepagecount;
		 *   5. maptab[bumpedlpn] = newphypage
		 *   	; modify vpagebitmap, numfreepagecount
		 * 
		 */
		 
		 
		if(new_temp_buf_wmask != mtdblk->cache_fullmask)
		{
			
			countrmw++;
			/* not all sectors here are new
			* do merge with flash
			* and write to new location
			*/
#if 1
			printk(KERN_INFO "%x: read modify %d count = %u",current->pid, cache_buf,countrmw);
			printk(KERN_INFO "cache_buf mask = %llx",new_temp_buf_wmask);
			if(countrmw > 64)
			{
				printk(KERN_INFO "okay stop here");
				BUG();
			}
			
#endif

			//down_read(&(map_tabl_lock));
			while (test_and_set_bit(bumped_lpn, maptab_bitmap) != 0)
			{
				schedule();
			}
			phy_page_offs = map_table[bumped_lpn];
			//up_read(&(map_tabl_lock));
			if (test_and_clear_bit(bumped_lpn, maptab_bitmap) == 0)
			{
				printk(KERN_INFO "mapbitmap cleared wrong");
				BUG();
			}
			if(gc_testing_on == 1)
			{
			
				if(phy_page_offs == INVALID_PAGE_NUMBER)
				{
					printk(KERN_INFO "myftl: wsect %ul %ul",bumped_lpn,phy_page_offs);
					goto the_write_part;
				}
			}
			else
			{
				if(phy_page_offs == INVALID_PAGE_NUMBER)
				{
					printk(KERN_INFO "myftl: wsect %ul %ul",bumped_lpn,phy_page_offs);
					BUG();
				}
			}
	
	
#ifdef ASSERT_ON
#ifndef USE_VIRGO_RESTRICTED
#ifndef EIGHT_BANK_FLASH
			if((bumped_lpn > (mtdblk->pages_per_blk * mtdblk->num_blks)) || (phy_page_offs > (mtdblk->pages_per_blk * mtdblk->num_blks)))
			{
				printk(KERN_INFO "myftl: ASSERT flush_toflash logic_page_offs %ul %ul  > %ul",bumped_lpn,phy_page_offs,(mtdblk->pages_per_blk * mtdblk->num_blks));
				BUG();
			}
#endif
#endif
#endif


		

		
			//rd_buf = vmalloc(mtd->writesize);
			rd_buf = get_spare_buf();
			if (!rd_buf)
			{
				printk(KERN_INFO "myftl: vmalloc fail");
				BUG();
	//return -EINTR;
			}
			//oob_buf = vmalloc(mtd->oobsize);
			oob_buf = get_spare_oobbuf();
			if (!oob_buf)
			{
				printk(KERN_INFO "myftl: vmalloc fail");
				BUG();
	//return -EINTR;
			}
	
			/* should be optimised to read in 4KB
			 * should be optimised to store the read value 
			 */
	
			ops.mode = MTD_OOB_AUTO;
			ops.datbuf = rd_buf;
			ops.len = mtd->writesize;
			ops.oobbuf = oob_buf;
			ops.ooboffs = 0;	
			ops.ooblen = mtd->oobsize;


			res = mtd->read_oob(mtd,phy_page_offs<<mtdblk->pageshift, &ops);
			if(ops.retlen < mtd->writesize)
			{
				printk(KERN_ERR "myftl: merge_with_flash read failure");
				return -1;
			}
	
	
			mask = 1;
			size_copied = 0;
			sect_idx = 0;

			while(size_copied < mtdblk->cache_size)
			{
				if(((mask) & (new_temp_buf_wmask)) == 0)
				{
	
					memcpy(new_temp_buf +sect_idx*mtdblk->blksize,rd_buf+sect_idx*mtdblk->blksize,mtdblk->blksize);	
	
				}
				mask = mask <<1;
				sect_idx++;
				size_copied += mtdblk->blksize;
	
			}
			put_spare_buf(rd_buf);
			put_spare_oobbuf(oob_buf);
			//vfree(rd_buf);
			//vfree(oob_buf);
		}
the_write_part:
		;
		int tried = 0; phy_addr = INVALID_PAGE_NUMBER;
		while(tried < (numpllbanks*2) && phy_addr == INVALID_PAGE_NUMBER)
//		while(tried < 128 && phy_addr == INVALID_PAGE_NUMBER)
		{
			phy_addr = get_ppage(mtdblk,RAND_SEL,0);
			tried++;
		}

		

#ifndef USE_VIRGO_RESTRICTED
#ifndef EIGHT_BANK_FLASH
		if(phy_addr >= (mtdblk->pages_per_blk * mtdblk->num_blks))
		{
			printk(KERN_INFO "myftl: ASSERT new_writesect phyaddr %ul >= %ul",phy_addr,(mtdblk->pages_per_blk * mtdblk->num_blks));
			BUG();
		}
#endif
#endif	
		if(phy_addr == INVALID_PAGE_NUMBER)
		{
			printk(KERN_INFO "myftl: ASSERT new_writesect phyaddr %ul",phy_addr);
			BUG();
		}
		if(phy_addr >= 16777216)
		{
			printk(KERN_INFO "myftl: ASSERT new_writesect wr phyaddr %ul",phy_addr);
			BUG();
		}
		
#if 0
		printk(KERN_INFO "%x: w maptable[%u] = %u",current->pid,logic_page_num,phy_addr);
#endif
		uint32_t oldblkno,page_in_blk;
		uint32_t newblkno,bank_num;
		int banknum;
		
		if(phy_addr != INVALID_PAGE_NUMBER){
			/*physical page write to medium. do we need a lock here?*/

			page_shift = mtdblk->pageshift;
			
			banknum = phy_addr/(mtdblk->pages_per_blk*mtdblk->hwblks_per_bank);
#if 0						
#ifdef USE_VIRGO_RESTRICTED
			banknum = phy_addr/(mtdblk->pages_per_blk*4096);
#else
			banknum = phy_addr/(mtdblk->pages_per_blk*mtdblk->blks_per_bank);
#endif
#endif


#ifdef EIGHT_BANK_FLASH
			banknum = banknum/8;
#endif
			
			
			//new_oob_buf = vmalloc(mtd->oobsize);
			new_oob_buf = get_spare_oobbuf();
		
			if (!new_oob_buf)
			{
				printk(KERN_INFO "myftl deinit: vmalloc fail");
				BUG();
				//return -EINTR;
			}

			oobdata = &oobvalues;
			
			atomic_inc(&mtdblk->seq_num);
			oobdata->seq_number = mtdblk->seq_num.counter;
			oobdata->logic_page_num = bumped_lpn;
			oobdata->blk_type = DATA_BLK;
			memcpy(new_oob_buf,oobdata,sizeof(*oobdata));
			
#ifdef DEBUG_IO_LOAD
			int sum,avg;
					
			countthewrites++;
			if(countthewrites == 1000)
			{
				countthewrites = 0;
				sum=0;
				for(i = 0; i < numpllbanks;i++)
				{
					sum += mtdblk->activity_matrix.num_writes[i].counter;
					
				}
				avg = sum/numpllbanks;
				printk(KERN_INFO "IO load total = %d avg = %d",sum,avg);
			}
#endif
			
			
			if(mtdblk->activity_matrix.gc_goingon[banknum].counter == 1)
			{
#ifdef PLL_GC_DEBUG
				printk(KERN_INFO "%x: bank %d numWr%ld and GC %ld",current->pid,banknum,mtdblk->activity_matrix.num_writes[banknum].counter, mtdblk->activity_matrix.gc_goingon[banknum].counter);
#endif
				atomic_inc(&gc_on_writes_collisions);
			}
			/*
			* oob operation modes
			*
			* MTD_OOB_PLACE:	oob data are placed at the given offset
			* MTD_OOB_AUTO:	oob data are automatically placed at the free areas
			*			which are defined by the ecclayout
			* MTD_OOB_RAW:		mode to read raw data+oob in one chunk. The oob data
			*			is inserted into the data. Thats a raw image of the
			*			flash contents.
			*/
			
			/**
			
			struct mtd_oob_ops {
			mtd_oob_mode_t	mode; operation mode
			size_mtd_t len;number of data bytes to write/read
			size_mtd_t retlen;number of data bytes written/read
			size_mtd_t ooblen;number of oob bytes to write/read
			size_mtd_t oobretlen;number of oob bytes written/read
			uint32_t ooboffs;offset of oob data in the oob area (only relevant when mode = MTD_OOB_PLACE)
			uint8_t	 *datbuf;data buffer - if NULL only oob data are read/written
			uint8_t	 *oobbuf;oob data buffer
			};
			Note, it is allowed to read more then one OOB area at one go, but not write.
			 * The interface assumes that the OOB write requests program only one page's
			 * OOB area.
			 */
			
			ops.mode = MTD_OOB_AUTO;
			ops.ooblen = mtd->oobsize;
			ops.len = mtd->writesize;
			ops.retlen = 0;
			ops.oobretlen = 0;
			ops.ooboffs = 0;
			ops.datbuf = new_temp_buf;
			ops.oobbuf = new_oob_buf;
			retval = 1;
		
			retval = mtd->write_oob(mtd,(phy_addr<<page_shift), &ops);
			
//			mtd->write(mtd,(phy_addr<<page_shift),mtdblk->cache_size,&retlen,new_temp_buf);

			//if(retval != 0)
			if(ops.retlen != mtd->writesize)
			{

				printk("myftl: mtd write %llx  %ld %d %d fail",phy_addr<<page_shift,phy_addr,sizeof(*oobdata),ops.retlen);
				vfree(new_temp_buf);
				BUG();			
				return -1;
			}
			
			

			
		
		
			if(bumped_lpn >= 16777216)
			{
					printk(KERN_INFO "myftl: ASSERT bumped_lpn wr %ul",bumped_lpn);
					BUG();
			}
			old_phy_page_offs = map_table[bumped_lpn];
			if(gc_testing_on == 0)
			{
				if(old_phy_page_offs >= 16777216)
				{
					printk(KERN_INFO "myftl: ASSERT old_phy_page_offs wr %ul",old_phy_page_offs);
					BUG();
				}
				oldblkno = old_phy_page_offs/(mtdblk->pages_per_blk);
				page_in_blk = old_phy_page_offs%(mtdblk->pages_per_blk);
				if(page_in_blk >= 64)
				{
					printk(KERN_INFO "myftl: ASSERT page_in_blk wr %ul",page_in_blk);
					BUG();
				}
				if(oldblkno >= 262144)
				{
					printk(KERN_INFO "myftl: ASSERT oldblkno wr %ul",oldblkno);
					BUG();
				}
				bank_num = oldblkno/mtdblk->hwblks_per_bank;
#if 0
#ifdef USE_VIRGO_RESTRICTED
				bank_num = oldblkno/4096;
#else
				bank_num = oldblkno/mtdblk->blks_per_bank;
#endif
#endif
#ifdef EIGHT_BANK_FLASH
				bank_num = bank_num/8;
#endif
			
				test_and_clear_bit(page_in_blk,blk_info[oldblkno].valid_pages_map);
				atomic_dec(&blk_info[oldblkno].num_valid_pages);
				atomic_inc(&bank_info[bank_num].perbank_ndirty_pages);
			}
			else 
			{
			if(old_phy_page_offs == INVALID_PAGE_NUMBER_32)
			{
				printk(KERN_INFO "myftl: ASSERT old_phy_page_offs wr %ul",old_phy_page_offs);
				goto 	modifymaptab;
			}
			if(old_phy_page_offs >= 16777216)
			{
				printk(KERN_INFO "myftl: ASSERT old_phy_page_offs wr %ul",old_phy_page_offs);
				BUG();
			}
			oldblkno = old_phy_page_offs/(mtdblk->pages_per_blk);
			page_in_blk = old_phy_page_offs%(mtdblk->pages_per_blk);
			if(page_in_blk >= 64)
			{
					printk(KERN_INFO "myftl: ASSERT page_in_blk wr %ul",page_in_blk);
					BUG();
			}
			if(oldblkno >= 262144)
			{
					printk(KERN_INFO "myftl: ASSERT oldblkno wr %ul",oldblkno);
					BUG();
			}
			bank_num = oldblkno/mtdblk->hwblks_per_bank;
#if 0
			#ifdef USE_VIRGO_RESTRICTED
	 		bank_num = oldblkno/4096;
			#else
			bank_num = oldblkno/mtdblk->blks_per_bank;
			#endif
#endif
#ifdef EIGHT_BANK_FLASH
			bank_num = bank_num/8;
#endif
			
			test_and_clear_bit(page_in_blk,blk_info[oldblkno].valid_pages_map);
			atomic_dec(&blk_info[oldblkno].num_valid_pages);
			atomic_inc(&bank_info[bank_num].perbank_ndirty_pages);
			}
			
	modifymaptab:		
			//down_write(&(map_tabl_lock));
			while (test_and_set_bit(bumped_lpn, maptab_bitmap) != 0)
			{
				schedule();
			}
			map_table[bumped_lpn] =  phy_addr;
			
#ifdef APR_DEBUG	
	
			printk(KERN_INFO "%x: rmap[%ld] = %ld ",current->pid,phy_addr,reverse_map_tab[phy_addr]);
#endif
			if(gc_testing_on ==1)
				reverse_map_tab[phy_addr] = bumped_lpn;
			
#ifdef APR_DEBUG	
	
			printk(KERN_INFO "%x: map[%ld] = %ld ",current->pid,bumped_lpn,phy_addr);
#endif
			
			//up_write(&(map_tabl_lock));
			if (test_and_clear_bit(bumped_lpn, maptab_bitmap) == 0)
			{
				printk(KERN_INFO "mapbitmap cleared wrong");
				BUG();
			}
			
			if(phy_addr >= 16777216)
			{
					printk(KERN_INFO "myftl: ASSERT phy_addr wr %ul",old_phy_page_offs);
					BUG();
			}

			newblkno = phy_addr/(mtdblk->pages_per_blk);
			page_in_blk = phy_addr%(mtdblk->pages_per_blk);
			
			if(page_in_blk >= 64)
			{
					printk(KERN_INFO "myftl: ASSERT page_in_blk wr %ul",page_in_blk);
					BUG();
			}
			if(newblkno  >= 262144)
			{
					printk(KERN_INFO "myftl: ASSERT newblkno  wr %ul",oldblkno);
					BUG();
			}
			test_and_set_bit(page_in_blk,blk_info[newblkno].valid_pages_map);
			atomic_inc(&blk_info[newblkno].num_valid_pages);
			
			put_spare_buf(new_temp_buf);
			put_spare_oobbuf(new_oob_buf);
			//vfree(new_temp_buf);
			//vfree(new_oob_buf);
			

			banknum = phy_addr/(mtdblk->pages_per_blk*mtdblk->hwblks_per_bank);
#if 0						
#ifdef USE_VIRGO_RESTRICTED
			banknum = phy_addr/(mtdblk->pages_per_blk*4096);
#else
			banknum = phy_addr/(mtdblk->pages_per_blk*mtdblk->blks_per_bank);
#endif
#endif


#ifdef EIGHT_BANK_FLASH
			banknum = banknum/8;
#endif

			

			//#ifndef NON_SCHEDULED
			
			#if defined (BG_C_GC) || defined (BG_UNC_GC)
			atomic_dec(&mtdblk->activity_matrix.num_writes[banknum]);
			
			#ifdef PLL_GC_DEBUG
			printk(KERN_INFO "%x: [%ld]num_wr-- = %ld",current->pid,banknum,mtdblk->activity_matrix.num_writes[banknum].counter);
			#endif
			#endif
			
		}
		else
		{
			printk(KERN_INFO "myftl: ASSERT new_writesect phyaddr %ul",phy_addr);
			BUG();
		}


			
	}
#if 0
	printk(KERN_INFO "writesect done");
#endif
	return 0;
}						 
#endif

#define CKPT_FTL 1
					 
#define MAP_TAB 1
#define BLKINFO_TAB 2
#define FREEMAP_TAB 3					 					 
#define BANKINFO_TAB 4

int read_a_ckpt_blk(struct mtdblk_dev *mtdblk,int blkno)
{
	
	struct mtd_info *mtd;
	struct oob_data oobvalues,*oobdata;
	
	
	uint32_t map_table_size;
	uint32_t blk_info_size;
	uint32_t free_map_size;
	uint32_t bank_info_size;
	int ret_next_blk;
	
	
	
	
	
	struct mtd_oob_ops ops;
	

	uint64_t phy_page;
	int i;
	uint32_t num_map_table_blks;
	uint32_t num_blk_info_blks;
	uint32_t num_freemap_blks;
	uint32_t num_bankinfo_blks;
	uint32_t num_freemap_bankinfo_blks;
	uint32_t num_blks_req;
	
	uint8_t *rd_buf,*oob_buf;
	
	uint32_t blk_iter;
	uint64_t seq_num;
	int32_t rd_len;
	int res;
	uint32_t cur_read;
	uint32_t map_table_index = 0;
	uint32_t blk_info_index = 0;
	uint32_t bank_info_index = 0;
	uint32_t free_map_index =0;
	uint32_t rdmap_blks = 0;
	
	mtd = mtdblk->mbd.mtd;
	map_table_size = mtdblk->pages_per_blk * mtdblk->num_blks * sizeof(uint32_t);
	blk_info_size = mtdblk->num_blks * sizeof(struct per_blk_info);
	bank_info_size = mtdblk->num_parallel_banks* sizeof(struct per_bank_info);
	free_map_size = mtdblk->num_blks/8;
	
	uint32_t num_map_table_pages;
	uint32_t num_blk_info_pages ;
	uint32_t num_freemap_pages ;
	uint32_t num_bankinfo_pages ;
	uint32_t flash_page_size;
	uint32_t total_ckpt_pages;
	
	flash_page_size = mtd->writesize;
			
	num_map_table_pages = (map_table_size/flash_page_size) +((map_table_size)%(flash_page_size) ? 1 : 0);
	num_blk_info_pages = (blk_info_size/flash_page_size)+((blk_info_size)%(flash_page_size) ? 1 : 0);
	num_freemap_pages = (free_map_size/flash_page_size) +((free_map_size)%(flash_page_size) ? 1 : 0);
	num_bankinfo_pages = (bank_info_size/flash_page_size)+((bank_info_size)%(flash_page_size) ? 1 : 0);
	
	total_ckpt_pages = num_map_table_pages+num_blk_info_pages+num_freemap_pages+num_bankinfo_pages;
	num_blks_req = total_ckpt_pages/mtdblk->pages_per_blk+((total_ckpt_pages)%(mtdblk->pages_per_blk) ? 1 : 0);

	
	printk(KERN_INFO "init ftl maptabpages = %d %d",num_map_table_pages,num_map_table_pages/mtdblk->pages_per_blk);
	printk(KERN_INFO "init ftl blkinfopages = %d %d",num_blk_info_pages,num_blk_info_pages/mtdblk->pages_per_blk);
	printk(KERN_INFO "init ftl freemapages = %d %d",num_freemap_pages,num_freemap_pages/mtdblk->pages_per_blk);
	printk(KERN_INFO "init ftl bankinfopages = %d %d",num_bankinfo_pages,num_bankinfo_pages/mtdblk->pages_per_blk);
	

	num_map_table_blks = (num_map_table_pages/mtdblk->pages_per_blk) +((num_map_table_pages%mtdblk->pages_per_blk) ? 1 : 0);
	num_blk_info_blks = (num_blk_info_pages/mtdblk->pages_per_blk)+((num_blk_info_pages%mtdblk->pages_per_blk) ? 1 : 0);
	num_freemap_blks = (num_freemap_pages/mtdblk->pages_per_blk) +((num_freemap_pages%mtdblk->pages_per_blk) ? 1 : 0);
	num_bankinfo_blks = (num_bankinfo_pages/mtdblk->pages_per_blk)+((num_bankinfo_pages%mtdblk->pages_per_blk) ? 1 : 0);

	
	
	int found_map_table_blks = 0;
	int found_blk_info_blks = 0;
	int found_freemap_blks = 0;
	int found_bankinfo_blks = 0;
	
	rd_buf = vmalloc(mtd->writesize);
	oob_buf = vmalloc(mtd->oobsize);		
	if(rd_buf == NULL || oob_buf == NULL)
	{
		printk(KERN_INFO "vmalloc fail");
		BUG();
	}
	
	
	
	map_table_index = 0;
	blk_info_index = 0;
	bank_info_index = 0;
	free_map_index =0;
	
	
	blk_iter = blkno;
	{
		phy_page = blk_iter*mtdblk->pages_per_blk;
			
		ops.mode = MTD_OOB_AUTO;
		ops.datbuf = rd_buf;
		ops.len = mtd->writesize;
		ops.oobbuf = oob_buf;
		ops.ooboffs = 0;	
		ops.ooblen = mtd->oobsize;
	
	
		
	
		res = mtd->read_oob(mtd,phy_page<<mtdblk->pageshift, &ops);
		if(ops.retlen < mtd->writesize)
		{
			printk(KERN_ERR "FTL read failure");
			printk(KERN_ERR " phypage = %ld",phy_page);
			BUG();
		}
		oobdata = &oobvalues;
		memcpy(oobdata, oob_buf,sizeof(*oobdata));
		
		if(oobdata->blk_type != MAP_BLK)
		{
			printk(KERN_INFO "chained blk not map blk");
			BUG();
		}
			
		
		
		seq_num = oobdata->seq_number;
		ret_next_blk = oobdata->logic_page_num;
	
		int mycount;
		printk(KERN_INFO "rd oobbuf");
		printk(KERN_INFO " ");
		for(mycount = 0; mycount < sizeof(*oobdata);mycount++)
			printk(" %d ",oob_buf[mycount]);
	

		printk(KERN_INFO "blk = %ld seq_num = %u",blk_iter,seq_num);

		rdmap_blks++;
//		arr[seq_num] = blk_iter;
		if(seq_num < num_map_table_blks)
		{
			/* do something with map table */
			found_map_table_blks++;
			cur_read = MAP_TAB;
			
			map_table_index = seq_num*mtdblk->pages_per_blk*(mtd->writesize);
			
			if((map_table_index + mtd->writesize) < map_table_size)
			{
				memcpy(((uint8_t*)map_table)+map_table_index,rd_buf,mtd->writesize);
				map_table_index = map_table_index+mtd->writesize;
			}
			else
			{
				memcpy(((uint8_t*)map_table)+map_table_index,rd_buf,(map_table_size-map_table_index));
				map_table_index = map_table_size;
			}
			/* read the remaining pages*/
			
			if(map_table_index == map_table_size)
			{
				cur_read = BLKINFO_TAB;
			}
		}
		else if(seq_num < num_map_table_blks+num_blk_info_blks)
		{
			found_blk_info_blks++;
			cur_read = BLKINFO_TAB;
			blk_info_index = (seq_num*mtdblk->pages_per_blk*mtd->writesize)-map_table_size;
			
			if((blk_info_index + mtd->writesize) < blk_info_size)
			{
				
				memcpy(((uint8_t*)blk_info)+ blk_info_index,rd_buf,mtd->writesize);
				blk_info_index = blk_info_index+mtd->writesize;
			}
			else
			{
				memcpy(((uint8_t*)blk_info)+ blk_info_index,rd_buf,(blk_info_size-blk_info_index));
				blk_info_index = blk_info_size;
			}
			
			if(blk_info_index == blk_info_size)
			{
				cur_read = FREEMAP_TAB;
			}
			
			
		}
		else if(seq_num < num_map_table_blks+num_blk_info_blks+num_freemap_blks)
		{
			found_freemap_blks++;
			cur_read = FREEMAP_TAB;
			
			free_map_index = (seq_num*mtdblk->pages_per_blk*mtd->writesize)-(blk_info_size+map_table_size);
			
		
			
			if(free_map_index + mtd->writesize < free_map_size)
			{
				memcpy(((uint8_t*)mtdblk->free_blk_map)+ free_map_index,rd_buf,mtd->writesize);
				free_map_index = free_map_index+mtd->writesize;
			}
			else
			{
				memcpy(((uint8_t*)mtdblk->free_blk_map)+ free_map_index,rd_buf,(free_map_size-free_map_index));
				free_map_index = free_map_size;
			}
			
			
			if(free_map_index == free_map_size)
			{
				cur_read = BANKINFO_TAB;
			}
		}
		else if(seq_num < num_map_table_blks+num_blk_info_blks+num_freemap_blks+num_bankinfo_blks)
		{
			found_bankinfo_blks++;
			
			bank_info_index = (seq_num*mtdblk->pages_per_blk*mtd->writesize)-(blk_info_size+map_table_size+free_map_size);
			if(bank_info_index + mtd->writesize < bank_info_size)
			{
				memcpy(((uint8_t*)bank_info)+ bank_info_index,rd_buf,mtd->writesize);
				bank_info_index = bank_info_index+mtd->writesize;
			}
			else
			{
				memcpy(((uint8_t*)bank_info)+ bank_info_index,rd_buf,(bank_info_size-bank_info_index));
				bank_info_index = bank_info_size;
			}
			
			cur_read = BANKINFO_TAB;

			if(bank_info_index == bank_info_size)
			{
				goto end_of_func;
			}
		}
		else
		{
			printk(KERN_INFO "bug: wrong seqnum = %u",seq_num);
			BUG();
		}

		for(i = 1; i < mtdblk->pages_per_blk;i++)
		{
			if(cur_read == MAP_TAB)
			{
			
				if((map_table_index + mtd->writesize) > map_table_size)
				{
					rd_len = map_table_size - map_table_index;
					if(rd_len <= 0)
					{
						printk(KERN_INFO "rdlen = %d",rd_len);
						goto next_page;
					}
				}
				else
				{
					rd_len = mtd->writesize;
				}
				phy_page = blk_iter*mtdblk->pages_per_blk+i;

				ops.mode = MTD_OOB_AUTO;
				ops.datbuf = rd_buf;
				ops.len = mtd->writesize;
				ops.oobbuf = oob_buf;
				ops.ooboffs = 0;	
				ops.ooblen = mtd->oobsize;



				res = mtd->read_oob(mtd,phy_page<<mtdblk->pageshift, &ops);
				if(ops.retlen < mtd->writesize)
				{
					printk(KERN_ERR "FTL read failure");
					printk(KERN_ERR " phypage = %ld",phy_page);
					BUG();
				}
				oobdata = &oobvalues;
				memcpy(oobdata, oob_buf,sizeof(*oobdata));
				if(oobdata->blk_type != MAP_BLK)
				{
					printk(KERN_INFO "the remaining page in mapblk not map");
					BUG();
				}

	
				memcpy(((uint8_t*)map_table)+ map_table_index,rd_buf,rd_len);
				map_table_index = map_table_index+rd_len;
				if(map_table_index == map_table_size)
				{
					cur_read = BLKINFO_TAB;
				}
			
			}
			else if(cur_read == BLKINFO_TAB)
			{
		
				if((blk_info_index + mtd->writesize) > blk_info_size)
				{
					rd_len = blk_info_size - blk_info_index;
					if(rd_len <= 0)
					{
						printk(KERN_INFO "rdlen = %d",rd_len);
						goto next_page;
					}
				}
				else
				{
					rd_len = mtd->writesize;
				}
				phy_page = blk_iter*mtdblk->pages_per_blk+i;

				ops.mode = MTD_OOB_AUTO;
				ops.datbuf = rd_buf;
				ops.len = mtd->writesize;
				ops.oobbuf = oob_buf;
				ops.ooboffs = 0;	
				ops.ooblen = mtd->oobsize;



				res = mtd->read_oob(mtd,phy_page<<mtdblk->pageshift, &ops);
				if(ops.retlen < mtd->writesize)
				{
					printk(KERN_ERR "FTL read failure");
					printk(KERN_ERR " phypage = %ld",phy_page);
					BUG();
				}
				oobdata = &oobvalues;
				memcpy(oobdata, oob_buf,sizeof(*oobdata));
				if(oobdata->blk_type != MAP_BLK)
				{
					printk(KERN_INFO "the remaining page in mapblk not map");
					BUG();
				}
	
				memcpy(((uint8_t*)blk_info)+ blk_info_index,rd_buf,rd_len);

				blk_info_index = blk_info_index+rd_len;
				if(blk_info_index == blk_info_size)
				{
					cur_read = FREEMAP_TAB;
				}
		
				
			}
			else if(cur_read == FREEMAP_TAB)
			{
									
				if((free_map_index + mtd->writesize) > free_map_size)
				{
					rd_len = free_map_size - free_map_index;
					if(rd_len <= 0)
					{
						printk(KERN_INFO "rdlen = %d",rd_len);
						goto next_page;
					}
				}
				else
				{
					rd_len = mtd->writesize;
				}
				phy_page = blk_iter*mtdblk->pages_per_blk+i;

				ops.mode = MTD_OOB_AUTO;
				ops.datbuf = rd_buf;
				ops.len = mtd->writesize;
				ops.oobbuf = oob_buf;
				ops.ooboffs = 0;	
				ops.ooblen = mtd->oobsize;



				res = mtd->read_oob(mtd,phy_page<<mtdblk->pageshift, &ops);
				if(ops.retlen < mtd->writesize)
				{
					printk(KERN_ERR "FTL read failure");
					printk(KERN_ERR " phypage = %ld",phy_page);
					BUG();
				}
				oobdata = &oobvalues;
	
	
	
				memcpy(oobdata, oob_buf,sizeof(*oobdata));
				if(oobdata->blk_type != MAP_BLK)
				{
					printk(KERN_INFO "the remaining page in mapblk not map");
					BUG();
				}
	
	
	
				memcpy(((uint8_t*)mtdblk->free_blk_map) + free_map_index,rd_buf,rd_len);

				free_map_index = free_map_index+rd_len;
				if(free_map_index == free_map_size)
				{
					cur_read = BANKINFO_TAB;
				}
		
					
			}
			else if(cur_read == BANKINFO_TAB)
			{	
				
				if((bank_info_index + mtd->writesize) > bank_info_size)
				{
					rd_len = bank_info_size - bank_info_index;
					if(rd_len <= 0)
					{
						goto next_page;
					}
				}
				else
				{
					rd_len = mtd->writesize;
				}
				phy_page = blk_iter*mtdblk->pages_per_blk+i;

				ops.mode = MTD_OOB_AUTO;
				ops.datbuf = rd_buf;
				ops.len = mtd->writesize;
				ops.oobbuf = oob_buf;
				ops.ooboffs = 0;	
				ops.ooblen = mtd->oobsize;



				res = mtd->read_oob(mtd,phy_page<<mtdblk->pageshift, &ops);
				if(ops.retlen < mtd->writesize)
				{
					printk(KERN_ERR "FTL read failure");
					printk(KERN_ERR " phypage = %ld",phy_page);
					BUG();
				}
				oobdata = &oobvalues;
				memcpy(oobdata, oob_buf,sizeof(*oobdata));
				if(oobdata->blk_type != MAP_BLK)
				{
					printk(KERN_INFO "the remaining page in mapblk not map");
					BUG();
				}

				memcpy(((uint8_t*)bank_info) + bank_info_index,rd_buf,rd_len);
		
				bank_info_index = bank_info_index+rd_len;
				if(bank_info_index == bank_info_size)
				{
					goto end_of_func;
				}
			
			
			}
		next_page:
		;				
		} /*for i = 1 to pages_per_blk */

		
end_of_func:
		;		
	} /* for(blk_iter = start_blk; blk_iter < end_blk;blk_iter++) */
	return ret_next_blk;

	
	
}
					 					 
int read_ckpt(struct mtdblk_dev *mtdblk,int start_blk,int end_blk,int arr[])
{
	
	struct mtd_info *mtd;
	struct oob_data oobvalues,*oobdata;
	
	
	uint32_t map_table_size;
	uint32_t blk_info_size;
	uint32_t free_map_size;
	uint32_t bank_info_size;
	
	
	
	
	
	
	struct mtd_oob_ops ops;
	
#if 0
	uint32_t page_shift = mtdblk->pageshift;
	uint8_t *temp_buf;
	uint32_t size;
	uint32_t retval;
	uint32_t wr_len;
	uint32_t blk;
	uint32_t phy_addr;
	uint32_t flashblksize;
	
	uint32_t pages_written;
#endif
	uint64_t phy_page;
	int i;
	uint32_t num_map_table_blks;
	uint32_t num_blk_info_blks;
	uint32_t num_freemap_blks;
	uint32_t num_bankinfo_blks;
	uint32_t num_freemap_bankinfo_blks;
	uint32_t num_blks_req;
	
	uint8_t *rd_buf,*oob_buf;
	
	uint32_t blk_iter;
	uint64_t seq_num;
	int32_t rd_len;
	int res;
	uint32_t cur_read;
	uint32_t map_table_index = 0;
	uint32_t blk_info_index = 0;
	uint32_t bank_info_index = 0;
	uint32_t free_map_index =0;
	uint32_t rdmap_blks = 0;
	
	mtd = mtdblk->mbd.mtd;
	map_table_size = mtdblk->pages_per_blk * mtdblk->num_blks * sizeof(uint32_t);
	blk_info_size = mtdblk->num_blks * sizeof(struct per_blk_info);
	bank_info_size = mtdblk->num_parallel_banks* sizeof(struct per_bank_info);
	free_map_size = mtdblk->num_blks/8;
	
	uint32_t num_map_table_pages;
	uint32_t num_blk_info_pages ;
	uint32_t num_freemap_pages ;
	uint32_t num_bankinfo_pages ;
	uint32_t flash_page_size;
	uint32_t total_ckpt_pages;
	
	flash_page_size = mtd->writesize;
			
	num_map_table_pages = (map_table_size/flash_page_size) +((map_table_size)%(flash_page_size) ? 1 : 0);
	num_blk_info_pages = (blk_info_size/flash_page_size)+((blk_info_size)%(flash_page_size) ? 1 : 0);
	num_freemap_pages = (free_map_size/flash_page_size) +((free_map_size)%(flash_page_size) ? 1 : 0);
	num_bankinfo_pages = (bank_info_size/flash_page_size)+((bank_info_size)%(flash_page_size) ? 1 : 0);
	
	total_ckpt_pages = num_map_table_pages+num_blk_info_pages+num_freemap_pages+num_bankinfo_pages;
	num_blks_req = total_ckpt_pages/mtdblk->pages_per_blk+((total_ckpt_pages)%(mtdblk->pages_per_blk) ? 1 : 0);

	
	printk(KERN_INFO "init ftl maptabpages = %d %d",num_map_table_pages,num_map_table_pages/mtdblk->pages_per_blk);
	printk(KERN_INFO "init ftl blkinfopages = %d %d",num_blk_info_pages,num_blk_info_pages/mtdblk->pages_per_blk);
	printk(KERN_INFO "init ftl freemapages = %d %d",num_freemap_pages,num_freemap_pages/mtdblk->pages_per_blk);
	printk(KERN_INFO "init ftl bankinfopages = %d %d",num_bankinfo_pages,num_bankinfo_pages/mtdblk->pages_per_blk);
	

	num_map_table_blks = (num_map_table_pages/mtdblk->pages_per_blk) +((num_map_table_pages%mtdblk->pages_per_blk) ? 1 : 0);
	num_blk_info_blks = (num_blk_info_pages/mtdblk->pages_per_blk)+((num_blk_info_pages%mtdblk->pages_per_blk) ? 1 : 0);
	num_freemap_blks = (num_freemap_pages/mtdblk->pages_per_blk) +((num_freemap_pages%mtdblk->pages_per_blk) ? 1 : 0);
	num_bankinfo_blks = (num_bankinfo_pages/mtdblk->pages_per_blk)+((num_bankinfo_pages%mtdblk->pages_per_blk) ? 1 : 0);

	
	
	int found_map_table_blks = 0;
	int found_blk_info_blks = 0;
	int found_freemap_blks = 0;
	int found_bankinfo_blks = 0;
	
	rd_buf = vmalloc(mtd->writesize);
	oob_buf = vmalloc(mtd->oobsize);		
	if(rd_buf == NULL || oob_buf == NULL)
	{
		printk(KERN_INFO "vmalloc fail");
		BUG();
	}
	
	
	
	map_table_index = 0;
	blk_info_index = 0;
	bank_info_index = 0;
	free_map_index =0;
	

	for(blk_iter = start_blk; blk_iter < end_blk;blk_iter++)
	{
		phy_page = blk_iter*mtdblk->pages_per_blk;
			
		ops.mode = MTD_OOB_AUTO;
		ops.datbuf = rd_buf;
		ops.len = mtd->writesize;
		ops.oobbuf = oob_buf;
		ops.ooboffs = 0;	
		ops.ooblen = mtd->oobsize;
	
	
		
	
		res = mtd->read_oob(mtd,phy_page<<mtdblk->pageshift, &ops);
		if(ops.retlen < mtd->writesize)
		{
			printk(KERN_ERR "FTL read failure");
			printk(KERN_ERR " phypage = %ld",phy_page);
			BUG();
		}
		oobdata = &oobvalues;
		memcpy(oobdata, oob_buf,sizeof(*oobdata));
		
		
			
		if(oobdata->blk_type == MAP_BLK)
		{
			seq_num = oobdata->seq_number;
		
			int mycount;
			printk(KERN_INFO "rd oobbuf");
			printk(KERN_INFO " ");
			for(mycount = 0; mycount < sizeof(*oobdata);mycount++)
			printk(" %d ",oob_buf[mycount]);
		
#if 1
			printk(KERN_INFO "blk = %ld seq_num = %u",blk_iter,seq_num);
#endif
			rdmap_blks++;
			arr[seq_num] = blk_iter;
			if(seq_num < num_map_table_blks)
			{
				/* do something with map table */
				found_map_table_blks++;
				cur_read = MAP_TAB;
				
				map_table_index = seq_num*mtdblk->pages_per_blk*(mtd->writesize);
				
				if((map_table_index + mtd->writesize) < map_table_size)
				{
					memcpy(((uint8_t*)map_table)+map_table_index,rd_buf,mtd->writesize);
					map_table_index = map_table_index+mtd->writesize;
				}
				else
				{
					memcpy(((uint8_t*)map_table)+map_table_index,rd_buf,(map_table_size-map_table_index));
					map_table_index = map_table_size;
				}
				/* read the remaining pages*/
				
				if(map_table_index == map_table_size)
				{
					cur_read = BLKINFO_TAB;
				}
			}
			else if(seq_num < num_map_table_blks+num_blk_info_blks)
			{
				found_blk_info_blks++;
				cur_read = BLKINFO_TAB;
				blk_info_index = (seq_num*mtdblk->pages_per_blk*mtd->writesize)-map_table_size;
				
				if((blk_info_index + mtd->writesize) < blk_info_size)
				{
					
					memcpy(((uint8_t*)blk_info)+ blk_info_index,rd_buf,mtd->writesize);
					blk_info_index = blk_info_index+mtd->writesize;
				}
				else
				{
					memcpy(((uint8_t*)blk_info)+ blk_info_index,rd_buf,(blk_info_size-blk_info_index));
					blk_info_index = blk_info_size;
				}
				
				if(blk_info_index == blk_info_size)
				{
					cur_read = FREEMAP_TAB;
				}
				
				
			}
			else if(seq_num < num_map_table_blks+num_blk_info_blks+num_freemap_blks)
			{
				found_freemap_blks++;
				cur_read = FREEMAP_TAB;
				
				free_map_index = (seq_num*mtdblk->pages_per_blk*mtd->writesize)-(blk_info_size+map_table_size);
				
			
				
				if(free_map_index + mtd->writesize < free_map_size)
				{
					memcpy(((uint8_t*)mtdblk->free_blk_map)+ free_map_index,rd_buf,mtd->writesize);
					free_map_index = free_map_index+mtd->writesize;
				}
				else
				{
					memcpy(((uint8_t*)mtdblk->free_blk_map)+ free_map_index,rd_buf,(free_map_size-free_map_index));
					free_map_index = free_map_size;
				}
				
				
				if(free_map_index == free_map_size)
				{
					cur_read = BANKINFO_TAB;
				}
			}
			else if(seq_num < num_map_table_blks+num_blk_info_blks+num_freemap_blks+num_bankinfo_blks)
			{
				found_bankinfo_blks++;
				
				bank_info_index = (seq_num*mtdblk->pages_per_blk*mtd->writesize)-(blk_info_size+map_table_size+free_map_size);
				if(bank_info_index + mtd->writesize < bank_info_size)
				{
					memcpy(((uint8_t*)bank_info)+ bank_info_index,rd_buf,mtd->writesize);
					bank_info_index = bank_info_index+mtd->writesize;
				}
				else
				{
					memcpy(((uint8_t*)bank_info)+ bank_info_index,rd_buf,(bank_info_size-bank_info_index));
					bank_info_index = bank_info_size;
				}
				
				cur_read = BANKINFO_TAB;
#if 0
				printk(KERN_INFO "bank_info_index = %u",bank_info_index);
#endif
				if(bank_info_index == bank_info_size)
				{
					goto next_blk;
				}
			}
			else
			{
				printk(KERN_INFO "bug: wrong seqnum = %u",seq_num);
				BUG();
			}
#if 0
			printk(KERN_INFO " blk[ %d] 0th page done cur_rd = %d",blk_iter,cur_read);
#endif
			for(i = 1; i < mtdblk->pages_per_blk;i++)
			{
				if(cur_read == MAP_TAB)
				{
				
					if((map_table_index + mtd->writesize) > map_table_size)
					{
						rd_len = map_table_size - map_table_index;
						if(rd_len <= 0)
						{
							printk(KERN_INFO "rdlen = %d",rd_len);
							goto next_page;
						}
					}
					else
					{
						rd_len = mtd->writesize;
					}
					phy_page = blk_iter*mtdblk->pages_per_blk+i;
				
					ops.mode = MTD_OOB_AUTO;
					ops.datbuf = rd_buf;
					ops.len = mtd->writesize;
					ops.oobbuf = oob_buf;
					ops.ooboffs = 0;	
					ops.ooblen = mtd->oobsize;
	
	
	
					res = mtd->read_oob(mtd,phy_page<<mtdblk->pageshift, &ops);
					if(ops.retlen < mtd->writesize)
					{
						printk(KERN_ERR "FTL read failure");
						printk(KERN_ERR " phypage = %ld",phy_page);
						BUG();
					}
					oobdata = &oobvalues;
					memcpy(oobdata, oob_buf,sizeof(*oobdata));
					if(oobdata->blk_type != MAP_BLK)
					{
						printk(KERN_INFO "the remaining page in mapblk not map");
						BUG();
					}
	
					
					memcpy(((uint8_t*)map_table)+ map_table_index,rd_buf,rd_len);
					map_table_index = map_table_index+rd_len;
					if(map_table_index == map_table_size)
					{
						cur_read = BLKINFO_TAB;
					}
				
				}
				else if(cur_read == BLKINFO_TAB)
				{
						
					if((blk_info_index + mtd->writesize) > blk_info_size)
					{
						rd_len = blk_info_size - blk_info_index;
						if(rd_len <= 0)
						{
							printk(KERN_INFO "rdlen = %d",rd_len);
							goto next_page;
						}
					}
					else
					{
						rd_len = mtd->writesize;
					}
					phy_page = blk_iter*mtdblk->pages_per_blk+i;
	
					ops.mode = MTD_OOB_AUTO;
					ops.datbuf = rd_buf;
					ops.len = mtd->writesize;
					ops.oobbuf = oob_buf;
					ops.ooboffs = 0;	
					ops.ooblen = mtd->oobsize;



					res = mtd->read_oob(mtd,phy_page<<mtdblk->pageshift, &ops);
					if(ops.retlen < mtd->writesize)
					{
						printk(KERN_ERR "FTL read failure");
						printk(KERN_ERR " phypage = %ld",phy_page);
						BUG();
					}
					oobdata = &oobvalues;
					memcpy(oobdata, oob_buf,sizeof(*oobdata));
					if(oobdata->blk_type != MAP_BLK)
					{
						printk(KERN_INFO "the remaining page in mapblk not map");
						BUG();
					}
					
					memcpy(((uint8_t*)blk_info)+ blk_info_index,rd_buf,rd_len);
			
					blk_info_index = blk_info_index+rd_len;
					if(blk_info_index == blk_info_size)
					{
						cur_read = FREEMAP_TAB;
					}
			
					
				}
				else if(cur_read == FREEMAP_TAB)
				{
					
						
					if((free_map_index + mtd->writesize) > free_map_size)
					{
						rd_len = free_map_size - free_map_index;
						if(rd_len <= 0)
						{
							printk(KERN_INFO "rdlen = %d",rd_len);
							goto next_page;
						}
					}
					else
					{
						rd_len = mtd->writesize;
					}
					phy_page = blk_iter*mtdblk->pages_per_blk+i;
	
					ops.mode = MTD_OOB_AUTO;
					ops.datbuf = rd_buf;
					ops.len = mtd->writesize;
					ops.oobbuf = oob_buf;
					ops.ooboffs = 0;	
					ops.ooblen = mtd->oobsize;



					res = mtd->read_oob(mtd,phy_page<<mtdblk->pageshift, &ops);
					if(ops.retlen < mtd->writesize)
					{
						printk(KERN_ERR "FTL read failure");
						printk(KERN_ERR " phypage = %ld",phy_page);
						BUG();
					}
					oobdata = &oobvalues;
					
					
					
					memcpy(oobdata, oob_buf,sizeof(*oobdata));
					if(oobdata->blk_type != MAP_BLK)
					{
						printk(KERN_INFO "the remaining page in mapblk not map");
						BUG();
					}
					
					
					
					memcpy(((uint8_t*)mtdblk->free_blk_map) + free_map_index,rd_buf,rd_len);
			
					free_map_index = free_map_index+rd_len;
					if(free_map_index == free_map_size)
					{
						cur_read = BANKINFO_TAB;
					}
			
						
				}
				else if(cur_read == BANKINFO_TAB)
				{	
					
					if((bank_info_index + mtd->writesize) > bank_info_size)
					{
						rd_len = bank_info_size - bank_info_index;
						if(rd_len <= 0)
						{
							goto next_page;
						}
					}
					else
					{
						rd_len = mtd->writesize;
					}
					phy_page = blk_iter*mtdblk->pages_per_blk+i;
	
					ops.mode = MTD_OOB_AUTO;
					ops.datbuf = rd_buf;
					ops.len = mtd->writesize;
					ops.oobbuf = oob_buf;
					ops.ooboffs = 0;	
					ops.ooblen = mtd->oobsize;



					res = mtd->read_oob(mtd,phy_page<<mtdblk->pageshift, &ops);
					if(ops.retlen < mtd->writesize)
					{
						printk(KERN_ERR "FTL read failure");
						printk(KERN_ERR " phypage = %ld",phy_page);
						BUG();
					}
					oobdata = &oobvalues;
					memcpy(oobdata, oob_buf,sizeof(*oobdata));
					if(oobdata->blk_type != MAP_BLK)
					{
						printk(KERN_INFO "the remaining page in mapblk not map");
						BUG();
					}
#if 0
					printk(KERN_INFO "bank_info_index = %u",bank_info_index);
#endif
					memcpy(((uint8_t*)bank_info) + bank_info_index,rd_buf,rd_len);
			
					bank_info_index = bank_info_index+rd_len;
					if(bank_info_index == bank_info_size)
					{
						goto next_blk;
					}
				
				
				}
next_page:
		;				
			} /*for i = 1 to pages_per_blk */
#if 0	
			printk(KERN_INFO " blk[ %d] done",blk_iter);
#endif
		}/*if(oobdata->blk_type == MAP_BLK) */
next_blk:
		;		
	} /* for(blk_iter = start_blk; blk_iter < end_blk;blk_iter++) */
	

	printk(KERN_INFO "rdckpt mapblks = %d",rdmap_blks);
	
	printk(KERN_INFO "read_ckpt found maptabblks = %d",found_map_table_blks);
	printk(KERN_INFO "read_ckpt found blkinfoblks = %d",found_blk_info_blks);
	printk(KERN_INFO "read_ckpt found freemapblks = %d",found_freemap_blks);
	printk(KERN_INFO "read_ckpt found bankinfoblks = %d",found_bankinfo_blks);
	
	return 0;
}



#define CKPT_RANGE 10
int alloc_near_boundary(struct mtdblk_dev *mtdblk)
{
	int bank;
	uint32_t start_blk;
	uint32_t last_blk;
	uint32_t blk;
	
        for(bank=0; bank < numpllbanks;bank++)
        {
		
                start_blk = mtdblk->cur_writing[bank].first_blk;
         
            
                for(blk = start_blk;blk<= start_blk+CKPT_RANGE;blk++)
                {
                        if(blk_isfree(mtdblk,blk))
                        {
                                if(is_block_bad(mtdblk,blk))
                                {
                                        continue;
                                }
                        }
			blk_unfree(mtdblk,blk);
                        atomic_dec(&bank_info[bank].perbank_nfree_blks);
                        return blk;
                }
        }
        
        for(bank=0; bank < numpllbanks;bank++)
        {
		
                last_blk = mtdblk->cur_writing[bank].last_blk;
         
            
		for(blk = last_blk-CKPT_RANGE;blk<= last_blk;blk++)
                {
                        if(blk_isfree(mtdblk,blk))
                        {
                                if(is_block_bad(mtdblk,blk))
                                {
                                        continue;
                                }
                        }
			blk_unfree(mtdblk,blk);
                        atomic_dec(&bank_info[bank].perbank_nfree_blks);
                        return blk;
                }
        }
        return INVALID;
        
}

/* missed for checkpoint:
 * sequence number
 * curwritten block
 */
int wr_ckpt_chained(struct mtdblk_dev *mtdblk)
{
        struct oob_data oobvalues,*oobdata;
        uint32_t map_table_size;
        uint32_t blk_info_size;
        uint32_t freeblkmap_size;
        uint32_t bank_info_size;
        uint32_t flashblksize;
        uint8_t *oob_buf;
        uint32_t pages_written;
        uint64_t phy_addr;
        uint32_t blk;
        uint32_t wr_len;
        uint32_t retval;
        uint32_t size;
        struct mtd_oob_ops ops;
        uint8_t *temp_buf;
        uint32_t page_shift = mtdblk->pageshift;
        uint32_t blks_written;
        struct mtd_info *mtd;
        uint8_t *wr_buf;
        mtd = mtdblk->mbd.mtd;
	int i;
	
#if 1
        flashblksize = (mtd->erasesize);
        oob_buf = vmalloc(mtd->oobsize);
        wr_buf =  vmalloc(mtd->writesize);
        if (!oob_buf|| !wr_buf)
        {
                printk(KERN_INFO "myftl deinit: vmalloc fail");
                BUG();
		//return -EINTR;
        }
	 
	uint32_t free_map_size;
      	
	map_table_size = mtdblk->pages_per_blk * mtdblk->num_blks * sizeof(uint32_t);
	blk_info_size = mtdblk->num_blks * sizeof(struct per_blk_info);
	bank_info_size = mtdblk->num_parallel_banks* sizeof(struct per_bank_info);
	freeblkmap_size = mtdblk->num_blks/8;
	free_map_size = mtdblk->num_blks/8;
	
	uint32_t num_map_table_pages;
	uint32_t num_blk_info_pages ;
	uint32_t num_freemap_pages ;
	uint32_t num_bankinfo_pages ;
	uint32_t flash_page_size;
	uint32_t total_ckpt_pages;
	uint32_t num_blks_req;
	
	flash_page_size = mtd->writesize;
			
	num_map_table_pages = (map_table_size/flash_page_size) +((map_table_size)%(flash_page_size) ? 1 : 0);
	num_blk_info_pages = (blk_info_size/flash_page_size)+((blk_info_size)%(flash_page_size) ? 1 : 0);
	num_freemap_pages = (free_map_size/flash_page_size) +((free_map_size)%(flash_page_size) ? 1 : 0);
	num_bankinfo_pages = (bank_info_size/flash_page_size)+((bank_info_size)%(flash_page_size) ? 1 : 0);
	
	total_ckpt_pages = num_map_table_pages+num_blk_info_pages+num_freemap_pages+num_bankinfo_pages;
	num_blks_req = total_ckpt_pages/mtdblk->pages_per_blk+((total_ckpt_pages)%(mtdblk->pages_per_blk) ? 1 : 0);

	
	
        blks_written = 0;
        oobdata = &oobvalues;
			
	
        oobdata->seq_number = -1;
        oobdata->logic_page_num = INVALID_PAGE_NUMBER;
        oobdata->blk_type = MAP_BLK;
	
	int *ckpt_alloc_blk;
	int ckpt_alloc_blk_index;
	
	ckpt_alloc_blk = vmalloc(num_blks_req+1);
        blk = alloc_near_boundary(mtdblk);
        if(blk == INVALID)
        {
                printk(KERN_INFO "no free blk in CKPT_RANGE");
                BUG();
        }
	ckpt_alloc_blk[0] = blk;
	for(i = 1; i < num_blks_req;i++)
        {
                blk = exper_alloc_block(mtdblk,RAND_SEL);
		if(blk == INVALID)
		{
			printk(KERN_INFO "no free blk for CKPT");
			BUG();
		}
                ckpt_alloc_blk[i] = blk;
        }
        ckpt_alloc_blk[num_blks_req] = INVALID;
	ckpt_alloc_blk_index = -1;
	
	printk(KERN_INFO "allocd ckpt blks");
	for(i = 0; i < num_blks_req;i++)
	{
		printk(" %ld",ckpt_alloc_blk[i]);
	}
	
	
        for(size = 0,pages_written = mtdblk->pages_per_blk; size < map_table_size;)
        {
                if(pages_written < mtdblk->pages_per_blk)
                {
                        phy_addr++;
                }
                else
                {
                        //blk = exper_alloc_block(mtdblk,RAND_SEL);
                        ckpt_alloc_blk_index++;
                        blk = ckpt_alloc_blk[ckpt_alloc_blk_index];
			
                        blks_written++;
                        if(blk == INVALID)
                        {
                                printk(KERN_INFO "no enough block to checkpoint");
                                BUG();
                        }
                        phy_addr = blk*mtdblk->pages_per_blk;
                        pages_written = 0;
                        oobdata->seq_number++;
                        oobdata->logic_page_num = ckpt_alloc_blk[ckpt_alloc_blk_index+1];
                        printk(KERN_INFO "wrckpt: maptab blk =%ld seqnum = %ld",blk,oobdata->seq_number);
                }
		
		
#if 1
                printk(KERN_INFO "phyaddr = %u blk = %ld",phy_addr,phy_addr/mtdblk->pages_per_blk);
#endif
		
                if((map_table_size -(size+mtd->writesize)) > 0)
                {
                        wr_len = mtd->writesize;
                        temp_buf = ((uint8_t*)map_table)+size;
                }
                else
                {
                        wr_len = map_table_size -size;
                        temp_buf = ((uint8_t*)map_table)+size;
                        memset(wr_buf,0xFF,mtd->writesize);
                        memcpy(wr_buf,temp_buf,wr_len);
                        temp_buf = wr_buf;
                }
		
		
		
                memcpy(oob_buf,oobdata,sizeof(*oobdata));

		
                ops.mode = MTD_OOB_AUTO;
                ops.ooblen = mtd->oobsize;
                ops.len = mtd->writesize;
                ops.retlen = 0;
                ops.oobretlen = 0;
                ops.ooboffs = 0;
                ops.datbuf = temp_buf;
                ops.oobbuf = oob_buf;
                retval = 1;
		
                retval = mtd->write_oob(mtd,(phy_addr<<page_shift), &ops);
                if(ops.retlen != wr_len)
                {
                        printk("myftl: gc mtd write fail");
                        BUG();			
                        return -1;
                }
		
                size += wr_len;
                pages_written++;
        }
	
        printk(KERN_INFO "map tab ckptd");
        for(size = 0; size < blk_info_size;)
        {
                if(pages_written < mtdblk->pages_per_blk)
                {
                        phy_addr++;
                }
                else
                {
                        ckpt_alloc_blk_index++;
                        blk = ckpt_alloc_blk[ckpt_alloc_blk_index];
                        blks_written++;
                        if(blk == INVALID)
                        {
                                printk(KERN_INFO "no enough block to checkpoint");
                                BUG();
                        }
                        phy_addr = blk*mtdblk->pages_per_blk;
                        pages_written = 0;
                        oobdata->seq_number++;
                        oobdata->logic_page_num = ckpt_alloc_blk[ckpt_alloc_blk_index+1];
                        printk(KERN_INFO "wrckpt: blkinfo blk =%ld seqnum = %ld",blk,oobdata->seq_number);
                }

		
#if 0
                printk(KERN_INFO "phyaddr = %u",phy_addr);
#endif
		
		if((blk_info_size -(size+mtd->writesize)) > 0)
		{
			wr_len = mtd->writesize;
			temp_buf = ((uint8_t*)blk_info)+size;
		}
		else
		{
			wr_len = blk_info_size -size;


			temp_buf = ((uint8_t*)blk_info)+size;
			memset(wr_buf,0xFF,mtd->writesize);
			memcpy(wr_buf,temp_buf,wr_len);
			temp_buf = wr_buf;
		}


		memcpy(oob_buf,oobdata,sizeof(*oobdata));

		ops.mode = MTD_OOB_AUTO;
		ops.ooblen = mtd->oobsize;
		ops.len = mtd->writesize;
		ops.retlen = 0;
		ops.oobretlen = 0;
		ops.ooboffs = 0;
		ops.datbuf = temp_buf;
		ops.oobbuf = oob_buf;
		retval = 1;

		retval = mtd->write_oob(mtd,(phy_addr<<page_shift), &ops);
		if(ops.retlen != wr_len)
		{

			printk("myftl: gc mtd write fail");
	
			BUG();			
			return -1;
		}

		size += wr_len;
		pages_written++;
				
        }
        printk(KERN_INFO "blkinfo ckptd");
        for(size = 0; size < freeblkmap_size;)
        {
                if(pages_written < mtdblk->pages_per_blk)
                {
                        phy_addr++;
                }
                else
                {
                        ckpt_alloc_blk_index++;
                        blk = ckpt_alloc_blk[ckpt_alloc_blk_index];
                        if(blk == INVALID)
                        {
                                printk(KERN_INFO "no enough block to checkpoint");
                                BUG();
                        }
                        phy_addr = blk*mtdblk->pages_per_blk;
                        pages_written = 0;
                        oobdata->seq_number++;
                        oobdata->logic_page_num = ckpt_alloc_blk[ckpt_alloc_blk_index+1];
                        printk(KERN_INFO "wrckpt: freemap blk =%ld seqnum = %ld",blk,oobdata->seq_number);
                }

	
#if 0
                printk(KERN_INFO "phyaddr = %u",phy_addr);
#endif

		if((freeblkmap_size -(size+mtd->writesize)) > 0)
		{
			wr_len = mtd->writesize;
			temp_buf = ((uint8_t*)mtdblk->free_blk_map)+size;
		}
		else
		{
			wr_len = freeblkmap_size-size;

			temp_buf = ((uint8_t*)mtdblk->free_blk_map)+size;
			memset(wr_buf,0xFF,mtd->writesize);
			memcpy(wr_buf,temp_buf,wr_len);
			temp_buf = wr_buf;
		}


		memcpy(oob_buf,oobdata,sizeof(*oobdata));

		ops.mode = MTD_OOB_AUTO;
		ops.ooblen = mtd->oobsize;
		ops.len = mtd->writesize;
		ops.retlen = 0;
		ops.oobretlen = 0;
		ops.ooboffs = 0;
		ops.datbuf = temp_buf;
		ops.oobbuf = oob_buf;
		retval = 1;

		retval = mtd->write_oob(mtd,(phy_addr<<page_shift), &ops);
		if(ops.retlen != wr_len)
		{

			printk("myftl: gc mtd write fail");
	
			BUG();			
			return -1;
		}

		size += wr_len;
		pages_written++;
				
        }
        printk(KERN_INFO "freeblkmap ckptd");
	
        for(size = 0; size < bank_info_size;)
        {
                if(pages_written < mtdblk->pages_per_blk)
                {
                        phy_addr++;
                }
                else
                {
                        ckpt_alloc_blk_index++;
                        blk = ckpt_alloc_blk[ckpt_alloc_blk_index];
                        blks_written++;
                        if(blk == INVALID)
                        {
                                printk(KERN_INFO "no enough block to checkpoint");
                                BUG();
                        }
                        phy_addr = blk*mtdblk->pages_per_blk;
                        pages_written = 0;
                        oobdata->seq_number++;
                        oobdata->logic_page_num = ckpt_alloc_blk[ckpt_alloc_blk_index+1];
                        printk(KERN_INFO "wrckpt: bankinfo blk =%ld seqnum = %ld",blk,oobdata->seq_number);
                }

		
#if 0
                printk(KERN_INFO "phyaddr = %u",phy_addr);
#endif
		
		if((bank_info_size -(size+mtd->writesize)) > 0)
		{
			wr_len = mtd->writesize;
			temp_buf = ((uint8_t*)bank_info)+size;
		}
		else
		{
			wr_len = bank_info_size -size;
			temp_buf = ((uint8_t*)bank_info)+size;

			memset(wr_buf,0xFF,mtd->writesize);
			memcpy(wr_buf,temp_buf,wr_len);
			temp_buf = wr_buf;
		}


		memcpy(oob_buf,oobdata,sizeof(*oobdata));

		ops.mode = MTD_OOB_AUTO;
		ops.ooblen = mtd->oobsize;
		ops.len = mtd->writesize;
		ops.retlen = 0;
		ops.oobretlen = 0;
		ops.ooboffs = 0;
		ops.datbuf = temp_buf;
		ops.oobbuf = oob_buf;
		retval = 1;

		retval = mtd->write_oob(mtd,(phy_addr<<page_shift), &ops);
		if(ops.retlen != wr_len)
		{

			printk("myftl: gc mtd write fail");
	
			BUG();			
			return -1;
		}

		size += wr_len;
		pages_written++;
			
        }
	
	
        printk(KERN_INFO "bankinfo ckptd");
	
	
        vfree(oob_buf);
	vfree(wr_buf);
	vfree(ckpt_alloc_blk);
        printk(KERN_INFO " wr_ckpt = %d",blks_written);
        return 0;
#endif
}



struct ckpt_rd_thrdinfo
{
	uint32_t start_blk;
	uint32_t end_blk;
	int *rdarr;
	struct mtdblk_dev *mtdblk;
	int banknum;
};

void ckptrd(void *arg)
{
	struct mtdblk_dev *mtdblk = ((struct ckpt_rd_thrdinfo *)arg)->mtdblk;
	uint32_t start_blk=((struct ckpt_rd_thrdinfo *)arg)->start_blk;
	uint32_t end_blk=((struct ckpt_rd_thrdinfo *)arg)->end_blk;
	int *arr = ((struct ckpt_rd_thrdinfo *)arg)->rdarr;
	int banknum = ((struct ckpt_rd_thrdinfo *)arg)->banknum;
	/* end_blk passed is exclusive*/
	read_ckpt(mtdblk,start_blk,end_blk,arr);
	clear_bit(banknum,  &(mtdblk->ckptrd_mask));
	
	kfree(arg);
}


int parllel_blk_scan(struct mtdblk_dev *mtdblk)
{
#if 0
        struct task_struct *ckpt_thread_struct;
        int banknum;
        struct ckpt_rd_thrdinfo *ckpt_thread_arg;
        uint32_t start_blk,end_blk;
        int i;
        
        arr = vmalloc(num_blks_req*(sizeof(int)));
        if(arr == NULL)
        {
                printk(KERN_INFO "vmalloc fail");
                BUG();
        }
        for(i = 0;i < num_blks_req;i++)
        {
                arr[i] = -1;
        }
#if 0
        read_ckpt(mtdblk,0,mtdblk->num_blks,arr);
#else
        
	
        mtdblk->ckptrd_mask = 0xFFFFFFFFFFFFFFFF;
	
	
        for(banknum = 0; banknum < numpllbanks;banknum++)
        {
                start_blk = mtdblk->cur_writing[banknum].first_blk;
                end_blk = mtdblk->cur_writing[banknum].last_blk+1;
		
                ckpt_thread_arg = kmalloc(sizeof (struct ckpt_rd_thrdinfo),GFP_KERNEL);
                if(ckpt_thread_arg == NULL)
                {
                        printk(KERN_ERR "kmalloc ckpt_thread_arg fail");
                        BUG();
                }
                ckpt_thread_arg->start_blk = start_blk;
                ckpt_thread_arg->end_blk = end_blk;
                ckpt_thread_arg->rdarr = arr;
		
                ckpt_thread_arg->mtdblk = mtdblk;
                ckpt_thread_arg->banknum = banknum;
		
		
				
				
                ckpt_thread_struct  = kthread_run(ckptrd, (ckpt_thread_arg),
                                "ckptthrd %ld",banknum);

                if (IS_ERR(ckpt_thread_struct)) {
                        PTR_ERR(ckpt_thread_struct);
                        BUG();
                }
		
        }
	
        while(mtdblk->ckptrd_mask != 0ULL)
        {
                schedule();
        }
#endif
#endif
	return 0;
						 
}


uint32_t per_bank_bounded_scan(struct mtdblk_dev *mtdblk,int bank)
{
	uint32_t start_blk,last_blk;
	uint32_t blk_iter;
	uint64_t phy_page;
	struct mtd_oob_ops ops;
	uint8_t *rd_buf,*oob_buf;
	struct mtd_info *mtd;
	struct oob_data oobvalues,*oobdata;
	uint64_t seq_num;
	int res;
	
	mtd = mtdblk->mbd.mtd;
	
	
	rd_buf = vmalloc(mtd->writesize);
	oob_buf = vmalloc(mtd->oobsize);		
	if(rd_buf == NULL || oob_buf == NULL)
	{
		printk(KERN_INFO "vmalloc fail");
		BUG();
	}		
	
        start_blk = mtdblk->cur_writing[bank].first_blk;
        
        for(blk_iter = start_blk; blk_iter < start_blk+CKPT_RANGE;blk_iter++)
        {
                phy_page = blk_iter*mtdblk->pages_per_blk;
			
                ops.mode = MTD_OOB_AUTO;
                ops.datbuf = rd_buf;
                ops.len = mtd->writesize;
                ops.oobbuf = oob_buf;
                ops.ooboffs = 0;	
                ops.ooblen = mtd->oobsize;
			
	
                res = mtd->read_oob(mtd,phy_page<<mtdblk->pageshift, &ops);
                if(ops.retlen < mtd->writesize)
                {
                        printk(KERN_ERR "FTL read failure");
                        printk(KERN_ERR " phypage = %ld",phy_page);
                        BUG();
                }
                oobdata = &oobvalues;
                memcpy(oobdata, oob_buf,sizeof(*oobdata));
		
		
			
                if(oobdata->blk_type == MAP_BLK)
                {
                        seq_num = oobdata->seq_number;
                        if(seq_num == 0ULL)
			{
                                return blk_iter;
			}
		
                }
        }
	last_blk = mtdblk->cur_writing[bank].last_blk;
	for(blk_iter = last_blk-CKPT_RANGE; blk_iter <= last_blk;blk_iter++)
	{
		phy_page = blk_iter*mtdblk->pages_per_blk;
			
		ops.mode = MTD_OOB_AUTO;
		ops.datbuf = rd_buf;
		ops.len = mtd->writesize;
		ops.oobbuf = oob_buf;
		ops.ooboffs = 0;	
		ops.ooblen = mtd->oobsize;
			
	
		res = mtd->read_oob(mtd,phy_page<<mtdblk->pageshift, &ops);
		if(ops.retlen < mtd->writesize)
		{
			printk(KERN_ERR "FTL read failure");
			printk(KERN_ERR " phypage = %ld",phy_page);
			BUG();
		}
		oobdata = &oobvalues;
		memcpy(oobdata, oob_buf,sizeof(*oobdata));
		
		
			
		if(oobdata->blk_type == MAP_BLK)
		{
			seq_num = oobdata->seq_number;
			if(seq_num == 0ULL)
			{
				return blk_iter;
			}
		
		}
	}
  
	return INVALID;
}

struct scan_thrd_info
{
	struct mtdblk_dev *mtdblk;
	int bank;
};

void the_scan_thread(void *arg)
{
	struct mtdblk_dev *mtdblk = ((struct scan_thrd_info *)arg)->mtdblk;
	uint32_t bank=((struct scan_thrd_info *)arg)->bank;
	uint32_t found_blk;
	
	found_blk = per_bank_bounded_scan(mtdblk,bank);
	if(found_blk != INVALID)
	{
		mtdblk->first_ckpt_blk = found_blk;
	}
	clear_bit(bank,  &(mtdblk->ckptrd_mask));
	kfree(arg);
}

int parll_blk_scan(struct mtdblk_dev *mtdblk,int*arr)
{
	struct task_struct *ckpt_thread_struct;
	int banknum;
	struct ckpt_rd_thrdinfo *ckpt_thread_arg;
	uint32_t start_blk,end_blk;
		
	mtdblk->ckptrd_mask = 0xFFFFFFFFFFFFFFFF;
		
		
	for(banknum = 0; banknum < numpllbanks;banknum++)
	{
		start_blk = mtdblk->cur_writing[banknum].first_blk;
		end_blk = mtdblk->cur_writing[banknum].last_blk+1;
			
		ckpt_thread_arg = kmalloc(sizeof (struct ckpt_rd_thrdinfo),GFP_KERNEL);
		if(ckpt_thread_arg == NULL)
		{
			printk(KERN_ERR "kmalloc ckpt_thread_arg fail");
			BUG();
		}
		ckpt_thread_arg->start_blk = start_blk;
		ckpt_thread_arg->end_blk = end_blk;
		ckpt_thread_arg->rdarr = arr;
			
		ckpt_thread_arg->mtdblk = mtdblk;
		ckpt_thread_arg->banknum = banknum;
			
			
					
					
		ckpt_thread_struct  = kthread_run(ckptrd, (ckpt_thread_arg),
				"ckptthrd %ld",banknum);
	
		if (IS_ERR(ckpt_thread_struct)) {
			PTR_ERR(ckpt_thread_struct);
			BUG();
		}
			
	}
		
	while(mtdblk->ckptrd_mask != 0ULL)
	{
		schedule();
	}
}
int bounded_pll_blk_scan(struct mtdblk_dev *mtdblk,int *arr)
{
	struct task_struct *scan_thread_struct;
        int banknum;
	struct scan_thrd_info *scan_thread_arg;
        uint32_t start_blk,end_blk;
        int i;
	uint32_t map_table_size,blk_info_size,free_map_size,bank_info_size;
	uint32_t num_map_table_pages,num_blk_info_pages,num_freemap_pages,num_bankinfo_pages;
	uint32_t flash_page_size;
	uint32_t total_ckpt_pages;
	int rdmap_blks;	
	uint32_t next_blk;
	struct mtd_info *mtd;
	uint32_t num_blks_req;
	
	
	
	
	mtd = mtdblk->mbd.mtd;
	
	map_table_size = mtdblk->pages_per_blk * mtdblk->num_blks * sizeof(uint32_t);
	blk_info_size = mtdblk->num_blks * sizeof(struct per_blk_info);
	bank_info_size = mtdblk->num_parallel_banks* sizeof(struct per_bank_info);
	free_map_size = mtdblk->num_blks/8;
	
	
	
	flash_page_size = mtd->writesize;
			
	num_map_table_pages = (map_table_size/flash_page_size) +((map_table_size)%(flash_page_size) ? 1 : 0);
	num_blk_info_pages = (blk_info_size/flash_page_size)+((blk_info_size)%(flash_page_size) ? 1 : 0);
	num_freemap_pages = (free_map_size/flash_page_size) +((free_map_size)%(flash_page_size) ? 1 : 0);
	num_bankinfo_pages = (bank_info_size/flash_page_size)+((bank_info_size)%(flash_page_size) ? 1 : 0);
	
	total_ckpt_pages = num_map_table_pages+num_blk_info_pages+num_freemap_pages+num_bankinfo_pages;
	num_blks_req = total_ckpt_pages/mtdblk->pages_per_blk+((total_ckpt_pages)%(mtdblk->pages_per_blk) ? 1 : 0);

	
	printk(KERN_INFO "init ftl maptabpages = %d %d",num_map_table_pages,num_map_table_pages/mtdblk->pages_per_blk);
	printk(KERN_INFO "init ftl blkinfopages = %d %d",num_blk_info_pages,num_blk_info_pages/mtdblk->pages_per_blk);
	printk(KERN_INFO "init ftl freemapages = %d %d",num_freemap_pages,num_freemap_pages/mtdblk->pages_per_blk);
	printk(KERN_INFO "init ftl bankinfopages = %d %d",num_bankinfo_pages,num_bankinfo_pages/mtdblk->pages_per_blk);
	
#if 0
	num_map_table_blks = (num_map_table_pages/mtdblk->pages_per_blk) +((num_map_table_pages%mtdblk->pages_per_blk) ? 1 : 0);
	num_blk_info_blks = (num_blk_info_pages/mtdblk->pages_per_blk)+((num_blk_info_pages%mtdblk->pages_per_blk) ? 1 : 0);
	num_freemap_blks = (num_freemap_pages/mtdblk->pages_per_blk) +((num_freemap_pages%mtdblk->pages_per_blk) ? 1 : 0);
	num_bankinfo_blks = (num_bankinfo_pages/mtdblk->pages_per_blk)+((num_bankinfo_pages%mtdblk->pages_per_blk) ? 1 : 0);

#endif
	
     
#if 0
        read_ckpt(mtdblk,0,mtdblk->num_blks,arr);
#endif
        
	
        mtdblk->ckptrd_mask = 0xFFFFFFFFFFFFFFFF;
	
	mtdblk->first_ckpt_blk = INVALID;
        for(banknum = 0; banknum < numpllbanks;banknum++)
        {
                start_blk = mtdblk->cur_writing[banknum].first_blk;
                end_blk = mtdblk->cur_writing[banknum].last_blk+1;
		
		scan_thread_arg = kmalloc(sizeof (struct scan_thrd_info),GFP_KERNEL);
		if(scan_thread_arg == NULL)
                {
                        printk(KERN_ERR "kmalloc ckpt_thread_arg fail");
                        BUG();
                }
		scan_thread_arg->mtdblk = mtdblk;
		scan_thread_arg->bank = banknum;
        			
		scan_thread_struct  = kthread_run(the_scan_thread, (scan_thread_arg),
				"scan_thrd %ld",banknum);

		if (IS_ERR(scan_thread_struct)) {
			PTR_ERR(scan_thread_struct);
                        BUG();
                }
		
        }
	
        while(mtdblk->ckptrd_mask != 0ULL)
        {
                schedule();
        }
	
	if(mtdblk->first_ckpt_blk != INVALID)
	{
		printk(KERN_INFO "first ckpt blk = %d",mtdblk->first_ckpt_blk);
	}
	else
	{
		printk(KERN_INFO "no ckpt first ckpt blk = %d ",mtdblk->first_ckpt_blk);
		//BUG();
	}
	
	
	
	
	
	next_blk = mtdblk->first_ckpt_blk;
	rdmap_blks = 0;
	arr[0] = next_blk;
	while(next_blk != INVALID)
	{
		if(next_blk < 0 || next_blk > 262143)
		{
			printk("wrong blk = %ld", next_blk);
			BUG();
		}
		if(rdmap_blks > num_blks_req)
		{
			printk("rdmap_blks = %ld > num_blks_req %ld" ,rdmap_blks,num_blks_req);
		}
		next_blk = read_a_ckpt_blk(mtdblk,next_blk);
		rdmap_blks++;
		arr[rdmap_blks] = next_blk;
		
	}
	
	printk(KERN_INFO "rdckpt mapblks = %d",rdmap_blks);
	return 0;
	
	
}

int the_pagescan_thread(void *arg)
{
	
	
	struct mtdblk_dev *mtdblk = ((struct scan_thrd_info *)arg)->mtdblk;
	uint32_t bank=((struct scan_thrd_info *)arg)->bank;
	uint32_t start_blk,last_blk;
	uint32_t blk_iter;
	uint64_t phy_page;
	struct mtd_oob_ops ops;
	uint8_t *rd_buf,*oob_buf;
	struct mtd_info *mtd;
	struct oob_data oobvalues,*oobdata;
	uint64_t seq_num;
	int res;
	int page;
	
	mtd = mtdblk->mbd.mtd;
	
	
	rd_buf = vmalloc(mtd->writesize);
	oob_buf = vmalloc(mtd->oobsize);		
	if(rd_buf == NULL || oob_buf == NULL)
	{
		printk(KERN_INFO "vmalloc fail");
		BUG();
	}		
	
	start_blk = mtdblk->cur_writing[bank].first_blk;
	last_blk = mtdblk->cur_writing[bank].last_blk+1;
	printk(KERN_INFO "scan blks %ld->%ld",start_blk,last_blk);
	for(blk_iter = start_blk; blk_iter < last_blk;blk_iter++)
	{
		for(page = 0; page < mtdblk->pages_per_blk;page++)
		{
			phy_page = blk_iter*mtdblk->pages_per_blk+page;
				
			ops.mode = MTD_OOB_AUTO;
			ops.datbuf = rd_buf;
			ops.len = mtd->writesize;
			ops.oobbuf = oob_buf;
			ops.ooboffs = 0;	
			ops.ooblen = mtd->oobsize;
				
		
			res = mtd->read_oob(mtd,phy_page<<mtdblk->pageshift, &ops);
			if(ops.retlen < mtd->writesize)
			{
				printk(KERN_ERR "FTL read failure");
				printk(KERN_ERR " phypage = %ld",phy_page);
				BUG();
			}
			oobdata = &oobvalues;
			memcpy(oobdata, oob_buf,sizeof(*oobdata));
			
			if(scanseqnumber[phy_page] <= oobdata->seq_number)
			{	
				reverse_map_tab[phy_page] = oobdata->logic_page_num;
				scanseqnumber[phy_page] = oobdata->seq_number;
			}
		
		}
	}
	clear_bit(bank,  &(mtdblk->ckptrd_mask));
	kfree(arg);
}


void parallel_page_scan(struct mtdblk_dev *mtdblk)
{
	struct task_struct *scan_thread_struct;
	int banknum;
	struct scan_thrd_info *scan_thread_arg;
	uint32_t start_blk,end_blk;
	int i;
	
	
	
	struct mtd_info *mtd;
	uint32_t num_blks_req;
	
	
	mtdblk->ckptrd_mask = 0xFFFFFFFFFFFFFFFF;
	
	printk(KERN_INFO " parallel page scan 0 -> %d", numpllbanks);
	for(banknum = 0; banknum < numpllbanks;banknum++)
	{
		start_blk = mtdblk->cur_writing[banknum].first_blk;
		end_blk = mtdblk->cur_writing[banknum].last_blk+1;
		
		scan_thread_arg = kmalloc(sizeof (struct scan_thrd_info),GFP_KERNEL);
		if(scan_thread_arg == NULL)
		{
			printk(KERN_ERR "kmalloc ckpt_thread_arg fail");
			BUG();
		}
		scan_thread_arg->mtdblk = mtdblk;
		scan_thread_arg->bank = banknum;
        			
		scan_thread_struct  = kthread_run(the_pagescan_thread, (scan_thread_arg),
				"pgscan_thrd %ld",banknum);

		if (IS_ERR(scan_thread_struct)) {
			PTR_ERR(scan_thread_struct);
			BUG();
		}
		
	}
	
	while(mtdblk->ckptrd_mask != 0ULL)
	{
		schedule();
	}
	
}
static int init_ftl(struct mymtd_blktrans_dev *mbd)
{

	int i;
	int ii;
	struct cache_num_node *node;
	struct mtdblk_dev *mtdblk = container_of(mbd, struct mtdblk_dev, mbd);
	int *arr;
	uint32_t map_table_size,blk_info_size,free_map_size,bank_info_size;
	
	uint32_t num_map_table_blks,num_blk_info_blks,num_freemap_blks,num_bankinfo_blks;
	
	uint32_t num_blks_req;
	uint32_t flash_blk_size = mbd->mtd->erasesize;
	uint64_t intermed_mask;
	uint64_t mask;
	int num_bits;
	mtdblk->init_not_done = 1;
	
#ifdef EIGHT_BANK_FLASH
	numpllbanks = 8;
#endif
	
	mtdblk->cache_size = mbd->mtd->writesize;
	mtdblk->num_parallel_banks =  numpllbanks;
	mtdblk->hwblks_per_bank = 4096;
	
	mtdblk->num_blks = ((mbd->size)<<(mtdblk->blkshift))/(mbd->mtd->erasesize);
	mtdblk->blks_per_bank = mtdblk->num_blks/mtdblk->num_parallel_banks;
	mtdblk->pages_per_blk = mbd->mtd->erasesize/mbd->mtd->writesize;
	/* 10% of blocks are reserved */
	mtdblk->reserved_blks_per_bank = (10*mtdblk->blks_per_bank)/100;
	if(sizeof(long unsigned int) != 8)
	{
		printk(KERN_INFO "written mask size is wrong");
		return -1;
	}
	for(i = 0; i < MAX_FTL_CACHEBUFS;i++)
	{
		//mtdblk->cached_buf[i].buf = vmalloc(mtdblk->cache_size);
		//if (!mtdblk->cached_buf[i].buf)
		//	return -EINTR;
		mutex_init(&mtdblk->buf_lock[i]);
		init_rwsem(&(mtdblk->bufstate_lock[i]));
		mtdblk->cached_buf[i].cache_state =  STATE_EMPTY;
		mtdblk->cached_buf[i].written_mask = 0ULL;
		mtdblk->cached_buf[i].logic_page = INVALID_PAGE_NUMBER;
		mtdblk->cached_buf[i].last_touch = jiffies;
		atomic_set( &mtdblk->cached_buf[i].writes_in_progress, 0 );
		atomic_set( &mtdblk->cached_buf[i].flush_in_progress, 0 );
		atomic_set( &mtdblk->cached_buf[i].wait_to_flush, 0 );

		for(ii = 0; ii < 64;ii++)
		{
			mtdblk->cached_buf[i].logic_sect_num[ii] = INVALID_SECT_NUM;
		}

	}
	mutex_init(&mtdblk->select_buf_lock);
	
	mutex_init(&mtdblk->flush_buf_lock);
	
	mutex_init(&mtdblk->exper_buf_lock);
	mtdblk->exper_buf = vmalloc(mtdblk->cache_size);
	mtdblk->exper_buf_sect_idx = 0;
	
	for(i = 0; i < MAX_FTL_CACHEBUFS;i++)
	{
		//mutex_init(&mtdblk->buf_lock[i]);
		mtdblk->buf[i] = vmalloc(mtdblk->cache_size);
		mtdblk->buf_idx[i] = 0;
	}
	
	
	for(i = 0; i < 64;i++)
	{
		init_rwsem(&(mtdblk->free_map_lock[i]));
	}
	init_rwsem(&map_tabl_lock);

	
	
			
	mtdblk->pageshift = ffs(mbd->mtd->writesize)-1;
	atomic_set(&mtdblk->freeblk_count,mtdblk->num_blks);
	 
	/*init_cur_wr info initialisation*/
	mtdblk->num_cur_wr_blks = mtdblk->num_parallel_banks;
	
	if(mtdblk->num_cur_wr_blks > MAX_FTL_CACHEBUFS){
		printk(KERN_ERR "num_cur_wr_blks > MAX_FTL_CACHEBUFS");
		BUG();
	}
#ifdef USE_VIRGO_RESTRICTED	
	printk(KERN_INFO "using index = %d",index_in_bank);
#endif
	for(i = 0; i < mtdblk->num_cur_wr_blks;i++)
	{
		
		init_rwsem(&(mtdblk->cur_wr_state[i]));
#ifdef USE_VIRGO_RESTRICTED
		
		mtdblk->cur_writing[i].first_blk = i*4096+index_in_bank*64;
		mtdblk->cur_writing[i].last_blk = mtdblk->cur_writing[i].first_blk + mtdblk->blks_per_bank -1;
		printk(KERN_INFO "bank %d [%ld %ld]",i,mtdblk->cur_writing[i].first_blk,mtdblk->cur_writing[i].last_blk);
#else
		mtdblk->cur_writing[i].first_blk = i*mtdblk->blks_per_bank;
		mtdblk->cur_writing[i].last_blk = mtdblk->cur_writing[i].first_blk + mtdblk->blks_per_bank -1;
#endif
		mtdblk->cur_writing[i].last_gc_blk = mtdblk->cur_writing[i].first_blk;
		mtdblk->cur_writing[i].blk = -1;
		mtdblk->cur_writing[i].last_wrpage = -1;
		mtdblk->cur_writing[i].centroid = -1;
		mtdblk->cur_writing[i].state = STATE_CLEAN;		
				
	}
	
#ifdef EIGHT_BANK_FLASH
	if(numpllbanks < 64)
	{
		int flash_bank;
		for(i = 0; i < numpllbanks;i++)
		{
			
			flash_bank = i*(64/numpllbanks);
			init_rwsem(&(mtdblk->cur_wr_state[i]));
	
			mtdblk->cur_writing[i].first_blk = flash_bank*mtdblk->hwblks_per_bank+(index_in_bank*64);
			mtdblk->cur_writing[i].last_blk = mtdblk->cur_writing[i].first_blk + mtdblk->blks_per_bank -1;
	
			mtdblk->cur_writing[i].last_gc_blk = mtdblk->cur_writing[i].first_blk;
			mtdblk->cur_writing[i].blk = -1;
			mtdblk->cur_writing[i].last_wrpage = -1;
			mtdblk->cur_writing[i].centroid = -1;
			mtdblk->cur_writing[i].state = STATE_CLEAN;		
					
		}
	}
#endif
	init_rwsem(&(mtdblk->rand_wr_state));
	mtdblk->rand_writing.blk = -1;
	mtdblk->rand_writing.last_wrpage = -1;
	mtdblk->rand_writing.state = STATE_CLEAN;	
			
	/* queues bufs buflook up table initialisation*/
	
	qnode_cache = kmem_cache_create("qnode_slab",
			sizeof(struct cache_num_node), 0,
			       SLAB_PANIC, NULL);
	
	if (!qnode_cache)
	{
		printk(KERN_ERR "kmemcachealloc fail");
		BUG();
	}
	
#if 0
	for(i = 0; i < MAP_TABLE_SIZE; i++)
		cache_num[i] = INVALID_CACHE_NUM;
#endif
	

	for(i = 0; i < MAX_FTL_CACHEBUFS; i++)
	{
		buf_lookup_tab[i] = INVALID_PAGE_NUMBER;
	}
	mutex_init(&mtdblk->buf_lookup_tab_mutex);
	
	
	lfq_init_rcu(&empty_bufsq, call_rcu);
	lfq_init_rcu(&full_bufsq, call_rcu);
	
	
	for(i = 0; i < MAX_FTL_CACHEBUFS;i++)
	{
		node = kmem_cache_alloc(qnode_cache, GFP_KERNEL);
		//node = kmalloc(sizeof(*node),GFP_KERNEL);
		if (!node)
		{
			printk(KERN_INFO "kmem_cache_alloc fail \n");	
			BUG();
		}
		node->value = i;
		lfq_node_init_rcu(&node->list);
		rcu_read_lock();
		lockfree_enqueue(&empty_bufsq, &node->list);
		rcu_read_unlock();
	}
	
	/* garbage collection initialisation*/
	atomic_set(&mtdblk->seq_num ,0);
	/* level0 GC: freeblks is half */
	mtdblk->gc_thresh[0] = 0;
	/* level1 GC: freeblks is quarter */
	mtdblk->gc_thresh[1] = mtdblk->pages_per_blk/8;
	/* level2 GC freeblks is 1/8th*/
	mtdblk->gc_thresh[2] = mtdblk->pages_per_blk/4;
	
	
	
	/* activity matrix initialisation*/
	for(i = 0; i < numpllbanks;i++)
	{
		atomic_set(&mtdblk->activity_matrix.num_reads[i],0);	
		atomic_set(&mtdblk->activity_matrix.num_writes[i],0);	
		atomic_set(&mtdblk->activity_matrix.gc_goingon[i],0);	
		atomic_set(&mtdblk->activity_matrix.num_reads_pref[i],0);	
		
	}
	
		
	
	
	/* prefetching initialisation*/
	lfq_init_rcu(&pfetch_bufsq, call_rcu);
	for(i = 0; i < MAX_FTL_PREFETCH_BUFS;i++)
	{
		#if 1
		node = kmem_cache_alloc(qnode_cache, GFP_KERNEL);
		//node = kmalloc(sizeof(*node),GFP_KERNEL);
		if (!node)
		{
			printk(KERN_INFO "kmem_cache_alloc fail \n");	
			BUG();
		}
		node->value = i;
		lfq_node_init_rcu(&node->list);
		rcu_read_lock();
		lockfree_enqueue(&pfetch_bufsq, &node->list);
		rcu_read_unlock();
		#endif
		
	}
	for(i = 0; i < MAX_FTL_PREFETCH_BUFS;i++)
	{
		init_rwsem(&(pfetch_buf_lock[i]));
	}
	
	for(i = 0; i < MAX_FTL_PREFETCH_BUFS; i++)
	{
		pfetch_lookup_tab[i] = INVALID_PAGE_NUMBER;
		prefetch_buf[i] = vmalloc(mbd->mtd->writesize);
		if(prefetch_buf[i] == NULL)
		{
			printk(KERN_ERR "prefetch buf alloc fail");
			BUG();
		}
		p_fetch_info[i].state = NOT_IN_USE;
		p_fetch_info[i].last_touch = jiffies;
		p_fetch_info[i].read_mask =0ULL;
		p_fetch_info[i].index_inlist = -1;
		
	}
	pref_buf_count = MAX_FTL_PREFETCH_BUFS;
	
	
	
	/* prefetch buffering initialisations */
#ifdef PER_BUFFER_FLUSH
	for(i =0; i < MAX_FTL_PREFETCH_BUFS;i++)
	{
		prefetch_buf_sched[i].buf_num = i;
		INIT_DELAYED_WORK(&prefetch_buf_sched[i].workq, give_up_prefetch_buf);
	}
#else

#ifdef BUFFLUSHD
	mtdblk->bufflushd  = kthread_run(wbuf_flush_thread, (mtdblk),	"wbufflushdmn");

	if (IS_ERR(mtdblk->bufflushd)) {
		PTR_ERR(mtdblk->bufflushd);
		BUG();
	}
#endif

#endif
	
#ifdef PREFETCH_ACL	
	mtdblk->bufflushd  = kthread_run(buf_flush_thread, (mtdblk),	"bufflushthrd");

	if (IS_ERR(mtdblk->bufflushd)) {
		PTR_ERR(mtdblk->bufflushd);
		BUG();
	}
#endif
	
	
	
	lfq_init_rcu(&spare_bufQ, call_rcu);
	lfq_init_rcu(&spare_oobbufQ, call_rcu);
	for(i = 0; i < MAX_FTL_CACHEBUFS ;i++)
	{
		spare_cache_list_ptr[i] = vmalloc(mbd->mtd->writesize);
		if(spare_cache_list_ptr[i] == NULL)
		{
			printk(KERN_ERR "myftl: sparebufs init fail");
			BUG();
		}
		put_spare_buf(spare_cache_list_ptr[i]);
	}

	mtdblk->FFbuf= vmalloc(mbd->mtd->writesize);

	if(mtdblk->FFbuf == NULL)
	{
		printk(KERN_ERR "myftl: sparebufs init fail");
		BUG();
	}
	memset(mtdblk->FFbuf,0xFF,mbd->mtd->writesize);
	
	for(i = 0; i < MAX_FTL_CACHEBUFS ;i++)
	{
		spare_oobbuf_list_ptr[i] = vmalloc(mbd->mtd->oobsize);
		if(spare_oobbuf_list_ptr[i] == NULL)
		{
			printk(KERN_ERR "myftl: sparebufs init fail");
			BUG();
		}
		put_spare_oobbuf(spare_oobbuf_list_ptr[i]);
	}
	
	map_table_size = mtdblk->pages_per_blk * mtdblk->num_blks * sizeof(uint32_t);
	blk_info_size = mtdblk->num_blks * sizeof(struct per_blk_info);
	bank_info_size = mtdblk->num_parallel_banks * sizeof(struct per_bank_info);
	free_map_size = mtdblk->num_blks/8;
	
	uint32_t num_map_table_pages;
	uint32_t num_blk_info_pages ;
	uint32_t num_freemap_pages ;
	uint32_t num_bankinfo_pages ;
	uint32_t flash_page_size;
	uint32_t total_ckpt_pages;
	
	flash_page_size = mbd->mtd->writesize;
			
	num_map_table_pages = (map_table_size/flash_page_size) +((map_table_size)%(flash_page_size) ? 1 : 0);
	num_blk_info_pages = (blk_info_size/flash_page_size)+((blk_info_size)%(flash_page_size) ? 1 : 0);
	num_freemap_pages = (free_map_size/flash_page_size) +((free_map_size)%(flash_page_size) ? 1 : 0);
	num_bankinfo_pages = (bank_info_size/flash_page_size)+((bank_info_size)%(flash_page_size) ? 1 : 0);
	
	total_ckpt_pages = num_map_table_pages+num_blk_info_pages+num_freemap_pages+num_bankinfo_pages;
	num_blks_req = total_ckpt_pages/mtdblk->pages_per_blk+((total_ckpt_pages)%(mtdblk->pages_per_blk) ? 1 : 0);
#if 0
	num_map_table_blks = (map_table_size/flash_blk_size) +((map_table_size)%(flash_blk_size) ? 1 : 0);
	num_blk_info_blks = (blk_info_size/flash_blk_size)+((blk_info_size)%(flash_blk_size) ? 1 : 0);
	num_freemap_blks = (free_map_size/flash_blk_size) +((free_map_size)%(flash_blk_size) ? 1 : 0);
	num_bankinfo_blks = (bank_info_size/flash_blk_size)+((bank_info_size)%(flash_blk_size) ? 1 : 0);
#endif
	
	printk(KERN_INFO "init ftl maptabpages = %d %d",num_map_table_pages,num_map_table_pages/mtdblk->pages_per_blk);
	printk(KERN_INFO "init ftl blkinfopages = %d %d",num_blk_info_pages,num_blk_info_pages/mtdblk->pages_per_blk);
	printk(KERN_INFO "init ftl freemapages = %d %d",num_freemap_pages,num_freemap_pages/mtdblk->pages_per_blk);
	printk(KERN_INFO "init ftl bankinfopages = %d %d",num_bankinfo_pages,num_bankinfo_pages/mtdblk->pages_per_blk);
	
	
	printk(KERN_INFO "init ftl numblksreq = %d",num_blks_req);
	
	int ckpt_not_found;
	/* assume ckpt not found now.*/
	ckpt_not_found = 1;
	
	if(first_time == 0)
	{
#ifdef CKPT_FTL
		arr = vmalloc(num_blks_req*(sizeof(int)));
		if(arr == NULL)
		{
			printk(KERN_INFO "vmalloc fail");
			BUG();
		}
		for(i = 0;i < num_blks_req;i++)
		{
			arr[i] = -1;
		}
	#if 0
		read_ckpt(mtdblk,0,mtdblk->num_blks,arr);
	#else
		//parll_blk_scan(mtdblk,arr);
		bounded_pll_blk_scan(mtdblk,arr);
	#endif
	
	
		/* we have done the scan ; probably we have found the ckpt*/
		ckpt_not_found = 0;
		for(i = 0;i < num_blks_req;i++)
		{
			if(arr[i] == -1)
			{
				ckpt_not_found = 1;
				break;
			}
			else
			{
				/* clear away this particular map block*/
				atomic_set(&blk_info[i].num_valid_pages,0);
				bitmap_zero((blk_info[i].valid_pages_map),64);
				async_erase(mtdblk,i);
				blk_free(mtdblk,i);
			}
		}
		vfree(arr);
	
	}
	
	if(first_time == 1 || ckpt_not_found == 1 )
#endif
	{
		if(first_time != 1 && ckpt_not_found == 1)
		{
			parallel_page_scan(mtdblk);
		}
		
		/*reinitialise*/
		printk(KERN_INFO "ckpt  not found and pagescan done");
		int bank_count;  int j =0;
		for(i = 0; i < MAP_TABLE_SIZE/64; i++)
		{
			for(bank_count = 0; bank_count < 64;bank_count++)
			{
				map_table[j] = i+bank_count*(mtdblk->blks_per_bank*mtdblk->pages_per_blk);
				if(map_table[j] > MAP_TABLE_SIZE)
				{
					printk(KERN_INFO "maptabe wrong %ld %d %d",map_table[j],i,bank_count);
				}
				j++;
			}
		}
		printk(KERN_INFO "%ld entries init %ld",j,MAP_TABLE_SIZE);
		/* blk info initialisation */
		for(i = 0; i < mtdblk->num_blks;i++)
		{

			/* the value here should be INVALID */
			atomic_set(&blk_info[i].num_valid_pages,0);
		
			bitmap_zero((blk_info[i].valid_pages_map),64);
		
		}
		/* bank info initialisation */
		for(i = 0; i < numpllbanks ;i++)
		{
			atomic_set(&bank_info[i].perbank_nfree_blks,mtdblk->blks_per_bank);
			atomic_set(&bank_info[i].perbank_ndirty_pages,0);
		 
		}
		bitmap_zero((mtdblk->free_blk_map),262144);
		
		if(gc_testing_on == 1)
		{
			for(i = 0; i < MAP_TABLE_SIZE;i++)
				reverse_map_tab[i] = INVALID_PAGE_NUMBER_32;
		}
	
		
	}
	
	list_lru_init(&fdirty_bufs_list);
	list_lru_init(&empty_bufs_list);
	list_lru_init(&dirty_bufs_list);
	
	atomic_set(&num_gcollected,0);
	atomic_set(&num_gc_threads,0);
	atomic_set(&num_prefetch_threads,0);
	atomic_set(&gc_on_writes_collisions,0);
	atomic_set(&num_gc_wakeups,0);
	
	atomic_set(&num_l0_gcollected,0);
	atomic_set(&num_l1_gcollected,0);
	atomic_set(&num_l2_gcollected,0);
	atomic_set(&num_erase_gcollected,0);
	atomic_set(&num_cperase_gcollected,0);

	struct cache_buf_list *cachebuftmp;
	for(i = 0; i < MAX_FTL_CACHEBUFS;i++)
	{
		cachebuftmp = kmalloc(sizeof(struct cache_buf_list),GFP_KERNEL);
		if(cachebuftmp != NULL)
		{
			
			cachebuftmp->value = i;	
			cache_list_ptr[i] = cachebuftmp;
			
			list_lru_add(&empty_bufs_list,&(cachebuftmp->list));
		}
		else
		{
			printk(KERN_INFO " cache_buflist  alloc fail");
			BUG();
		}
	}

	
	atomic_set(&activenumgcthread,NUM_GC_THREAD);
	//#ifndef NON_SCHEDULED	
	#ifdef BG_C_GC
	for(i =	0; i < NUM_GC_THREAD;i++)
	{
		mtdblk->gcthrd_arg[i].mtdblk_ptr = mtdblk;
		mtdblk->gcthrd_arg[i].thrdnum = i;
		
		mtdblk->ftlgc_thrd[i]  = kthread_run(check_and_dogc_thrd, &(mtdblk->gcthrd_arg[i]), "gcthrd");
	
		if (IS_ERR(mtdblk->ftlgc_thrd[i])) {
			PTR_ERR(mtdblk->ftlgc_thrd[i]);
			BUG();
		}
	}
	#endif
	
	#ifdef BG_UNC_GC
	for(i =	0; i < NUM_GC_THREAD;i++)
	{
		
		mtdblk->gcthrd_arg[i].mtdblk_ptr = mtdblk;
		mtdblk->gcthrd_arg[i].thrdnum = i;
		
		mtdblk->ftlgc_thrd[i]  = kthread_run(do_naive_gc_thrd, &(mtdblk->gcthrd_arg[i]), "gcthrd");
	
		if (IS_ERR(mtdblk->ftlgc_thrd[i])) {
			PTR_ERR(mtdblk->ftlgc_thrd[i]);
			BUG();
		}
		
		
	}
	#endif
	

		
	int bank;
	for(bank = 0; bank < numpllbanks;bank++)
	{
		cost_thresh[bank][0] = init_cost_thresh[0];
		cost_thresh[bank][1] = init_cost_thresh[1];
		cost_thresh[bank][2] = init_cost_thresh[2];
		cost_thresh[bank][3] = init_cost_thresh[3];
	}
	
	
	scheduled_for_gc[0] = -1;
	scheduled_for_gc[1] = -1;
	
	
#ifdef PREFETCH_ACL	
	for(i = 0; i < MAX_ACCESS_LIST ;i++)
	{
		mtdblk->accesslist[i] = INVALID_PAGE_NUMBER_32;
	}
	mtdblk->acc_listindex = 0;
	
	for(i = 0; i < MAX_PREF_THREAD; i++)
	{	
		mtdblk->ftl_prefetch_thrd[i]  = kthread_run(prefetch_access_list, mtdblk, "pftchthrd");

		if (IS_ERR(mtdblk->ftl_prefetch_thrd[i])) {
			PTR_ERR(mtdblk->ftl_prefetch_thrd[i]);
			BUG();
		}
	}
#endif

#if 0
	struct CacheNumNode *tmp;
	tmp = kmalloc(sizeof(struct CacheNumNode),GFP_KERNEL);
	if(tmp == NULL)
	{
		printk(KERN_INFO " sentinel kmalloc fail");
		BUG();
	}
	tmp->num = INVALID_VALUE;
	tmp->next = NULL;
	init_lock_freeQ(&mtdblk->lfree_empty_bufsq,tmp);
	
	tmp = kmalloc(sizeof(struct CacheNumNode),GFP_KERNEL);
	if(tmp == NULL)
	{
		printk(KERN_INFO " sentinel kmalloc fail");
		BUG();
	}
	tmp->num = INVALID_VALUE;
	tmp->next = NULL;
	init_lock_freeQ(&mtdblk->lfree_full_dirty_bufsq,tmp);	
	
	for(i = 0; i < MAX_FTL_CACHEBUFS;i++)
	{
		tmp = kmalloc(sizeof(struct CacheNumNode),GFP_KERNEL);
		tmp->num = i;
		tmp->next = NULL;
		if(tmp != NULL)
		{
			
			lockfree_enq(&(mtdblk->lfree_empty_bufsq),tmp);
			cache_list_ptr[i] = tmp;
		}
		else
		{
			printk(KERN_INFO " empty list alloc fail");
			BUG();
		}
	}
#endif
	num_bits = mtdblk->cache_size/mtdblk->blksize;
	
	/*mask = ~((-1)UL)<<num_bits*/
	if(num_bits == 64)
	{
		mask = -1ULL;
	}
	else if(num_bits < 64)
	{
		intermed_mask = -1ULL;
		for(i = 0;i < num_bits;i++)
			intermed_mask = intermed_mask <<1;
		mask = ~intermed_mask;
	}
	else
	{
		printk(KERN_ERR "cachesize blksize not supported");
		BUG();
	}
	
	mtdblk->cache_fullmask = mask;
	mtdblk->last_wr_time =0;
	mtdblk->init_not_done = 0;
	return 0;
}

int wr_ckpt(struct mtdblk_dev *mtdblk)
{
	struct oob_data oobvalues,*oobdata;
	uint32_t map_table_size;
	uint32_t blk_info_size;
	uint32_t freeblkmap_size;
	uint32_t bank_info_size;
	uint32_t flashblksize;
	uint8_t *oob_buf;
	uint32_t pages_written;
	uint64_t phy_addr;
	uint32_t blk;
	uint32_t wr_len;
	uint32_t retval;
	uint32_t size;
	struct mtd_oob_ops ops;
	uint8_t *temp_buf;
	uint32_t page_shift = mtdblk->pageshift;
	uint32_t blks_written;
	struct mtd_info *mtd;
	uint8_t *wr_buf;
	mtd = mtdblk->mbd.mtd;
#if 1
	flashblksize = (mtd->erasesize);
	oob_buf = vmalloc(mtd->oobsize);
	wr_buf =  vmalloc(mtd->writesize);
	if (!oob_buf|| !wr_buf)
	{
		printk(KERN_INFO "myftl deinit: vmalloc fail");
		BUG();
		//return -EINTR;
	}
	 
	map_table_size = mtdblk->pages_per_blk * mtdblk->num_blks * sizeof(uint32_t);
	blk_info_size = mtdblk->num_blks * sizeof(struct per_blk_info);
	bank_info_size = mtdblk->num_parallel_banks* sizeof(struct per_bank_info);
	freeblkmap_size = mtdblk->num_blks/8;
	
	blks_written = 0;
	oobdata = &oobvalues;
			
	
	oobdata->seq_number = -1;
	oobdata->logic_page_num = INVALID_PAGE_NUMBER;
	oobdata->blk_type = MAP_BLK;
	
	
	
	for(size = 0,pages_written = mtdblk->pages_per_blk; size < map_table_size;)
	{
		if(pages_written < mtdblk->pages_per_blk)
		{
			phy_addr++;
		}
		else
		{
			blk = exper_alloc_block(mtdblk,RAND_SEL);
			
			blks_written++;
			if(blk == INVALID)
			{
				printk(KERN_INFO "no enough block to checkpoint");
				BUG();
			}
			phy_addr = blk*mtdblk->pages_per_blk;
			pages_written = 0;
			oobdata->seq_number++;
			printk(KERN_INFO "wrckpt: maptab blk =%ld seqnum = %ld",blk,oobdata->seq_number);
		}
		
		
#if 1
		printk(KERN_INFO "phyaddr = %u blk = %ld",phy_addr,phy_addr/mtdblk->pages_per_blk);
#endif
		
		if((map_table_size -(size+mtd->writesize)) > 0)
		{
			wr_len = mtd->writesize;
			temp_buf = ((uint8_t*)map_table)+size;
		}
		else
		{
			wr_len = map_table_size -size;
			temp_buf = ((uint8_t*)map_table)+size;
			memset(wr_buf,0xFF,mtd->writesize);
			memcpy(wr_buf,temp_buf,wr_len);
			temp_buf = wr_buf;
		}
		
		
		
		memcpy(oob_buf,oobdata,sizeof(*oobdata));
		int mycount;
		printk(KERN_INFO "wr oobbuf");
		printk(KERN_INFO " ");
		for(mycount = 0; mycount < sizeof(*oobdata);mycount++)
			printk(" %d ",oob_buf[mycount]);
		
		ops.mode = MTD_OOB_AUTO;
		ops.ooblen = mtd->oobsize;
		ops.len = mtd->writesize;
		ops.retlen = 0;
		ops.oobretlen = 0;
		ops.ooboffs = 0;
		ops.datbuf = temp_buf;
		ops.oobbuf = oob_buf;
		retval = 1;
		
		retval = mtd->write_oob(mtd,(phy_addr<<page_shift), &ops);
		if(ops.retlen != wr_len)
		{
			printk("myftl: gc mtd write fail");
			BUG();			
			return -1;
		}
		
		size += wr_len;
		pages_written++;
	}
	
	printk(KERN_INFO "map tab ckptd");
	for(size = 0; size < blk_info_size;)
	{
		if(pages_written < mtdblk->pages_per_blk)
		{
			phy_addr++;
		}
		else
		{
			blk = exper_alloc_block(mtdblk,RAND_SEL);
			blks_written++;
			if(blk == INVALID)
			{
				printk(KERN_INFO "no enough block to checkpoint");
				BUG();
			}
			phy_addr = blk*mtdblk->pages_per_blk;
			pages_written = 0;
			oobdata->seq_number++;
			printk(KERN_INFO "wrckpt: blkinfo blk =%ld seqnum = %ld",blk,oobdata->seq_number);
		}

		
#if 0
		printk(KERN_INFO "phyaddr = %u",phy_addr);
#endif
		
		if((blk_info_size -(size+mtd->writesize)) > 0)
		{
			wr_len = mtd->writesize;
			temp_buf = ((uint8_t*)blk_info)+size;
		}
		else
		{
			wr_len = blk_info_size -size;
			
			
			temp_buf = ((uint8_t*)blk_info)+size;
			memset(wr_buf,0xFF,mtd->writesize);
			memcpy(wr_buf,temp_buf,wr_len);
			temp_buf = wr_buf;
		}
		
		
		memcpy(oob_buf,oobdata,sizeof(*oobdata));
		
		ops.mode = MTD_OOB_AUTO;
		ops.ooblen = mtd->oobsize;
		ops.len = mtd->writesize;
		ops.retlen = 0;
		ops.oobretlen = 0;
		ops.ooboffs = 0;
		ops.datbuf = temp_buf;
		ops.oobbuf = oob_buf;
		retval = 1;
		
		retval = mtd->write_oob(mtd,(phy_addr<<page_shift), &ops);
		if(ops.retlen != wr_len)
		{

			printk("myftl: gc mtd write fail");
				
			BUG();			
			return -1;
		}
		
		size += wr_len;
		pages_written++;
				
	}
	printk(KERN_INFO "blkinfo ckptd");
	for(size = 0; size < freeblkmap_size;)
	{
		if(pages_written < mtdblk->pages_per_blk)
		{
			phy_addr++;
		}
		else
		{
			blk = exper_alloc_block(mtdblk,RAND_SEL);
			if(blk == INVALID)
			{
				printk(KERN_INFO "no enough block to checkpoint");
				BUG();
			}
			phy_addr = blk*mtdblk->pages_per_blk;
			pages_written = 0;
			oobdata->seq_number++;
			printk(KERN_INFO "wrckpt: freemap blk =%ld seqnum = %ld",blk,oobdata->seq_number);
		}

	
#if 0
		printk(KERN_INFO "phyaddr = %u",phy_addr);
#endif
		
		if((freeblkmap_size -(size+mtd->writesize)) > 0)
		{
			wr_len = mtd->writesize;
			temp_buf = ((uint8_t*)mtdblk->free_blk_map)+size;
		}
		else
		{
			wr_len = freeblkmap_size-size;
			
			temp_buf = ((uint8_t*)mtdblk->free_blk_map)+size;
			memset(wr_buf,0xFF,mtd->writesize);
			memcpy(wr_buf,temp_buf,wr_len);
			temp_buf = wr_buf;
		}
		
		
		memcpy(oob_buf,oobdata,sizeof(*oobdata));
		
		ops.mode = MTD_OOB_AUTO;
		ops.ooblen = mtd->oobsize;
		ops.len = mtd->writesize;
		ops.retlen = 0;
		ops.oobretlen = 0;
		ops.ooboffs = 0;
		ops.datbuf = temp_buf;
		ops.oobbuf = oob_buf;
		retval = 1;
		
		retval = mtd->write_oob(mtd,(phy_addr<<page_shift), &ops);
		if(ops.retlen != wr_len)
		{

			printk("myftl: gc mtd write fail");
				
			BUG();			
			return -1;
		}
		
		size += wr_len;
		pages_written++;
				
	}
	printk(KERN_INFO "freeblkmap ckptd");
	
	for(size = 0; size < bank_info_size;)
	{
		if(pages_written < mtdblk->pages_per_blk)
		{
			phy_addr++;
		}
		else
		{
			blk = exper_alloc_block(mtdblk,RAND_SEL);
			blks_written++;
			if(blk == INVALID)
			{
				printk(KERN_INFO "no enough block to checkpoint");
				BUG();
			}
			phy_addr = blk*mtdblk->pages_per_blk;
			pages_written = 0;
			oobdata->seq_number++;
			printk(KERN_INFO "wrckpt: bankinfo blk =%ld seqnum = %ld",blk,oobdata->seq_number);
		}

		
#if 0
		printk(KERN_INFO "phyaddr = %u",phy_addr);
#endif
		
		if((bank_info_size -(size+mtd->writesize)) > 0)
		{
			wr_len = mtd->writesize;
			temp_buf = ((uint8_t*)bank_info)+size;
		}
		else
		{
			wr_len = bank_info_size -size;
			temp_buf = ((uint8_t*)bank_info)+size;
			
			memset(wr_buf,0xFF,mtd->writesize);
			memcpy(wr_buf,temp_buf,wr_len);
			temp_buf = wr_buf;
		}
		
		
		memcpy(oob_buf,oobdata,sizeof(*oobdata));
		
		ops.mode = MTD_OOB_AUTO;
		ops.ooblen = mtd->oobsize;
		ops.len = mtd->writesize;
		ops.retlen = 0;
		ops.oobretlen = 0;
		ops.ooboffs = 0;
		ops.datbuf = temp_buf;
		ops.oobbuf = oob_buf;
		retval = 1;
		
		retval = mtd->write_oob(mtd,(phy_addr<<page_shift), &ops);
		if(ops.retlen != wr_len)
		{

			printk("myftl: gc mtd write fail");
				
			BUG();			
			return -1;
		}
		
		size += wr_len;
		pages_written++;
				
	}
	
	
	printk(KERN_INFO "bankinfo ckptd");
	
	
	vfree(oob_buf);
	printk(KERN_INFO " wr_ckpt = %d",blks_written);
	return 0;
#endif
}




int deinit_ftl(struct mymtd_blktrans_dev *mbd)
{
	int i;
	struct mtd_info *mtd;
	
	struct mtdblk_dev *mtdblk = container_of(mbd, struct mtdblk_dev, mbd);
	
	
	
	mtd = mtdblk->mbd.mtd;
	
#ifdef CKPT_FTL
	//wr_ckpt(mtdblk);
	wr_ckpt_chained(mtdblk);
#endif
	
	
	//for(i = 0; i < MAX_FTL_CACHEBUFS;i++)
	{
	//	vfree(mtdblk->cached_buf[i].buf);
	}

	
	printk(KERN_INFO "deinit_ftl\n");
	#ifdef BUFFLUSHD
	kthread_stop(mtdblk->bufflushd);
	#endif
	
	
	
	/* have to dequeue all the bufindexes here */
	lfq_destroy_rcu(&empty_bufsq);
	lfq_destroy_rcu(&full_bufsq);
	lfq_destroy_rcu(&pfetch_bufsq);
	return 0;
}

static int mtdblock_open(struct mymtd_blktrans_dev *mbd)
{
	struct mtdblk_dev *mtdblk = container_of(mbd, struct mtdblk_dev, mbd);
	

	printk(KERN_INFO "FTLblock_open\n");

	mutex_lock(&mtdblks_lock);
	if (mtdblk->count) {
		mtdblk->count++;
		mutex_unlock(&mtdblks_lock);
		return 0;
	}

	
	/* OK, it's not open. Create cache info for it */
	mtdblk->count = 1;
	if (!(mbd->mtd->flags & MTD_NO_ERASE)) {
		mtdblk->cache_size = mbd->mtd->writesize;
	}

	
	mutex_unlock(&mtdblks_lock);
	
	printk(KERN_INFO "=========FTL open===========");
	printk(KERN_INFO "FTL mbd size = %lld",mtdblk->mbd.size);
	printk(KERN_INFO "FTL parallel banks = %d", mtdblk->num_parallel_banks);
	printk(KERN_INFO "FTL number of blks = %d", mtdblk->num_blks);
	printk(KERN_INFO "FTL blksperbank = %d",mtdblk->blks_per_bank);
	printk(KERN_INFO "FTL pagesperblk = %d",mtdblk->pages_per_blk);
	printk(KERN_INFO "FTL mtdblksize = %d", mtdblk->blksize);
	printk(KERN_INFO "FTL pagesize = %d",mbd->mtd->writesize);
	printk(KERN_INFO "FTL oobsize = %d",mbd->mtd->oobsize);
	printk(KERN_INFO "FTL flashblksize = %d",mbd->mtd->erasesize);
	printk(KERN_INFO "FTL cachesize = %d",mtdblk->cache_size);
	printk(KERN_INFO "FTL pageshift = %d",mtdblk->pageshift);
	printk(KERN_INFO "FTL blkshift = %d", mtdblk->blkshift);
	printk(KERN_INFO "FTL num_cur_wr_blks = %d",mtdblk->num_cur_wr_blks);
	uint64_t temp = (((uint64_t)mtdblk->num_blks)*mtdblk->pages_per_blk*mbd->mtd->writesize);
	temp = temp/512;
	printk(KERN_INFO "FTL sectors 0 -> %lld",(temp));
	printk(KERN_INFO "cache_fullmask = %llx",mtdblk->cache_fullmask);
	printk(KERN_INFO "FTLblock_open ok\n");

	return 0;
}


static int flush_cached_data(struct mtdblk_dev *mtdblk)
{
	int i;
	printk(KERN_INFO "FTL:flush_cached_data \n");
#if 0
	for(i = 0; i < MAX_FTL_CACHEBUFS;i++)
	{
		if(flush_to_flash(mtdblk,i) == -1)
		{
			printk(KERN_ERR "FTL: flush_to_flash error");
			BUG();
			
		}
	}
#endif

	return 0;
} 
static int mtdblock_release(struct mymtd_blktrans_dev *mbd)
{
	struct mtdblk_dev *mtdblk = container_of(mbd, struct mtdblk_dev, mbd);

	printk(KERN_INFO "FTL mtdblock_release ");

	mutex_lock(&mtdblks_lock);

	
	flush_cached_data(mtdblk);
	

	if (!--mtdblk->count) {
		/* It was the last usage. Free the cache */
		if (mbd->mtd->sync)
			mbd->mtd->sync(mbd->mtd);
		
	}

	mutex_unlock(&mtdblks_lock);

	DEBUG(MTD_DEBUG_LEVEL1, "ok\n");

	return 0;
}

static int mtdblock_flush(struct mymtd_blktrans_dev *dev)
{
	struct mtdblk_dev *mtdblk = container_of(dev, struct mtdblk_dev, mbd);

	printk(KERN_INFO "FTL mtdblock_flush ");

	flush_cached_data(mtdblk);


	if (dev->mtd->sync)
		dev->mtd->sync(dev->mtd);
	return 0;
}

static void mtdblock_add_mtd(struct mtd_blktrans_ops *tr, struct mtd_info *mtd)
{
	struct mtdblk_dev *dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	unsigned num_tabl_sects;
	uint64_t restricted_size;
	
	printk(KERN_ERR "FTL mtdblock_add_mtd ");

	
	if (!dev)
		return;

	dev->mbd.mtd = mtd;
	dev->mbd.devnum = mtd->index;

	
	dev->mbd.tr = tr;
	dev->blksize = tr->blksize;
	dev->blkshift = ffs(dev->blksize) - 1;
	
	
	/* 8 bytes per table entry*/
#ifdef USE_VIRGO_RESTRICTED
	//num_tabl_sects = ((mtd->size/mtd->writesize)*8)/dev->blksize;
	
	/* mapping table is in RAM*/
	num_tabl_sects = 0;
	restricted_size = 8Ull*1024*1024*1024;
	dev->mbd.size = (restricted_size >>(dev->blkshift));
	
#else
	num_tabl_sects = 0;
	dev->mbd.size = (mtd->size >>(dev->blkshift)) - num_tabl_sects;
#endif
	
#ifdef EIGHT_BANK_FLASH
	
	num_tabl_sects = 0;
	restricted_size = 1Ull*1024*1024*1024;
	dev->mbd.size = (restricted_size >>(dev->blkshift));
#endif
	
	printk(KERN_INFO "mbd size = %lld",dev->mbd.size);
	if (!(mtd->flags & MTD_WRITEABLE))
		dev->mbd.readonly = 1;

	if (add_mymtd_blktrans_dev(&dev->mbd))
		kfree(dev);
	
	if(init_ftl(&(dev->mbd))!= 0)
	{
		
		printk(KERN_ERR "FTL init fail");
		BUG();
	}
	
	struct mtdblk_dev *mtdblk = dev;
	
	printk(KERN_INFO "FTL mbd size = %lld",mtdblk->mbd.size);
	printk(KERN_INFO "FTL parallel banks = %d", mtdblk->num_parallel_banks);
	printk(KERN_INFO "FTL number of blks = %d", mtdblk->num_blks);
	printk(KERN_INFO "FTL blksperbank = %d",mtdblk->blks_per_bank);
	printk(KERN_INFO "FTL res_blksperbank = %d",mtdblk->reserved_blks_per_bank);
	printk(KERN_INFO "FTL pagesperblk = %d",mtdblk->pages_per_blk);
	printk(KERN_INFO "FTL mtdblksize = %d", mtdblk->blksize);
	printk(KERN_INFO "FTL pagesize = %d",dev->mbd.mtd->writesize);
	printk(KERN_INFO "FTL oobsize = %d",dev->mbd.mtd->oobsize);
	printk(KERN_INFO "FTL flashblksize = %d",dev->mbd.mtd->erasesize);
	printk(KERN_INFO "FTL cachesize = %d",mtdblk->cache_size);
	printk(KERN_INFO "FTL pageshift = %d",mtdblk->pageshift);
	printk(KERN_INFO "FTL blkshift = %d", mtdblk->blkshift);
	printk(KERN_INFO "FTL num_cur_wr_blks = %d",mtdblk->num_cur_wr_blks);
	uint64_t temp = (((uint64_t)mtdblk->num_blks)*mtdblk->pages_per_blk*dev->mbd.mtd->writesize);
	temp = temp/512;
	printk(KERN_INFO "FTL sectors 0 -> %lld",(temp));
	printk(KERN_INFO "cache_fullmask = %llx",mtdblk->cache_fullmask);
}

int pr_bank_info(struct mymtd_blktrans_dev *dev)
{
	int i;
	struct mtdblk_dev *mtdblk = container_of(dev, struct mtdblk_dev, mbd);
	struct mtd_info *mtd = mtdblk->mbd.mtd;
	uint32_t total_occupiedblks =0;
	uint32_t freeblks =0;
	
	printk(KERN_INFO "===GET INFO==");
	printk(KERN_INFO "FTL mbd size = %lld",mtdblk->mbd.size);
	printk(KERN_INFO "FTL parallel banks = %d", mtdblk->num_parallel_banks);
	printk(KERN_INFO "FTL number of blks = %d", mtdblk->num_blks);
	printk(KERN_INFO "FTL blksperbank = %d",mtdblk->blks_per_bank);
	printk(KERN_INFO "FTL pagesperblk = %d",mtdblk->pages_per_blk);
	printk(KERN_INFO "FTL mtdblksize = %d", mtdblk->blksize);
	printk(KERN_INFO "FTL pagesize = %d",mtd->writesize);
	printk(KERN_INFO "FTL oobsize = %d",mtd->oobsize);
	printk(KERN_INFO "FTL flashblksize = %d",mtd->erasesize);
	printk(KERN_INFO "FTL cachesize = %d",mtdblk->cache_size);
	printk(KERN_INFO "FTL pageshift = %d",mtdblk->pageshift);
	printk(KERN_INFO "FTL blkshift = %d", mtdblk->blkshift);
	printk(KERN_INFO "FTL num_cur_wr_blks = %d",mtdblk->num_cur_wr_blks);
	
	for(i = 0; i < mtdblk->num_cur_wr_blks;i++)
	{
		printk(KERN_INFO "FTL bank %d = %d %d %d",i,mtdblk->cur_writing[i].first_blk,mtdblk->cur_writing[i].last_blk,bank_info[i].perbank_nfree_blks.counter);
		freeblks += bank_info[i].perbank_nfree_blks.counter;	
	}
		
	
	printk(KERN_INFO "num gcL0 calls = %ld ",num_l0_gcollected.counter);
	printk(KERN_INFO "num gcL1 calls = %ld ",num_l1_gcollected.counter);
	printk(KERN_INFO "num gcL2 calls = %ld ",num_l2_gcollected.counter);
	
	
	printk(KERN_INFO "num blks gc'd = %ld ",num_gcollected.counter);
	printk(KERN_INFO "num blks only er = %ld ",num_erase_gcollected.counter);
	printk(KERN_INFO "num blks  er andcp = %ld ",num_cperase_gcollected.counter);
	
	printk(KERN_INFO "numgc wakeups = %ld",num_gc_wakeups.counter);
	
	printk(KERN_INFO "number of writes with GC on = %ld ",gc_on_writes_collisions.counter);
	
	
		
	total_occupiedblks = mtdblk->num_blks - freeblks;
		
	printk(KERN_INFO "num blks occupied = %ld %ld %ld",total_occupiedblks,mtdblk->num_blks,mtdblk->blks_per_bank);
	
		
	return 0;
	
}
	
static void mtdblock_remove_dev(struct mymtd_blktrans_dev *dev)
{
	printk(KERN_ERR	"FTL deinit ");
	if(deinit_ftl((dev))!= 0)
	{
		
		printk(KERN_ERR "FTL diinit fail");
		BUG();
	}
	
	del_mymtd_blktrans_dev(dev);
	
}


struct file* file_open(const char* path, int flags, int rights) {
	struct file* filp = NULL;
	mm_segment_t oldfs;
	int err = 0;

	oldfs = get_fs();
	set_fs(get_ds());
	filp = filp_open(path, flags, rights);
	set_fs(oldfs);
	if(IS_ERR(filp)) {
		err = PTR_ERR(filp);
		return NULL;
	}
	return filp;
}

void file_close(struct file* file) {
	filp_close(file, NULL);
}

int file_read(struct file* file, unsigned long long offset, unsigned char* data, unsigned int size) {
	mm_segment_t oldfs;
	int ret;

	oldfs = get_fs();
	set_fs(get_ds());

	ret = vfs_read(file, data, size, &offset);

	set_fs(oldfs);
	return ret;
}

int file_write(struct file* file, unsigned long long offset, unsigned char* data, unsigned int size) {
	mm_segment_t oldfs;
	int ret;

	oldfs = get_fs();
	set_fs(get_ds());

	ret = vfs_write(file, data, size, &offset);

	set_fs(oldfs);
	return ret;
}




static void write_file_freeblks(struct mtdblk_dev * mtdblk)
{
	struct file *file;
	static loff_t pos = 0;
	char test_str[10];
	struct timeval tv;
	char print_str[64];
	int i;
	
	
	file = file_open("/home/srimugunthan/ftl_log1", O_WRONLY|O_CREAT, 0644);
	if(file == NULL)
	{
	
		printk(KERN_INFO "dbg file open error");
		return;
	}


 



	for(i = 0; i < numpllbanks;i++)
	{
		snprintf(print_str, sizeof(print_str), "%ld	\n", bank_info[i].perbank_nfree_blks.counter);
		file_write(file, pos,print_str, strlen(print_str));
		pos = pos + strlen(print_str);
	
	
	}

  

	file_close(file);
}

static void write_file_vpages(struct mtdblk_dev *mtdblk)
{
	struct file *file;
	static loff_t pos = 0;
	char test_str[10];
	struct timeval tv;
	char print_str[64];
	int i;
	uint32_t blk,blkcount;
	uint32_t start_blk,end_blk;
	
	
	file = file_open("/home/srimugunthan/ftl_log2", O_WRONLY|O_CREAT, 0644);
	if(file == NULL)
	{
	
		printk(KERN_INFO "dbg file open error");
		return;
	}
	for(i=0,blkcount = 0; i < numpllbanks;i++)
	{
	
		start_blk = mtdblk->cur_writing[i].first_blk;
		end_blk = mtdblk->cur_writing[i].last_blk;
		//printk(KERN_INFO "ER %ld -> %ld ",start_blk,end_blk);
		for(blk = start_blk;blk<= end_blk;blk++)
		{
				if(!(blk_isfree(mtdblk,blk)))
				{
					//snprintf(print_str, sizeof(print_str), "%d %ld	\n",blk_isfree(mtdblk,i), blk_info[i].num_valid_pages.counter);
					snprintf(print_str, sizeof(print_str), "%ld	\n",blk_info[blk].num_valid_pages.counter);
					file_write(file, pos,print_str, strlen(print_str));
					pos = pos + strlen(print_str);
				}
				blkcount++;
		
		}
	
	
	}
	file_close(file);
}

static void write_file_maptab(struct mtdblk_dev *mtdblk)
{
	struct file *file;
	static loff_t pos = 0;
	char test_str[10];
	struct timeval tv;
	char print_str[64];
	int i;
	
	
	file = file_open("/home/srimugunthan/ftl_log3", O_WRONLY|O_CREAT, 0644);
	if(file == NULL)
	{
	
		printk(KERN_INFO "dbg file open error");
		return;
	}
	for(i = 0; i < MAP_TABLE_SIZE;i++)
	{
	//	if(!(blk_isfree(mtdblk,i)))
		{
			snprintf(print_str, sizeof(print_str), "%ld	\n", map_table[i]);
			file_write(file, pos,print_str, strlen(print_str));
			pos = pos + strlen(print_str);
		}
	
	
	}
	file_close(file);
}

void write_log(struct mtdblk_dev * mtdblk)
{
	write_file_vpages(mtdblk);
	write_file_freeblks(mtdblk);
//	write_file_maptab(mtdblk);
}



#if 1
/* generates a mask with onebits set*/
uint64_t generate_rand_mask(int onebits)
{
	uint64_t return_mask = 0ULL;
	unsigned long temp_rand =0;
	uint32_t bitindex;
	int i;
	for(i = 0; i < onebits;i++)
	{
	loop_begin:
		get_random_bytes(&temp_rand, sizeof(temp_rand));
		bitindex = temp_rand%(sizeof(uint64_t));
		if(!(test_bit(bitindex,&return_mask)))
		{
			test_and_set_bit(bitindex,&return_mask);
		}
		else
		{
			goto loop_begin;
		}
					
				
	}
	return return_mask;
}
#endif

int get_normal_incr(int bank_num,struct mtdblk_dev * mtdblk)
{
	
	
	unsigned long temp_rand =0;
	int ret_val = 0;
	
	
	if(bank_num >= 0 && bank_num < numpllbanks/4)
	{
		get_random_bytes(&temp_rand, sizeof(temp_rand));
		ret_val = temp_rand%(mtdblk->blks_per_bank/8);
				
		return ret_val;
	}
	if(bank_num == numpllbanks/4)
	{
		ret_val = (mtdblk->blks_per_bank/8);
		return ret_val;
	}
	if(bank_num > numpllbanks/4 && bank_num < numpllbanks/2)
	{
		get_random_bytes(&temp_rand, sizeof(temp_rand));
		ret_val = (mtdblk->blks_per_bank/8)+ temp_rand%(2*mtdblk->blks_per_bank/8);
				
		return ret_val;
	}
	if(bank_num == numpllbanks/2)
	{
		ret_val = (3*mtdblk->blks_per_bank/8);
		return ret_val;
	}
	if(bank_num > numpllbanks/2 && bank_num < 3*numpllbanks/4)
	{
		get_random_bytes(&temp_rand, sizeof(temp_rand));
		ret_val = (mtdblk->blks_per_bank/8)+ temp_rand%(2*mtdblk->blks_per_bank/8);
				
		return ret_val;
	}
	if(bank_num == 3*numpllbanks/4)
	{
		ret_val = (mtdblk->blks_per_bank/8);
		return ret_val;
	}
	if(bank_num > 3*numpllbanks/4 && bank_num < numpllbanks)
	{
		get_random_bytes(&temp_rand, sizeof(temp_rand));
		ret_val = temp_rand%(mtdblk->blks_per_bank/8);
				
		return ret_val;
				
	}
	printk(KERN_ERR "myftl: error control in none of if = %d %d",bank_num,numpllbanks);
	return ret_val;
}

struct GC_info
{
	uint32_t last_phypage;
};
struct GC_info GC_phase_bankinfo[64];

		

uint32_t getvpage(struct mtdblk_dev *mtdblk ,int bankno)
{
	uint32_t blknum;
	uint32_t phypage;
	uint32_t page_in_blk;
	uint64_t cp_mask;
	int i;
	
	
	phypage = GC_phase_bankinfo[bankno].last_phypage;
	
	page_in_blk = phypage%(mtdblk->pages_per_blk);
	
	
	if(phypage == INVALID_PAGE_NUMBER_32 || page_in_blk == (mtdblk->pages_per_blk-1))
	{
		if(phypage == INVALID_PAGE_NUMBER_32)
		{
			blknum = mtdblk->cur_writing[bankno].first_blk;
			printk(KERN_INFO "starting with B%d",bankno);
		}
		else
		{
			blknum = phypage/(mtdblk->pages_per_blk);
			blknum++;
			
		}
		while(1)
		{
			
			if(blknum > mtdblk->cur_writing[bankno].last_blk)
				return INVALID_PAGE_NUMBER_32;
			if((!blk_isfree(mtdblk,blknum)) && (blk_info[blknum].num_valid_pages.counter > 0))
				break;
			blknum++;
		}
		phypage = blknum*(mtdblk->pages_per_blk);
		page_in_blk = phypage%(mtdblk->pages_per_blk);
		
		
		/* non zero return : set*/
		if((!test_bit(page_in_blk, blk_info[blknum].valid_pages_map)))
		{
			bitmap_copy(&cp_mask,blk_info[blknum].valid_pages_map,64);
			printk(KERN_INFO "should be 1 blk = %d page%d map %llx vpage = %d",blknum,page_in_blk,cp_mask,
			blk_info[blknum].num_valid_pages.counter);
			BUG();
					
		}
			
		GC_phase_bankinfo[bankno].last_phypage = phypage;
		return phypage;
		
	}
	
	phypage++;
	page_in_blk = phypage%(mtdblk->pages_per_blk);
	blknum = phypage/(mtdblk->pages_per_blk);
	if(test_bit(page_in_blk, blk_info[blknum].valid_pages_map) == 0)
	{
		blknum++;
		while(1)
		{
			
			if(blknum > mtdblk->cur_writing[bankno].last_blk)
				return INVALID_PAGE_NUMBER_32;
			if((!blk_isfree(mtdblk,blknum)) && (blk_info[blknum].num_valid_pages.counter > 0))
				break;
			blknum++;
		}
		//printk(KERN_INFO " B%d blk",bankno,blknum);
		
		phypage = blknum*(mtdblk->pages_per_blk);
		page_in_blk = phypage%(mtdblk->pages_per_blk);
		
		/* non zero return : set*/
		if((!test_bit(page_in_blk, blk_info[blknum].valid_pages_map)))
		{
			bitmap_copy(&cp_mask,blk_info[blknum].valid_pages_map,64);
			printk(KERN_INFO "should be 1 blk = %d page%d map %llx vpage = %d",blknum,page_in_blk,cp_mask,
			blk_info[blknum].num_valid_pages.counter);
			BUG();
					
		}
		GC_phase_bankinfo[bankno].last_phypage = phypage;
		return phypage;

	}
	else
	{
		GC_phase_bankinfo[bankno].last_phypage = phypage;
		return phypage;
	}

}



void modify_accesslist(struct mymtd_blktrans_dev *dev,unsigned long arg)
{
#ifdef PREFETCH_ACL	
	
	struct mtdblk_dev *mtdblk = container_of(dev, struct mtdblk_dev, mbd);
	uint32_t *acc_list;
	int aclindex;
	int i;
	acc_list = (uint32_t*)arg;
	aclindex  = mtdblk->acc_listindex;
	
	printk(KERN_INFO "recvd accesslist: %x",arg);
	for(i = 0;acc_list[i] != INVALID_PAGE_NUMBER_32;i++)
	{
		mtdblk->accesslist[aclindex] = acc_list[i];
		printk(KERN_INFO "[%ld] =  %ld", aclindex, mtdblk->accesslist[aclindex]);
		aclindex = (aclindex+1)%MAX_ACCESS_LIST;
		
	}
	mtdblk->acc_listindex = aclindex;
	for(i = 0; i < MAX_PREF_THREAD;i++)
		wake_up_process(mtdblk->ftl_prefetch_thrd[i]);
#endif
}
#if 0
/* 32 64 128 256 */
int synth_num_free_blks[8] = {
								28,
								56,
								90,
								100,
								150,
								85,
								60,
								30
							};
							
#endif

/* 8 16 32 64 */
int synth_num_free_blks[8] = {
								20,
								24,
								25,
								34,
								35,
								25,
								24,
								15
							};
							
void modify_ds_for_gc(struct mymtd_blktrans_dev *dev)
{
	struct mtdblk_dev *mtdblk = container_of(dev, struct mtdblk_dev, mbd);
	struct mtd_info *mtd = mtdblk->mbd.mtd;
	uint32_t lower_limit;
	uint32_t highest_limit;
	uint32_t start_with;
	uint32_t num_free_blks;
	int i;
	uint32_t start_blk,end_blk;
	uint32_t minvpages,vpages,dirty_pages;
	uint32_t num_occupied_blks;
	uint32_t blk,blk_count;
	unsigned long temp_rand =0;
	uint64_t mask;
	uint32_t lpn_count;
	uint32_t phy_page;
	int j;
	uint32_t total_occupiedblks = 0;
	int optional_incr;
	gc_testing_on =1;
	lower_limit = mtdblk->blks_per_bank/8;
	highest_limit = mtdblk->blks_per_bank/2;
	start_with = lower_limit-1;
	
	lpn_count = 0;
	for(j = 0; j < MAP_TABLE_SIZE ;j++)
	{
		reverse_map_tab[j] = INVALID_PAGE_NUMBER;
		map_table[j] = INVALID_PAGE_NUMBER;
	}
	
	for(i=0; i < numpllbanks;i++)
	{
		
		start_blk = mtdblk->cur_writing[i].first_blk;
		end_blk = mtdblk->cur_writing[i].last_blk;
		
		atomic_set(&bank_info[i].perbank_nfree_blks,mtdblk->blks_per_bank);
		/*decide num free blks for this bank;*/
		
#ifdef EIGHT_BANK_FLASH
		num_free_blks = synth_num_free_blks[i];
#else		
		num_free_blks = start_with + get_normal_incr(i,mtdblk);
#endif



		if(num_free_blks < mtdblk->blks_per_bank/8)
			minvpages = mtdblk->gc_thresh[2];
		else if(num_free_blks < mtdblk->blks_per_bank/4)
			minvpages = mtdblk->gc_thresh[1];
		else if(num_free_blks < mtdblk->blks_per_bank/2)
			minvpages = mtdblk->gc_thresh[0];
		
		
		
		
		num_occupied_blks = mtdblk->blks_per_bank-(num_free_blks+mtdblk->reserved_blks_per_bank);
		total_occupiedblks += num_occupied_blks;
		
		
		/*
		 *  make num_occupied blocks  as occupied
		 *  with a few blocks with <= min_vpages
		 *  and a few blocks valid
		 */
		for(blk = start_blk,blk_count = 0; blk_count < num_occupied_blks; blk++,blk_count++)
		{
			
			
			blk_unfree(mtdblk,blk);
			phy_page = blk*mtdblk->pages_per_blk;
			
			get_random_bytes(&temp_rand, sizeof(temp_rand));
			/*15% is randomly dirty; 40% has less than minvpages 55% is full*/
			if(temp_rand%100 < 15)
			{
				vpages = temp_rand%mtdblk->pages_per_blk;
				
			}
			else if(temp_rand%100 < 55)
			{
				if(minvpages != 0)
				{
					vpages = temp_rand%minvpages;
				}
				else
				{
					vpages = 0;
				}
			}
			else
			{
				vpages = mtdblk->pages_per_blk;
			}
			
			dirty_pages = mtdblk->pages_per_blk - vpages;
			mask = 0xFFFFFFFFFFFFFFFF;
			//mask = generate_rand_mask(vpages);
			mask = mask >>dirty_pages;		
			//printk(KERN_INFO "blk = %d dirty_pages=%d mask = %llx",blk,dirty_pages,mask);
			
			for(j = 0; j < vpages;j++)
			{
				//map_table[lpn_count] = phy_page+j;
				//reverse_map_tab[phy_page+j] = lpn_count; 
				lpn_count++;
				
			}
			
			
			bitmap_copy(blk_info[blk].valid_pages_map,&mask,64);
			atomic_set(&blk_info[blk].num_valid_pages,vpages);
			atomic_dec(&bank_info[i].perbank_nfree_blks);
			
			atomic_add(dirty_pages,&bank_info[i].perbank_ndirty_pages);
			
			
			
		}
		
		
		GC_phase_bankinfo[i].last_phypage = INVALID_PAGE_NUMBER_32;

	}
	
	uint32_t lpn;
	uint32_t bankno;
	bankno = 0;
	for(lpn=0;lpn < lpn_count;lpn++)
	{
		
		phy_page = INVALID_PAGE_NUMBER_32;
		
		
		
		while(phy_page == INVALID_PAGE_NUMBER_32)
		{
			bankno = (bankno+1)%numpllbanks;
			
			phy_page = getvpage(mtdblk,bankno);
			
		}
		if(phy_page <0 || phy_page >= MAP_TABLE_SIZE)
		{
			printk(KERN_INFO "phy_page %u wrong",phy_page);
			BUG();
					
		}
		
		if(lpn <0 || lpn >= MAP_TABLE_SIZE)
		{
			
			printk(KERN_INFO "lpn %u wrong",lpn);
			BUG();
					
			
		}
		
				 
		map_table[lpn] = phy_page;
		reverse_map_tab[phy_page] = lpn;
		
	}
#if 0
	for(i=0; i < numpllbanks;i++)
	{
		
		start_blk = mtdblk->cur_writing[i].first_blk;
		end_blk = mtdblk->cur_writing[i].last_blk;
		printk(KERN_INFO "ER %ul -> %ul ",start_blk,end_blk);
		for(blk = start_blk;blk<= end_blk;blk++)
		{
			if(!is_block_bad(mtdblk,blk))
				sync_erase(mtdblk,blk);
		}
	}
#endif
	printk(KERN_INFO "num blks occupied = %ul %ul %ul",total_occupiedblks,mtdblk->num_blks,mtdblk->blks_per_bank);
	printk(KERN_INFO "num pages occupied = %ul %ul",lpn_count,mtdblk->num_blks*mtdblk->pages_per_blk);
	printk(KERN_INFO "logic blks occupied0->%ul",lpn_count/mtdblk->pages_per_blk);

	
}

static struct mtd_blktrans_ops mtdblock_tr = {
	.name		= "mtdblock",
	.major		= 31,
 	.part_bits	= 0,
	.blksize 	= 4096,
	.open		= mtdblock_open,
	.flush		= mtdblock_flush,
	.release	= mtdblock_release,
	.readsect	= mtdblock_readsect,
	.get_blkinfo	= pr_bank_info,
 	.prepare_for_gctest = modify_ds_for_gc,
	.bankinfo_filewr = write_log,
	.convey_pfetch_list=modify_accesslist,
	.writesect	= mtdblock_writesect,
 
	.add_mtd	= mtdblock_add_mtd,
	.remove_dev	= mtdblock_remove_dev,
	.owner		= THIS_MODULE,
};

static int __init init_mtdblock(void)
{
	mutex_init(&mtdblks_lock);

	return register_mymtd_blktrans(&mtdblock_tr);
}

void mymtd_blktrans_exit(void);

static void __exit cleanup_mtdblock(void)
{
	deregister_mymtd_blktrans(&mtdblock_tr);
	mymtd_blktrans_exit();
	
}

module_init(init_mtdblock);
module_exit(cleanup_mtdblock);


//MODULE_LICENSE("GPL");
//MODULE_AUTHOR("Srimugunthan");
//MODULE_DESCRIPTION("Caching FTL");
