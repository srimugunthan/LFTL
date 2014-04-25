#ifndef MY_BLKTRANS_H
#define MY_BLKTRANS_H
/*==*/
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/mtd/mtd.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/random.h>

#include <linux/mutex.h>
#include <linux/kref.h>
#include <linux/sysfs.h>
#include <linux/mempool.h>
#include <asm/delay.h> 
#include <linux/syscalls.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fcntl.h>
#include <asm/uaccess.h>
#include <linux/fs.h>
#include <asm/segment.h>
#include <asm/uaccess.h>
#include <linux/buffer_head.h>
#include <linux/time.h>
#include <asm/msr.h>
#include <linux/timex.h>
#include <asm/timex.h> 

struct hd_geometry;
struct mtd_info;
struct mtd_blktrans_ops;
struct file;
struct inode;

#define VIRGO_NUM_MAX_REQ_Q 16

//#define  ADAPTIVE_GC 1

struct thread_arg_data
{
	int qno;
	struct mymtd_blktrans_dev *dev;
};


struct my_bio_list
{
	struct list_head qelem_ptr;
	struct bio *bio;
};

struct list_lru {
	spinlock_t              lock;
	struct list_head        list;
	long                    nr_items;
};

struct cache_buf_list
{
	struct list_head list;
	int value;
};


int list_lru_init(struct list_lru *lru);
int list_lru_add(struct list_lru *lru, struct list_head *item);
void list_lru_touch(struct list_lru *lru,  struct list_head *item);
struct cache_buf_list *list_lru_deqhd(struct list_lru *lru);

struct cache_buf_list *list_lru_del(struct list_lru *lru,   struct list_head *item);

struct mymtd_blktrans_dev {
	struct mtd_blktrans_ops *tr;
	struct list_head list;
	struct mtd_info *mtd;
	struct mutex lock;
	int devnum;
	unsigned long size;
	int readonly;
	int open;
	struct kref ref;
	struct gendisk *disk;
	struct attribute_group *disk_attributes;
	struct task_struct *thread[VIRGO_NUM_MAX_REQ_Q];
	struct request_queue *rq;
	spinlock_t queue_lock;
	void *priv;
	
	/* my changes*/

	spinlock_t  mybioq_lock[VIRGO_NUM_MAX_REQ_Q];;
	struct my_bio_list qu[VIRGO_NUM_MAX_REQ_Q];
	struct thread_arg_data thrd_arg[VIRGO_NUM_MAX_REQ_Q];

	DECLARE_BITMAP(active_iokthread,64);

};

struct mtd_blktrans_ops {
	char *name;
	int major;
	int part_bits;
	int blksize;
	int blkshift;

	/* Access functions */
	int (*readsect)(struct mymtd_blktrans_dev *dev,
	      unsigned long block, char *buffer);
	int (*writesect)(struct mymtd_blktrans_dev *dev,
	      unsigned long block, char *buffer);
	int (*discard)(struct mymtd_blktrans_dev *dev,
	      unsigned long block, unsigned nr_blocks);

	/* Block layer ioctls */
	int (*getgeo)(struct mymtd_blktrans_dev *dev, struct hd_geometry *geo);
	int (*flush)(struct mymtd_blktrans_dev *dev);
	int (*get_blkinfo)(struct mymtd_blktrans_dev *dev);
	void (*prepare_for_gctest)(struct mymtd_blktrans_dev *dev);
	void (*bankinfo_filewr)(struct mymtd_blktrans_dev *dev);
	void (*convey_pfetch_list)(struct mymtd_blktrans_dev *dev,unsigned long arg);

	/* Called with mtd_table_mutex held; no race with add/remove */
	int (*open)(struct mymtd_blktrans_dev *dev);
	int (*release)(struct mymtd_blktrans_dev *dev);

	/* Called on {de,}registration and on subsequent addition/removal
	of devices, with mtd_table_mutex held. */
	void (*add_mtd)(struct mtd_blktrans_ops *tr, struct mtd_info *mtd);
	void (*remove_dev)(struct mymtd_blktrans_dev *dev);

	struct list_head devs;
	struct list_head list;
	struct module *owner;
};



int add_mymtd_blktrans_dev(struct mymtd_blktrans_dev *new);
int deregister_mymtd_blktrans(struct mtd_blktrans_ops *tr);
int register_mymtd_blktrans(struct mtd_blktrans_ops *tr);
int del_mymtd_blktrans_dev(struct mymtd_blktrans_dev *old);

/****/
#endif

