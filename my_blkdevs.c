/*
 * Interface to Linux block layer for MTD 'translation layers'.
 *
 * Copyright © 2003-2010 David Woodhouse <dwmw2@infradead.org>
 * Modified by Srimugunthan Dhandapani srimugunthan.dhandapani@gmail.com
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

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/fs.h>

#include <linux/mtd/mtd.h>
#include <linux/blkdev.h>
#include <linux/blkpg.h>
#include <linux/spinlock.h>
#include <linux/hdreg.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <asm/uaccess.h>
#include <linux/random.h>
#include "my_blktrans.h"
#include "lfq.h"


#define NORMAL_Q 1

extern struct mutex mtd_table_mutex;
extern struct mtd_info *__mtd_next_device(int i);

#define mtd_for_each_device(mtd)			\
	for ((mtd) = __mtd_next_device(0);		\
	     (mtd) != NULL;				\
	     (mtd) = __mtd_next_device(mtd->index + 1))


static LIST_HEAD(blktrans_majors);
static DEFINE_MUTEX(blktrans_ref_mutex);


static struct kmem_cache *mybiolist_cachep;
static mempool_t *biolistpool;
static struct lfq_queue_rcu rcuqu[VIRGO_NUM_MAX_REQ_Q];
static uint32_t last_lpn[VIRGO_NUM_MAX_REQ_Q];

struct bio_node {
	struct lfq_node_rcu list;
	struct rcu_head rcu;
	struct bio *bio;
};

void blktrans_dev_release(struct kref *kref)
{
	struct mymtd_blktrans_dev *dev =
		container_of(kref, struct mymtd_blktrans_dev, ref);

	dev->disk->private_data = NULL;
	blk_cleanup_queue(dev->rq);
	put_disk(dev->disk);
	list_del(&dev->list);
	kfree(dev);
}

static struct mymtd_blktrans_dev *blktrans_dev_get(struct gendisk *disk)
{
	struct mymtd_blktrans_dev *dev;

	mutex_lock(&blktrans_ref_mutex);
	dev = disk->private_data;

	if (!dev)
		goto unlock;
	kref_get(&dev->ref);
unlock:
	mutex_unlock(&blktrans_ref_mutex);
	return dev;
}

void blktrans_dev_put(struct mymtd_blktrans_dev *dev)
{
	mutex_lock(&blktrans_ref_mutex);
	kref_put(&dev->ref, blktrans_dev_release);
	mutex_unlock(&blktrans_ref_mutex);
}


static int do_blktrans_request(struct mtd_blktrans_ops *tr,
			       struct mymtd_blktrans_dev *dev,
			       struct request *req)
{
	unsigned long block, nsect;
	char *buf;

	block = blk_rq_pos(req) << 9 >> tr->blkshift;
	nsect = blk_rq_cur_bytes(req) >> tr->blkshift;

	buf = req->buffer;

	if (req->cmd_type != REQ_TYPE_FS)
		return -EIO;

	if (blk_rq_pos(req) + blk_rq_cur_sectors(req) >
	    get_capacity(req->rq_disk))
		return -EIO;

	if (req->cmd_flags & REQ_DISCARD)
		return tr->discard(dev, block, nsect);

	switch(rq_data_dir(req)) {
	case READ:
		for (; nsect > 0; nsect--, block++, buf += tr->blksize)
			if (tr->readsect(dev, block, buf))
				return -EIO;
		rq_flush_dcache_pages(req);
		return 0;
	case WRITE:
		if (!tr->writesect)
			return -EIO;

		rq_flush_dcache_pages(req);
		for (; nsect > 0; nsect--, block++, buf += tr->blksize)
			if (tr->writesect(dev, block, buf))
				return -EIO;
		return 0;
	default:
		printk(KERN_NOTICE "Unknown request %u\n", rq_data_dir(req));
		return -EIO;
	}
}

/*
 * If your request handling is slow, then it is often better to push
 * request handling to a dedicated thread. That may be the case if your
 * device can't do DMA for instance, and you have to transfer all the data
 * manually. Pushing that to a thread usually makes it easy to do that with
 * interrupts enabled and is thus nicer to the rest of the system.
 */
#if 0
static int mtd_blktrans_thread(void *arg)
{
	struct mymtd_blktrans_dev *dev = arg;
	struct request_queue *rq = dev->rq;
	struct request *req = NULL;

	spin_lock_irq(rq->queue_lock);

	while (!kthread_should_stop()) {
		int res;

		if (!req && !(req = blk_fetch_request(rq))) {
			set_current_state(TASK_INTERRUPTIBLE);

			if (kthread_should_stop())
				set_current_state(TASK_RUNNING);

			spin_unlock_irq(rq->queue_lock);
			schedule();
			spin_lock_irq(rq->queue_lock);
			continue;
		}

		spin_unlock_irq(rq->queue_lock);

		mutex_lock(&dev->lock);
		res = do_blktrans_request(dev->tr, dev, req);
		mutex_unlock(&dev->lock);

		spin_lock_irq(rq->queue_lock);

		if (!__blk_end_request_cur(req, res))
			req = NULL;
	}

	if (req)
		__blk_end_request_all(req, -EIO);

	spin_unlock_irq(rq->queue_lock);

	return 0;
}



#endif
#if 0
#define bio_iovec_idx(bio, idx)	(&((bio)->bi_io_vec[(idx)]))
#define bio_iovec(bio)		bio_iovec_idx((bio), (bio)->bi_idx)
#define bio_page(bio)		bio_iovec((bio))->bv_page
#define bio_offset(bio)		bio_iovec((bio))->bv_offset
#define bio_segments(bio)	((bio)->bi_vcnt - (bio)->bi_idx)
#define bio_sectors(bio)	((bio)->bi_size >> 9)

static inline void *bio_data(struct bio *bio)
{
	if (bio->bi_vcnt)
		return page_address(bio_page(bio)) + bio_offset(bio);

	return NULL;
}
#endif
#if 1
/* oct end code. correctly working with xfs*/
static int mtd_blktrans_thread(void *arg)
{
	struct mymtd_blktrans_dev *dev = arg;
	struct request_queue *rq = dev->rq;
	struct request *req = NULL;
	int sectors_xferred;
	struct bio_vec *bvec;
	struct bio *bio;
	int i;
	int res=0;
	unsigned long block, nsect;
	char *buf;
	int sects_to_transfer;
	char *wrong_buf;
	int num_sects_in_page;
	
	spin_lock_irq(rq->queue_lock);

	while (!kthread_should_stop()) {


		if (!req && !(req = blk_fetch_request(rq))) {
			set_current_state(TASK_INTERRUPTIBLE);

			if (kthread_should_stop())
				set_current_state(TASK_RUNNING);

			spin_unlock_irq(rq->queue_lock);
			schedule();
			spin_lock_irq(rq->queue_lock);
			continue;
	}

	spin_unlock_irq(rq->queue_lock);

	mutex_lock(&dev->lock);
		
	//res = do_blktrans_request(dev->tr, dev, req);
		
#if 1	

	sectors_xferred = 0;
	if (req->cmd_type != REQ_TYPE_FS)
	{
		res =  -EIO;
		printk(KERN_INFO "FTL ERROR: REQ_TYPE is not FS");
		goto after_req;
	}

	if (blk_rq_pos(req) + blk_rq_cur_sectors(req) >
	get_capacity(req->rq_disk))
	{
		res = -EIO;
		printk(KERN_INFO "FTL ERROR: REQ > capacity");
		goto after_req;
	}

		
	if (req->cmd_flags & REQ_DISCARD)
	{
		res = dev->tr->discard(dev, block, nsect);
		printk(KERN_INFO "FTL ERROR: REQ is discard");
		goto after_req;
	}
	
		
	//__rq_for_each_bio(bio, req)
	for (bio = (req)->bio; bio; bio = bio->bi_next)
	{
			
		
		/* Do each segment independently. */
		//bvec = ((&((bio)->bi_io_vec[((bio)->bi_idx)])));
		block = (bio->bi_sector << 9) >> dev->tr->blkshift;
		
		
		
		printk(KERN_INFO "vcnt  = %d ",bio->bi_vcnt);
		/*
		if (bio->bi_vcnt)
		{
		wrong_buf = page_address(((&((bio)->bi_io_vec[((bio)->bi_idx)])))->bv_page) + (((&((bio)->bi_io_vec[((bio)->bi_idx)])))->bv_offset);
		}
		(or)
		*/
		buf = bio_data(bio);
		
		bio_for_each_segment(bvec, bio, i)
		{

		buf = page_address(bvec->bv_page) + bvec->bv_offset;
		nsect = bvec->bv_len >> dev->tr->blkshift;
		sects_to_transfer = nsect;


		num_sects_in_page = PAGE_SIZE/(dev->tr->blksize);
		
		
		printk(KERN_INFO "block = %d nsect = %d buf = %u",block,nsect,buf); 
		
				
		switch(rq_data_dir(req)) {
			case READ:
				for (; ((nsect >0)); nsect--, block++, buf += dev->tr->blksize){
				if (dev->tr->readsect(dev, block, buf)){						
				res =  -EIO;
				goto after_req;
				}
				//num_sects_in_page--;
			}
			rq_flush_dcache_pages(req);
			break;
			case WRITE:
			if (!dev->tr->writesect){
		
			res =  -EIO;
			goto after_req;
			}
		
			rq_flush_dcache_pages(req);
			for (; ( (nsect >0)); nsect--, block++, buf += dev->tr->blksize){
				if (dev->tr->writesect(dev, block, buf)){
				res =  -EIO;
				goto after_req;
				}
				//num_sects_in_page--;
			}
			break;
			default:
				printk(KERN_NOTICE "Unknown request %u\n", rq_data_dir(req));
				res =  -EIO;
			}
			sectors_xferred += (sects_to_transfer-nsect);
				
		}
			
			
	}
#endif
after_req:
		mutex_unlock(&dev->lock);

		
		spin_lock_irq(rq->queue_lock);
		
		if (!__blk_end_request_cur(req, res))
			req = NULL;

		//if(!__blk_end_request(req, res, sectors_xferred*512))
		//req = NULL;
		printk(KERN_INFO "sectstrfd = %d res = %d req = %u ",sectors_xferred,res,req);
	}

	if (req)
	__blk_end_request_all(req, -EIO);

	spin_unlock_irq(rq->queue_lock);

	return 0;
}
#endif
	
#if 0
/* working thread code for now*/
static int mtd_blktrans_thread(void *arg)
{
	struct mymtd_blktrans_dev *dev = arg;
	struct request_queue *rq = dev->rq;
	struct request *req = NULL;
	int sectors_xferred;
	struct bio_vec *bvec;
	struct bio *bio;
	int i;
	int res=0;
	unsigned long block, nsect;
	char *buf;
	int sects_to_transfer;
	int print_sect_txfer;
	
	spin_lock_irq(rq->queue_lock);

	while (!kthread_should_stop()) {


		if (!req && !(req = blk_fetch_request(rq))) {
			set_current_state(TASK_INTERRUPTIBLE);

			if (kthread_should_stop())
				set_current_state(TASK_RUNNING);

			spin_unlock_irq(rq->queue_lock);
			schedule();
			spin_lock_irq(rq->queue_lock);
			continue;
		}

		spin_unlock_irq(rq->queue_lock);

		//mutex_lock(&dev->lock);
		
		//res = do_blktrans_request(dev->tr, dev, req);
		
		

		sectors_xferred = 0;
		if (req->cmd_type != REQ_TYPE_FS)
		{
			res =  -EIO;
			printk(KERN_INFO "FTL ERROR: REQ_TYPE is not FS");
			goto after_req;
		}

		if (blk_rq_pos(req) + blk_rq_cur_sectors(req) >
				  get_capacity(req->rq_disk))
		{
			res = -EIO;
			printk(KERN_INFO "FTL ERROR: REQ > capacity");
			goto after_req;
		}

		
		if (req->cmd_flags & REQ_DISCARD)
		{
			res = dev->tr->discard(dev, block, nsect);
			printk(KERN_INFO "FTL ERROR: REQ is discard");
			goto after_req;
		}
		printk(KERN_INFO "FTL: new req");
		
		__rq_for_each_bio(bio, req) {
			
			printk(KERN_INFO "for each bio"); 
			/* Do each segment independently. */
			bio_for_each_segment(bvec, bio, i) {
				
				block = (bio->bi_sector << 9) >> dev->tr->blkshift;
				nsect = bio_iovec(bio)->bv_len >> dev->tr->blkshift;
				sects_to_transfer = nsect;
				buf = 	page_address(bvec->bv_page) + bvec->bv_offset;
				printk(KERN_INFO "sect = %d nsect = %d buf = %u",block,nsect,buf); 
				
				
				switch(rq_data_dir(req)) {
					case READ:
						for (; nsect > 0; nsect--, block++, buf += dev->tr->blksize){
							if (dev->tr->readsect(dev, block, buf)){						
								res =  -EIO;
								goto after_req;
							}
						}
						rq_flush_dcache_pages(req);
						break;
					case WRITE:
						if (!dev->tr->writesect){
							
							res =  -EIO;
							goto after_req;
						}
		
						rq_flush_dcache_pages(req);
						for (; nsect > 0; nsect--, block++, buf += dev->tr->blksize){
							if (dev->tr->writesect(dev, block, buf)){
								res =  -EIO;
								goto after_req;
							}
						}
						break;
					default:
						printk(KERN_NOTICE "Unknown request %u\n", rq_data_dir(req));
						res =  -EIO;
				}
				sectors_xferred += (sects_to_transfer-nsect);
				
			}
			
			
#if 0
			spin_lock_irq(rq->queue_lock);
			//blk_update_request(req,res, sectors_xferred*512);
			//sectors_xferred = 0;
			
			print_sect_txfer = sectors_xferred;
			spin_unlock_irq(rq->queue_lock);
#endif			
	

			
		}
		after_req:
		//mutex_unlock(&dev->lock);

#if 0
		spin_lock_irq(rq->queue_lock);
		

	
		if (!blk_update_request(req, res, sectors_xferred*512))
		{
			blk_finish_request(req,res);	
			req = NULL;
		}
#endif
		
		spin_lock_irq(rq->queue_lock);

		if (!__blk_end_request_cur(req, res))
			req = NULL;
				
		printk(KERN_INFO "sectstrfd = %d res = %d req = %u ",print_sect_txfer,res,req);
	
	}

	if (req)
		__blk_end_request_all(req, -EIO);

	spin_unlock_irq(rq->queue_lock);

	return 0;
}
#endif
#if 0	
static  unsigned int mybio_cur_bytes(struct bio *bio)
{
	if (bio->bi_vcnt)
		return bio_iovec(bio)->bv_len;
	else /* dataless requests such as discard */
		return bio->bi_size;
}

static int  myblk_rq_cur_bytes(const struct request *rq)
{
	return rq->bio ? bio_cur_bytes(rq->bio) : 0;
}
			 
bool myblk_update_request(struct request *req, int error, unsigned int nr_bytes)
{
	int total_bytes, bio_nbytes, next_idx = 0;
	struct bio *bio;

	if (!req->bio)
		return false;

//	trace_block_rq_complete(req->q, req);

	/*
	* For fs requests, rq is just carrier of independent bio's
	* and each partial completion should be handled separately.
	* Reset per-request error on each partial completion.
	*
	* TODO: tj: This is too subtle.  It would be better to let
	* low level drivers do what they see fit.
	*/
	if (req->cmd_type == REQ_TYPE_FS)
		req->errors = 0;

	if (error && req->cmd_type == REQ_TYPE_FS &&
		   !(req->cmd_flags & REQ_QUIET)) {
		printk(KERN_ERR "end_request: I/O error, dev %s, sector %llu\n",
		       req->rq_disk ? req->rq_disk->disk_name : "?",
	 (unsigned long long)blk_rq_pos(req));
		   }

		   blk_account_io_completion(req, nr_bytes);

		   total_bytes = bio_nbytes = 0;
		   while ((bio = req->bio) != NULL) {
			   int nbytes;

			   if (nr_bytes >= bio->bi_size) {
				   req->bio = bio->bi_next;
				   nbytes = bio->bi_size;
				   req_bio_endio(req, bio, nbytes, error);
				   next_idx = 0;
				   bio_nbytes = 0;
			   } else {
				   int idx = bio->bi_idx + next_idx;

				   if (unlikely(idx >= bio->bi_vcnt)) {
					   blk_dump_rq_flags(req, "__end_that");
					   	   printk(KERN_ERR "%s: bio idx %d >= vcnt %d\n",
							   __func__, idx, bio->bi_vcnt);
					   break;
				   }

				   nbytes = bio_iovec_idx(bio, idx)->bv_len;
				   BIO_BUG_ON(nbytes > bio->bi_size);

			/*
				   * not a complete bvec done
			*/
				   if (unlikely(nbytes > nr_bytes)) {
					   bio_nbytes += nr_bytes;
					   total_bytes += nr_bytes;
					   break;
				   }

			/*
				   req			   * advance to the next vector
			*/
				   next_idx++;
				   bio_nbytes += nbytes;
			   }

			   total_bytes += nbytes;
			   nr_bytes -= nbytes;

			   bio = req->bio;
			   if (bio) {
			/*
				   * end more in this run, or just return 'not-done'
			*/
				   if (unlikely(nr_bytes <= 0))
					   break;
			   }
		   }

			/*
		   * completely done
			*/
		   if (!req->bio) {
		/*
			   * Reset counters so that the request stacking driver
			   * can find how many bytes remain in the request
			   * later.
		*/
			   req->__data_len = 0;
			   return false;
		   }

	/*
		   * if the request wasn't completed, update state
	*/
		   if (bio_nbytes) {
			   req_bio_endio(req, bio, bio_nbytes, error);
			   bio->bi_idx += next_idx;
			   bio_iovec(bio)->bv_offset += nr_bytes;
			   bio_iovec(bio)->bv_len -= nr_bytes;
		   }

		   req->__data_len -= total_bytes;
		   req->buffer = bio_data(req->bio);

		   /* update sector only for requests with clear definition of sector */
		   if (req->cmd_type == REQ_TYPE_FS || (req->cmd_flags & REQ_DISCARD))
			   req->__sector += total_bytes >> 9;

		   /* mixed attributes always follow the first bio */
		   if (req->cmd_flags & REQ_MIXED_MERGE) {
			   req->cmd_flags &= ~REQ_FAILFAST_MASK;
			   req->cmd_flags |= req->bio->bi_rw & REQ_FAILFAST_MASK;
		   }

	/*
		   * If total number of sectors is less than the first segment
		   * size, something has gone terribly wrong.
	*/
		   if (req->__data_len < myblk_rq_cur_bytes(req)) {
			   printk(KERN_ERR "blk: request botched\n");
			   req->__data_len = blk_rq_cur_bytes(req);
		   }

		   /* recalculate the number of segments */
		   blk_recalc_rq_segments(req);

		   return true;
}

void myblk_finish_request(struct request *req, int error)
{
#if 0
	if (blk_rq_tagged(req)){
		printk(KERN_INFO "req tagged");
		blk_queue_end_tag(req->q, req);
	}
#endif
	
	BUG_ON((!list_empty(&(req)->queuelist)));

	list_del_init(&req->timeout_list);
	

	if (req->cmd_flags & REQ_DONTPREP){
		printk(KERN_INFO "req REQ_DONTPREP");
		blk_unprep_request(req);
	}

	//blk_account_io_done(req);

	if (req->end_io){
		printk(KERN_INFO "req registered endio");
		req->end_io(req, error);
	}
	else {
		if (((req)->next_rq != NULL)){
			printk(KERN_INFO "req req->nextrq");
			__blk_put_request(req->next_rq->q, req->next_rq);
		}

		__blk_put_request(req->q, req);
	}
}
#endif


#if 0
/* direct bio processing with a req processing thread */
static int mtd_blktrans_thread(void *arg)
{
	struct mymtd_blktrans_dev *dev = arg;
	struct request_queue *rq = dev->rq;
	struct request *req = NULL;
	int sectors_xferred;
	struct bio_vec *bvec;
	struct bio *bio;
	int i;
	int res=0;
	unsigned long block, nsect;
	char *buf;
	int sects_to_transfer;
	int print_sect_txfer;
	
	spin_lock_irq(rq->queue_lock);

	while (!kthread_should_stop()) {


		if (!req && !(req = blk_fetch_request(rq))) {
			set_current_state(TASK_INTERRUPTIBLE);

			if (kthread_should_stop())
				set_current_state(TASK_RUNNING);

			spin_unlock_irq(rq->queue_lock);
			schedule();
			spin_lock_irq(rq->queue_lock);
			continue;
		}

		spin_unlock_irq(rq->queue_lock);

		//mutex_lock(&dev->lock);
		
		//res = do_blktrans_request(dev->tr, dev, req);
		
		

		sectors_xferred = 0;
		if (req->cmd_type != REQ_TYPE_FS)
		{
			res =  -EIO;
			printk(KERN_INFO "FTL ERROR: REQ_TYPE is not FS");
			goto after_req;
		}

		if (blk_rq_pos(req) + blk_rq_cur_sectors(req) >
				  get_capacity(req->rq_disk))
		{
			res = -EIO;
			printk(KERN_INFO "FTL ERROR: REQ > capacity");
			goto after_req;
		}

		
		if (req->cmd_flags & REQ_DISCARD)
		{
			res = dev->tr->discard(dev, block, nsect);
			printk(KERN_INFO "FTL ERROR: REQ is discard");
			goto after_req;
		}
		printk(KERN_INFO "FTL: new req");
		
		__rq_for_each_bio(bio, req) {
			
			printk(KERN_INFO "for each bio"); 
			/* Do each segment independently. */
			bio_for_each_segment(bvec, bio, i) {
				
				block = bio->bi_sector << 9 >> dev->tr->blkshift;
				nsect = bio_iovec(bio)->bv_len >> dev->tr->blkshift;
				sects_to_transfer = nsect;
				buf = 	page_address(bvec->bv_page) + bvec->bv_offset;
				printk(KERN_INFO "block = %d nsect = %d buf = %u",block,nsect,buf); 
				
				
				switch(rq_data_dir(req)) {
					case READ:
						for (; nsect > 0; nsect--, block++, buf += dev->tr->blksize){
							if (dev->tr->readsect(dev, block, buf)){						
								res =  -EIO;
								goto after_req;
							}
						}
						rq_flush_dcache_pages(req);
						break;
					case WRITE:
						if (!dev->tr->writesect){
							
							res =  -EIO;
							goto after_req;
						}
		
						rq_flush_dcache_pages(req);
						for (; nsect > 0; nsect--, block++, buf += dev->tr->blksize){
							if (dev->tr->writesect(dev, block, buf)){
								res =  -EIO;
								goto after_req;
							}
						}
						break;
					default:
						printk(KERN_NOTICE "Unknown request %u\n", rq_data_dir(req));
						res =  -EIO;
				}
				sectors_xferred += (sects_to_transfer-nsect);
				
			}
			
			
#if 0	
			bio_endio(bio, res);
#else
			spin_lock_irq(rq->queue_lock);
			//blk_update_request(req,res, sectors_xferred*512);
			//sectors_xferred = 0;
			
			print_sect_txfer = sectors_xferred;
			spin_unlock_irq(rq->queue_lock);
				
	
#endif
			
		}
		after_req:
		//mutex_unlock(&dev->lock);

		spin_lock_irq(rq->queue_lock);
		
#if 0	
		bio_endio(bio, res);
		if (!myblk_update_request(req, res, sectors_xferred*512))
		{
			//list_del_init(&req->timeout_list);
			//elv_completed_request(req->q, req);
		__blk_put_request(req->q, req);
			//myblk_finish_request(req,res);	
		req = NULL;
	}
#else
	
	

	
		
	if(!__blk_end_request(req, res, sectors_xferred*512))
		req = NULL;
			
	printk(KERN_INFO "sectstrfd = %d res = %d req = %u ",print_sect_txfer,res,req);
#endif
		


	

	
	}

	if (req)
		__blk_end_request_all(req, -EIO);

	spin_unlock_irq(rq->queue_lock);

	return 0;
}
#endif



						
/**************/
							 




#if 0
/* direct bio processing with a registered request  processing function*/
static void mtd_blktrans_request(struct request_queue *rq)
{
	struct request *req;
	int i;
	struct bio_vec *bvec;
	struct bio *bio;

	int sectors_xferred;
	struct mymtd_blktrans_dev *dev;
	struct mtd_blktrans_ops *tr;
	int res=0;
	unsigned long block, nsect;
	char *buf;
	int sects_to_transfer;

	dev = rq->queuedata;
	tr = dev->tr;

	spin_lock(rq->queue_lock);
	req = blk_fetch_request(rq);
	spin_unlock(rq->queue_lock);
	//while ((req = blk_fetch_request(rq)) != NULL)
	while(req != NULL)
	{

	
		printk(KERN_INFO "mtd_blktrans_request: ");	

		if (req == NULL ) {

		res =  -EIO;
		printk(KERN_INFO "FTL ERROR: req is NULL");
		goto after_req;
		}
		sectors_xferred = 0;

		if (req->cmd_type != REQ_TYPE_FS)
		{
			res =  -EIO;
			printk(KERN_INFO "FTL ERROR: REQ_TYPE is not FS");
			goto after_req;
		}

		if (blk_rq_pos(req) + blk_rq_cur_sectors(req) >
		get_capacity(req->rq_disk))
		{
			res = -EIO;
			printk(KERN_INFO "FTL ERROR: REQ > capacity");
			goto after_req;
		}

		
		if (req->cmd_flags & REQ_DISCARD)
		{
			res = dev->tr->discard(dev, block, nsect);
			printk(KERN_INFO "FTL ERROR: REQ is discard");
			goto after_req;
		}
		//printk(KERN_INFO "FTL: new req");
		
		
		__rq_for_each_bio(bio, req) {
	

		/* Do each segment independently. */
		bio_for_each_segment(bvec, bio, i) {

		block = bio->bi_sector << 9 >> dev->tr->blkshift;
		nsect = bio_iovec(bio)->bv_len >> dev->tr->blkshift;
		sects_to_transfer = nsect;
		buf = 	page_address(bvec->bv_page) + bvec->bv_offset;
		printk(KERN_INFO "block = %d nsect = %d buf = %u",block,nsect,buf); 

				
				
		switch(rq_data_dir(req)) {
			case READ:
				for (; nsect > 0; nsect--, block++, buf += dev->tr->blksize){
				if (dev->tr->readsect(dev, block, buf)){						
				res =  -EIO;
				goto after_req;
				}
			}
			rq_flush_dcache_pages(req);
			break;
			case WRITE:
				if (!dev->tr->writesect){
					
				res =  -EIO;
				goto after_req;
			}
			
			rq_flush_dcache_pages(req);
			for (; nsect > 0; nsect--, block++, buf += dev->tr->blksize){
			if (dev->tr->writesect(dev, block, buf)){
			res =  -EIO;
			goto after_req;
			}
			}
			break;
			default:
			printk(KERN_NOTICE "Unknown request %u\n", rq_data_dir(req));
			res =  -EIO;
		}
				

		/* is this correct on error path?*/
		sectors_xferred += (sects_to_transfer-nsect);

			
				
	
		}
			
	
	}
	
after_req:
		
	if(!__blk_end_request(req, res, sectors_xferred*512))
	req = NULL;
		
		
	//	printk(KERN_INFO "sectstrfd = %d res = %d req = %u ",sectors_xferred,res,req);
						spin_lock(rq->queue_lock);
						req = blk_fetch_request(rq);
						spin_unlock(rq->queue_lock);

	}
end_of_func:
	
	if (req){

	__blk_end_request_all(req, -EIO);
		
	}
	
}
#endif
						 
/**************/

						
													 
#if 0
/* original request processing function*/						
static void mtd_blktrans_request(struct request_queue *rq)
{
	struct mymtd_blktrans_dev *dev;
	struct request *req = NULL;

	dev = rq->queuedata;

	if (!dev)
		while ((req = blk_fetch_request(rq)) != NULL)
			__blk_end_request_all(req, -ENODEV);
	else
		wake_up_process(dev->thread[0]);
}
#endif

/**************/

#if 0
						
/* direct bio processing with a registered request  processing function*/
						
static void mtd_blktrans_request(struct request_queue *rq)
{
	struct request *req;
	int i;
	struct bio_vec *bvec;
	struct bio *bio;
	
	int sectors_xferred;
	struct mymtd_blktrans_dev *dev;
	struct mtd_blktrans_ops *tr;
	int res=0;
	unsigned long block, nsect;
	char *buf;
	int sects_to_transfer;
	
	dev = rq->queuedata;
	tr = dev->tr;
	
	spin_lock(rq->queue_lock);
	req = blk_fetch_request(rq);
	spin_unlock(rq->queue_lock);
	//while ((req = blk_fetch_request(rq)) != NULL)
	while(req != NULL)
	{

	
		printk(KERN_INFO "mtd_blktrans_request: ");	
		
		if (req == NULL ) {
				
			res =  -EIO;
			printk(KERN_INFO "FTL ERROR: req is NULL");
			goto after_req;
		}
		sectors_xferred = 0;
		
		if (req->cmd_type != REQ_TYPE_FS)
		{
			res =  -EIO;
			printk(KERN_INFO "FTL ERROR: REQ_TYPE is not FS");
			goto after_req;
		}

		if (blk_rq_pos(req) + blk_rq_cur_sectors(req) >
				  get_capacity(req->rq_disk))
		{
			res = -EIO;
			printk(KERN_INFO "FTL ERROR: REQ > capacity");
			goto after_req;
		}

		
		if (req->cmd_flags & REQ_DISCARD)
		{
			res = dev->tr->discard(dev, block, nsect);
			printk(KERN_INFO "FTL ERROR: REQ is discard");
			goto after_req;
		}
		//printk(KERN_INFO "FTL: new req");
		
		
		__rq_for_each_bio(bio, req) {
					
			
			/* Do each segment independently. */
			bio_for_each_segment(bvec, bio, i) {
				
				block = bio->bi_sector << 9 >> dev->tr->blkshift;
				nsect = bio_iovec(bio)->bv_len >> dev->tr->blkshift;
				sects_to_transfer = nsect;
				buf = 	page_address(bvec->bv_page) + bvec->bv_offset;
				printk(KERN_INFO "block = %d nsect = %d buf = %u",block,nsect,buf); 
				
				
				
				switch(rq_data_dir(req)) {
					case READ:
						for (; nsect > 0; nsect--, block++, buf += dev->tr->blksize){
							if (dev->tr->readsect(dev, block, buf)){						
								res =  -EIO;
								goto after_req;
							}
						}
						rq_flush_dcache_pages(req);
						break;
					case WRITE:
						if (!dev->tr->writesect){
							
							res =  -EIO;
							goto after_req;
						}
		
						rq_flush_dcache_pages(req);
						for (; nsect > 0; nsect--, block++, buf += dev->tr->blksize){
							if (dev->tr->writesect(dev, block, buf)){
								res =  -EIO;
								goto after_req;
							}
						}
						break;
					default:
						printk(KERN_NOTICE "Unknown request %u\n", rq_data_dir(req));
						res =  -EIO;
				}
				
				
				/* is this correct on error path?*/
				sectors_xferred += (sects_to_transfer-nsect);
			
			
				
	
			}
			
	
		}
	
after_req:
		
		if(!__blk_end_request(req, res, sectors_xferred*512))
			req = NULL;
		
		
	//	printk(KERN_INFO "sectstrfd = %d res = %d req = %u ",sectors_xferred,res,req);
		spin_lock(rq->queue_lock);
		req = blk_fetch_request(rq);
		spin_unlock(rq->queue_lock);

	}
end_of_func:
	
	if (req){
		
		__blk_end_request_all(req, -EIO);
		
	}
	
}
#endif
							 
/**************/							 
#if 0
/* direct bio processing without request queue*/
/* template lifted from dcssblk_make_request from dcssblk.c*/

static int ftl_make_request(struct request_queue *rq, struct bio *bio)
{
	
	
	int i;
	struct bio_vec *bvec;
	
	
	int sectors_xferred;
	struct mymtd_blktrans_dev *dev;
	
	int res=0;
	unsigned long block, nsect;
	char *buf;
	int sects_to_transfer;
	
	printk(KERN_INFO "ftlmake_req: ");
	
	dev = rq->queuedata;

	if (dev == NULL)
	goto fail;
#if 0
	if ((bio->bi_sector & 7) != 0 || (bio->bi_size & 4095) != 0)
	/* Request is not page-aligned. */
	goto fail;
	
	
	if (((bio->bi_size >> 9) + bio->bi_sector) > get_capacity(bio->bi_bdev->bd_disk)) 
	{
		printk(KERN_INFO "FTL err: bio size > capacity");
		goto fail;
	}
#endif		     

	sectors_xferred = 0;


	bio_for_each_segment(bvec, bio, i) {
	block = bio->bi_sector << 9 >> dev->tr->blkshift;
	nsect = bio_iovec(bio)->bv_len >> dev->tr->blkshift;
	sects_to_transfer = nsect;
	buf = page_address(bvec->bv_page) + bvec->bv_offset;
	printk(KERN_INFO "block = %d nsect = %d buf = %u",block,nsect,buf); 		
	switch(bio_data_dir(bio)) {
			case READ:
				for (; nsect > 0; nsect--, block++, buf += dev->tr->blksize){
				if (dev->tr->readsect(dev, block, buf)){						
				res =  -EIO;
				goto after_req;
				}
			}
			//rq_flush_dcache_pages(req);
			break;
			case WRITE:
			if (!dev->tr->writesect){

			res =  -EIO;
			goto after_req;
			}

			//rq_flush_dcache_pages(req);
			for (; nsect > 0; nsect--, block++, buf += dev->tr->blksize){
			if (dev->tr->writesect(dev, block, buf)){
			res =  -EIO;
			goto after_req;
			}
			}
			break;
			default:
				printk(KERN_NOTICE "Unknown request %u\n", bio_data_dir(bio));
				res =  -EIO;
			}
		
	
			/* is this correct on error path?*/
			sectors_xferred += (sects_to_transfer-nsect);
	

		}
after_req:
		bio_endio(bio, res);
	
		return 0;
fail:
		printk(" bio fail in ftl_make_request");
		bio_io_error(bio);
	
		return 0;
}
#endif
/**************/
						 








void init_device_queues(struct mymtd_blktrans_dev *dev)
{

	int i;
	
	#ifdef NORMAL_Q
	mybiolist_cachep = kmem_cache_create("mybioQ",
					   sizeof(struct my_bio_list), 0, SLAB_PANIC, NULL);
	
	biolistpool = mempool_create(BLKDEV_MIN_RQ, mempool_alloc_slab,
					  mempool_free_slab, mybiolist_cachep);
	for(i = 0;i < VIRGO_NUM_MAX_REQ_Q;i++)
		INIT_LIST_HEAD(&dev->qu[i].qelem_ptr);
	#endif
	
	
	mybiolist_cachep = kmem_cache_create("mybioQ",
					     sizeof(struct bio_node), 0, SLAB_PANIC, NULL);
	
	biolistpool = mempool_create(BLKDEV_MIN_RQ, mempool_alloc_slab,
					  mempool_free_slab, mybiolist_cachep);
	
	
	//struct my_bio_list *tmp = mempool_alloc(dev->biolistpool, gfp_mask);
	//mempool_free(tmp, dev->biolistpool);
	
	
	for(i = 0;i < VIRGO_NUM_MAX_REQ_Q;i++)
		lfq_init_rcu(&rcuqu[i], call_rcu);
		
	for(i = 0;i < VIRGO_NUM_MAX_REQ_Q;i++)
		spin_lock_init(&dev->mybioq_lock[i]);
}

void deinit_device_queues(struct mymtd_blktrans_dev *dev)
{
	mempool_destroy(biolistpool);
	
	kmem_cache_destroy(mybiolist_cachep);
	
}
#if 0
static int ftl_make_request(struct request_queue *rq, struct bio *bio)
{

	struct mymtd_blktrans_dev *dev;
	int qnum;	
	gfp_t gfp_mask;
	struct my_bio_list *tmp;
	unsigned long temp_rand;		
	



	dev = rq->queuedata;
	
	if (dev == NULL)
		goto fail;
	get_random_bytes(&temp_rand, sizeof(temp_rand));
	qnum = temp_rand%VIRGO_NUM_MAX_REQ_Q;
	//printk(KERN_INFO "ftlmake_req: %d",qnum);	
	//gfp_mask = GFP_NOIO |  __GFP_WAIT;
	gfp_mask = GFP_ATOMIC | GFP_NOFS;
	tmp= mempool_alloc(dev->biolistpool, gfp_mask);
	if (!tmp)
	{
		printk(KERN_ERR "mtftl: mempool_alloc fail");
		goto fail;
	}
	tmp->bio = bio;
	spin_lock(&dev->mybioq_lock[qnum]);
	list_add_tail(&(tmp->qelem_ptr), &(dev->qu[qnum].qelem_ptr));
	spin_unlock(&dev->mybioq_lock[qnum]);
#if 1
	if(task_is_stopped(dev->thread[qnum]))
		  printk(KERN_INFO "thread %d sleeping...",qnum);
	if(wake_up_process(dev->thread[qnum]) == 1)
		//printk(KERN_INFO "thread %d woken from sleep",qnum);
#endif
		;
	return 0;
fail:
	printk(" fail:  ftl_make_request");
	return -1;
}
#endif

						
#ifdef NORMAL_Q


static int ftl_make_request(struct request_queue *rq, struct bio *bio)
{

	struct mymtd_blktrans_dev *dev;
	int qnum;	
	gfp_t gfp_mask;
	struct my_bio_list *tmp;
	unsigned long temp_rand;		
	
	int i;		
	int found;
	
	
	uint32_t lpn;


	dev = rq->queuedata;
	
	if (dev == NULL)
		goto fail;
	if(bio_data_dir(bio) == WRITE)
	{
		lpn = ((bio->bi_sector << 9) >> 15);
		found = 0;
		for(i = 0; i < VIRGO_NUM_MAX_REQ_Q;i++)
		{
			if(lpn == last_lpn[i])
			{
				qnum = i;
				found = 1;
			}
		}
	
		if(found == 0)
		{
			get_random_bytes(&temp_rand, sizeof(temp_rand));
			qnum = temp_rand%VIRGO_NUM_MAX_REQ_Q;
		}
		last_lpn[qnum] = lpn;
	}
	else
	{
			get_random_bytes(&temp_rand, sizeof(temp_rand));
			qnum = temp_rand%VIRGO_NUM_MAX_REQ_Q;
	}
	
	
	
	
	//printk(KERN_INFO "ftlmake_req: %d",qnum);	
	//gfp_mask = GFP_NOIO |  __GFP_WAIT;
	gfp_mask = GFP_ATOMIC | GFP_NOFS;
	tmp= mempool_alloc(biolistpool, gfp_mask);
	if (!tmp)
	{
		printk(KERN_ERR "mtftl: mempool_alloc fail");
		goto fail;
	}
	tmp->bio = bio;
	spin_lock(&dev->mybioq_lock[qnum]);
	list_add_tail(&(tmp->qelem_ptr), &(dev->qu[qnum].qelem_ptr));
	spin_unlock(&dev->mybioq_lock[qnum]);
#if 1
	if(task_is_stopped(dev->thread[qnum]))
		  printk(KERN_INFO "thread %d sleeping...",qnum);

	test_and_set_bit(qnum, dev->active_iokthread);

	
	if(wake_up_process(dev->thread[qnum]) == 1)
		//printk(KERN_INFO "thread %d woken from sleep",qnum);
#endif
		;
	return 0;
fail:
	printk(" fail:  ftl_make_request");
	return -1;
}


static int mymtd_blktrans_thread(void *arg)
{
	struct bio_vec *bvec;
	int sectors_xferred;
	struct mymtd_blktrans_dev *dev;
	
	int res=0;
	uint64_t block, nsect;
	char *buf;
	int sects_to_transfer;
	struct list_head *list_hdp;
	struct bio *bio;
	int qnum = 0;
	struct my_bio_list *tmp;
	int i;
	int printcount = 0;
	int sleep_count = 0;
			
	dev = ((struct thread_arg_data *)arg)->dev;
	qnum = ((struct thread_arg_data *)arg)->qno;
	printk(KERN_INFO "myftl: thread %d inited",qnum);
	
	while (!kthread_should_stop()) {
		
#if 1
		/* no "lost wake-up" problem !! is the idiom usage correct?*/
		set_current_state(TASK_INTERRUPTIBLE);
		spin_lock(&dev->mybioq_lock[qnum]);
		if(list_empty(&(dev->qu[qnum].qelem_ptr)))
		{
			if(printcount<1)
			{
				//printk(KERN_INFO "myftl: thread %d emptQ",qnum);
				printcount =1; 
			}
			
	//		set_current_state(TASK_RUNNING);
			spin_unlock(&dev->mybioq_lock[qnum]);
//			if (kthread_should_stop())
//				set_current_state(TASK_RUNNING);
			
			/* wait in anticipation, before going to sleep*/
			if(sleep_count < 100)
			{
				sleep_count++;
				set_current_state(TASK_RUNNING);
				schedule();
			}
			else
			{

				test_and_clear_bit(qnum, dev->active_iokthread);

				schedule();
				sleep_count = 0;
			}
			
			continue;

		}
		set_current_state(TASK_RUNNING);
		spin_unlock(&dev->mybioq_lock[qnum]);
		
#else	
		
		spin_lock(&dev->mybioq_lock[qnum]);
		if(list_empty(&(dev->qu[qnum].qelem_ptr)))
		{
			//set_current_state(TASK_INTERRUPTIBLE);
			spin_unlock(&dev->mybioq_lock[qnum]);
			schedule();
			continue;
			
		}
		spin_unlock(&dev->mybioq_lock[qnum]);
		
#endif
		
		

		printcount = 0;		 
		
		spin_lock(&dev->mybioq_lock[qnum]);
		list_hdp = &(dev->qu[qnum].qelem_ptr);
		tmp = list_first_entry(list_hdp, struct my_bio_list, qelem_ptr);

		spin_unlock(&dev->mybioq_lock[qnum]);
		
		bio = tmp->bio;
		sectors_xferred = 0;
		     
	
#if 1
	/* like in nfhd_make_request*/	
		block = ((bio->bi_sector << 9) >> dev->tr->blkshift);
		//nsect = ((bio_iovec(bio)->bv_len) >> dev->tr->blkshift);
		//sects_to_transfer = nsect;
		//bvec = ((&((bio)->bi_io_vec[((bio)->bi_idx)])));
		//buf = bio_data(bio);
		//printk(KERN_INFO "vcnt = %d",bio->bi_vcnt);
		bio_for_each_segment(bvec, bio, i) {
			
			nsect = ((bvec->bv_len) >> dev->tr->blkshift);
			sects_to_transfer = nsect;		
		
			//printk(KERN_INFO "bisect= %ld bvlen = %ld", bio->bi_sector,bvec->bv_len);
			//printk(KERN_INFO "block = %ld nsect = %ld",block,nsect);
			buf = page_address(bvec->bv_page) + bvec->bv_offset;
			
			//printk(KERN_INFO "sectnum = %u nsect = %ld buf = %u",block,nsect,buf); 		
			switch(bio_data_dir(bio)) {
				case READ:
					for (; nsect > 0; nsect--, block++, buf += dev->tr->blksize){
						if (dev->tr->readsect(dev, block, buf)){						
						res =  -EIO;
						goto fail;
					}
				}
				//rq_flush_dcache_pages(req);
				break;
				case WRITE:
					if (!dev->tr->writesect){
						
						res =  -EIO;
						goto fail;
					}

					//rq_flush_dcache_pages(req);
					for (; nsect > 0; nsect--, block++, buf += dev->tr->blksize){
#if 1
						if (dev->tr->writesect(dev, block, buf)){
							res =  -EIO;
							goto fail;

						}
#endif
					}

					break;
				default:
					printk(KERN_NOTICE "Unknown request %ul\n", bio_data_dir(bio));
					res =  -EIO;
			}
		
		
			/* is this correct on error path?*/
			sectors_xferred += (sects_to_transfer-nsect);
	

		}
#else
		udelay(200);
#endif
		bio_endio(bio, res);
		spin_lock(&dev->mybioq_lock[qnum]);
		list_del(&(tmp->qelem_ptr));
		spin_unlock(&dev->mybioq_lock[qnum]);
		mempool_free(tmp,biolistpool);
		continue;
fail:
		printk(" bio fail in ftl_make_request");
		bio_io_error(bio);
		spin_lock(&dev->mybioq_lock[qnum]);
		list_del(&(tmp->qelem_ptr));
		spin_unlock(&dev->mybioq_lock[qnum]);
		mempool_free(tmp,biolistpool);
		
	
	
	}
	return 0;


}


#endif
										
#ifndef NORMAL_Q
static int ftl_make_request(struct request_queue *rq, struct bio *bio)
{

	struct mymtd_blktrans_dev *dev;
	int qnum;	
	gfp_t gfp_mask;

	unsigned long temp_rand;
	int i;		
	int found;
	
	struct bio_node *node;
	uint32_t lpn;


	dev = rq->queuedata;
	
	if (dev == NULL)
		goto fail;
	
	if(bio_data_dir(bio) == WRITE)
	{
		lpn = ((bio->bi_sector << 9) >> 15);
		found = 0;
		for(i = 0; i < VIRGO_NUM_MAX_REQ_Q;i++)
		{
			if(lpn == last_lpn[i])
			{
				qnum = i;
				found = 1;
			}
		}
	
		if(found == 0)
		{
			get_random_bytes(&temp_rand, sizeof(temp_rand));
			qnum = temp_rand%VIRGO_NUM_MAX_REQ_Q;
		}
		last_lpn[qnum] = lpn;
	}
	else
	{
			get_random_bytes(&temp_rand, sizeof(temp_rand));
			qnum = temp_rand%VIRGO_NUM_MAX_REQ_Q;
	}
	
	
	//printk(KERN_INFO "ftlmake_req: %d",qnum);	
	//gfp_mask = GFP_NOIO |  __GFP_WAIT;
	gfp_mask = GFP_ATOMIC | GFP_NOFS;
	
	node = mempool_alloc(biolistpool, gfp_mask);
				
	if (!node)
	{
		printk(KERN_ERR "mtftl: mempool_alloc fail");
		BUG();
	}
	node->bio = bio;
	lfq_node_init_rcu(&node->list);
	rcu_read_lock();
	lockfree_enqueue(&rcuqu[qnum], &node->list);
	rcu_read_unlock();
	
	
	
#if 1
	if(task_is_stopped(dev->thread[qnum]))
		printk(KERN_INFO "thread %d sleeping...",qnum);
	if(wake_up_process(dev->thread[qnum]) == 1)
		//printk(KERN_INFO "thread %d woken from sleep",qnum);
		;
#endif
	
	return 0;
fail:
		printk(" fail:  ftl_make_request");
	return -1;
}
#endif

#if 0

static int mymtd_blktrans_thread(void *arg)
{
	struct bio_vec *bvec;
	int sectors_xferred;
	struct mymtd_blktrans_dev *dev;
	
	int res=0;
	uint64_t block, nsect;
	char *buf;
	int sects_to_transfer;
	struct list_head *list_hdp;
	struct bio *bio;
	int qnum = 0;
	struct my_bio_list *tmp;
	int i;
	int printcount = 0;
			
	dev = ((struct thread_arg_data *)arg)->dev;
	qnum = ((struct thread_arg_data *)arg)->qno;
	printk(KERN_INFO "myftl: thread %d inited",qnum);
	
	while (!kthread_should_stop()) {
		
#if 1
		/* no "lost wake-up" problem !! is the idiom usage correct?*/
		set_current_state(TASK_INTERRUPTIBLE);
		spin_lock(&dev->mybioq_lock[qnum]);
		if(list_empty(&(dev->qu[qnum].qelem_ptr)))
		{
			if(printcount<1)
			{
				//printk(KERN_INFO "myftl: thread %d emptQ",qnum);
				printcount =1; 
			}
			
			set_current_state(TASK_RUNNING);
			spin_unlock(&dev->mybioq_lock[qnum]);
//			if (kthread_should_stop())
//				set_current_state(TASK_RUNNING);
			schedule();
			
			continue;

		}
		spin_unlock(&dev->mybioq_lock[qnum]);
		set_current_state(TASK_RUNNING);
#else	
		
		spin_lock(&dev->mybioq_lock[qnum]);
		if(list_empty(&(dev->qu[qnum].qelem_ptr)))
		{
			//set_current_state(TASK_INTERRUPTIBLE);
			spin_unlock(&dev->mybioq_lock[qnum]);
			schedule();
			continue;
			
		}
		spin_unlock(&dev->mybioq_lock[qnum]);
		
#endif
		
		

		printcount = 0;		 
		
		spin_lock(&dev->mybioq_lock[qnum]);
		list_hdp = &(dev->qu[qnum].qelem_ptr);
		tmp = list_first_entry(list_hdp, struct my_bio_list, qelem_ptr);

		spin_unlock(&dev->mybioq_lock[qnum]);
		
		bio = tmp->bio;
		sectors_xferred = 0;
		     
	
#if 1
	/* like in nfhd_make_request*/	
		block = ((bio->bi_sector << 9) >> dev->tr->blkshift);
		//nsect = ((bio_iovec(bio)->bv_len) >> dev->tr->blkshift);
		//sects_to_transfer = nsect;
		//bvec = ((&((bio)->bi_io_vec[((bio)->bi_idx)])));
		//buf = bio_data(bio);
		//printk(KERN_INFO "vcnt = %d",bio->bi_vcnt);
		bio_for_each_segment(bvec, bio, i) {
			
			nsect = ((bvec->bv_len) >> dev->tr->blkshift);
			sects_to_transfer = nsect;		
		
			//printk(KERN_INFO "bisect= %ld bvlen = %ld", bio->bi_sector,bvec->bv_len);
			//printk(KERN_INFO "block = %ld nsect = %ld",block,nsect);
			buf = page_address(bvec->bv_page) + bvec->bv_offset;
			
			//printk(KERN_INFO "sectnum = %u nsect = %ld buf = %u",block,nsect,buf); 		
			switch(bio_data_dir(bio)) {
				case READ:
					for (; nsect > 0; nsect--, block++, buf += dev->tr->blksize){
						if (dev->tr->readsect(dev, block, buf)){						
						res =  -EIO;
						goto fail;
					}
				}
				//rq_flush_dcache_pages(req);
				break;
				case WRITE:
					if (!dev->tr->writesect){
						
						res =  -EIO;
						goto fail;
					}

					//rq_flush_dcache_pages(req);
					for (; nsect > 0; nsect--, block++, buf += dev->tr->blksize){
#if 1
						if (dev->tr->writesect(dev, block, buf)){
							res =  -EIO;
							goto fail;

						}
#endif
					}

					break;
				default:
					printk(KERN_NOTICE "Unknown request %ul\n", bio_data_dir(bio));
					res =  -EIO;
			}
		
		
			/* is this correct on error path?*/
			sectors_xferred += (sects_to_transfer-nsect);
	

		}
#else
		udelay(200);
#endif
		bio_endio(bio, res);
		spin_lock(&dev->mybioq_lock[qnum]);
		list_del(&(tmp->qelem_ptr));
		spin_unlock(&dev->mybioq_lock[qnum]);
		mempool_free(tmp,dev->biolistpool);
		continue;
fail:
		printk(" bio fail in ftl_make_request");
		bio_io_error(bio);
		spin_lock(&dev->mybioq_lock[qnum]);
		list_del(&(tmp->qelem_ptr));
		spin_unlock(&dev->mybioq_lock[qnum]);
		mempool_free(tmp,dev->biolistpool);
		
	
	
	}
	return 0;


}
#endif
					 
void free_bio_node(struct rcu_head *head)
{
	struct bio_node *node =
			container_of(head, struct bio_node, rcu);
	
	mempool_free(node,biolistpool);
	
	//kfree(node);
}						

#ifndef NORMAL_Q
static int mymtd_blktrans_thread(void *arg)
{
	struct bio_vec *bvec;
	int sectors_xferred;
	struct mymtd_blktrans_dev *dev;
	
	int res=0;
	uint64_t block, nsect;
	char *buf;
	int sects_to_transfer;
	
	struct bio *bio;
	int qnum = 0;

	int i;
	int printcount = 0;
	struct bio_node *node;
	struct lfq_node_rcu *qnode;
			
	dev = ((struct thread_arg_data *)arg)->dev;
	qnum = ((struct thread_arg_data *)arg)->qno;
	printk(KERN_INFO "myftl: thread %d inited",qnum);
	
	while (!kthread_should_stop()) {
		

		/* no "lost wake-up" problem !! is the idiom usage correct?*/
		set_current_state(TASK_UNINTERRUPTIBLE);
		
		
		rcu_read_lock();
		qnode = lockfree_dequeue(&rcuqu[qnum]);
		node = container_of(qnode, struct bio_node, list);
		rcu_read_unlock();
	
		if(node == NULL)
		{
			if(printcount<1)
			{
				//printk(KERN_INFO "myftl: thread %d emptQ",qnum);
				printcount =1; 
			}
			
			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule();
			
			continue;
		}
	
		set_current_state(TASK_RUNNING);

		bio = node->bio;
		printcount = 0;		 
		sectors_xferred = 0;
		     
	

		/* like in nfhd_make_request*/	
		block = ((bio->bi_sector << 9) >> dev->tr->blkshift);
		//nsect = ((bio_iovec(bio)->bv_len) >> dev->tr->blkshift);
		//sects_to_transfer = nsect;
		//bvec = ((&((bio)->bi_io_vec[((bio)->bi_idx)])));
		//buf = bio_data(bio);
		#if 0
		printk(KERN_INFO "vcnt = %d",bio->bi_vcnt);
		#endif
		bio_for_each_segment(bvec, bio, i) {
			
			nsect = ((bvec->bv_len) >> dev->tr->blkshift);
			sects_to_transfer = nsect;		
		#if 0
			printk(KERN_INFO "%x: bisect= %ld bvlen = %ld type = %d", current->pid,bio->bi_sector,bvec->bv_len,bio_data_dir(bio));
		#endif
			//printk(KERN_INFO "block = %ld nsect = %ld",block,nsect);
			buf = page_address(bvec->bv_page) + bvec->bv_offset;
			
			//printk(KERN_INFO "sectnum = %u nsect = %ld buf = %u",block,nsect,buf); 		
			switch(bio_data_dir(bio)) {
				case READ:
					for (; nsect > 0; nsect--, block++, buf += dev->tr->blksize){
						if (dev->tr->readsect(dev, block, buf)){						
							res =  -EIO;
							goto fail;
						}
					}
				//rq_flush_dcache_pages(req);
					break;
				case WRITE:
					if (!dev->tr->writesect){
						
						res =  -EIO;
						goto fail;
					}

					//rq_flush_dcache_pages(req);
					for (; nsect > 0; nsect--, block++, buf += dev->tr->blksize){
#if 1
						if (dev->tr->writesect(dev, block, buf)){
							res =  -EIO;
							goto fail;

						}
#endif
					}

					break;
				default:
					printk(KERN_NOTICE "Unknown request %ul\n", bio_data_dir(bio));
					res =  -EIO;
			}
		
		
			/* is this correct on error path?*/
			sectors_xferred += (sects_to_transfer-nsect);
	

		}
#if 0
		printk(KERN_INFO "%x:endio bisect= %ld bvlen = %ld type = %d", current->pid,bio->bi_sector,bvec->bv_len,bio_data_dir(bio));
#endif	
		bio_endio(bio, res);
		call_rcu(&node->rcu, free_bio_node);	
		
		continue;
fail:
		printk(" bio fail in ftl_make_request");
		bio_io_error(bio);
		call_rcu(&node->rcu, free_bio_node);	
		
		
	
	
	}
	return 0;


}					 
#endif					 
							 
#if 0
static int mtd_blktrans_thread(void *arg)
{
	return 0;
}
#endif
						 

static int blktrans_open(struct block_device *bdev, fmode_t mode)
{
	struct mymtd_blktrans_dev *dev = blktrans_dev_get(bdev->bd_disk);
	int ret = 0;

	if (!dev)
		return -ERESTARTSYS; /* FIXME: busy loop! -arnd*/

	mutex_lock(&dev->lock);

	if (dev->open++)
		goto unlock;

	kref_get(&dev->ref);
	__module_get(dev->tr->owner);

	if (dev->mtd) {
		ret = dev->tr->open ? dev->tr->open(dev) : 0;
		__get_mtd_device(dev->mtd);
	}

unlock:
	mutex_unlock(&dev->lock);
	blktrans_dev_put(dev);
	return ret;
}

static int blktrans_release(struct gendisk *disk, fmode_t mode)
{
	struct mymtd_blktrans_dev *dev = blktrans_dev_get(disk);
	int ret = 0;

	if (!dev)
		return ret;

	mutex_lock(&dev->lock);

	if (--dev->open)
		goto unlock;

	kref_put(&dev->ref, blktrans_dev_release);
	module_put(dev->tr->owner);

	if (dev->mtd) {
		ret = dev->tr->release ? dev->tr->release(dev) : 0;
		__put_mtd_device(dev->mtd);
	}
unlock:
	mutex_unlock(&dev->lock);
	blktrans_dev_put(dev);
	return ret;
}

static int blktrans_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
	struct mymtd_blktrans_dev *dev = blktrans_dev_get(bdev->bd_disk);
	int ret = -ENXIO;

	if (!dev)
		return ret;

	mutex_lock(&dev->lock);

	if (!dev->mtd)
		goto unlock;

	ret = dev->tr->getgeo ? dev->tr->getgeo(dev, geo) : 0;
unlock:
	mutex_unlock(&dev->lock);
	blktrans_dev_put(dev);
	return ret;
}

#define BANKINFOGET 0xFFFFFFFF
#define PREPARE_GC  0xFFFFFFFE
#define BANKINFO_FWR 0xFFFFFFFD
#define FTL_PREFETCH 0xFFFFFFFC

static int blktrans_ioctl(struct block_device *bdev, fmode_t mode,
			      unsigned int cmd, unsigned long arg)
{
	struct mymtd_blktrans_dev *dev = blktrans_dev_get(bdev->bd_disk);
	int ret = -ENXIO;

	printk(KERN_INFO "blktrans_ioctl cmd = %d",cmd);
	if (!dev)
		return ret;

	printk(KERN_INFO "blktrans_ioctl !dev cmd = %d",cmd);
	mutex_lock(&dev->lock);

	if (!dev->mtd)
		goto unlock;
	printk(KERN_INFO "blktrans_ioctl !dev->mtd cmd = %d arg = %x",cmd,arg);

	switch (cmd) {
	
	case BLKFLSBUF:
		printk(KERN_INFO "blktrans_ioctl BLKFLSBUF");
		ret = dev->tr->flush ? dev->tr->flush(dev) : 0;
		break;
	case BANKINFOGET:
		printk(KERN_INFO "blktrans_ioctl BLKSSZGET");
		dev->tr->get_blkinfo(dev);
		break;
	case PREPARE_GC:
		printk(KERN_INFO "blktrans_ioctl PREPARE_GC");
		dev->tr->prepare_for_gctest(dev);
		break;
	case BANKINFO_FWR:
		printk(KERN_INFO "blktrans_ioctl BANKINFO_FWR");
		dev->tr->bankinfo_filewr(dev);
		break;
	case FTL_PREFETCH:
		printk(KERN_INFO "blktrans_ioctl FTL_PREFETCH");
		dev->tr->convey_pfetch_list(dev,arg);
		break;
	default:
		printk(KERN_INFO "blktrans_ioctl default");
		ret = -ENOTTY;
	}
unlock:
	mutex_unlock(&dev->lock);
	blktrans_dev_put(dev);
	return ret;
}

static const struct block_device_operations mtd_blktrans_ops = {
	.owner		= THIS_MODULE,
	.open		= blktrans_open,
	.release	= blktrans_release,
	.ioctl		= blktrans_ioctl,
	.getgeo		= blktrans_getgeo,
};



int add_mymtd_blktrans_dev(struct mymtd_blktrans_dev *new)
{
	struct mtd_blktrans_ops *tr = new->tr;
	struct mymtd_blktrans_dev *d;
	int last_devnum = -1;
	struct gendisk *gd;
	int ret;
	int i;
	

	if (mutex_trylock(&mtd_table_mutex)) {
		mutex_unlock(&mtd_table_mutex);
		BUG();
	}

	mutex_lock(&blktrans_ref_mutex);
	list_for_each_entry(d, &tr->devs, list) {
		if (new->devnum == -1) {
			/* Use first free number */
			if (d->devnum != last_devnum+1) {
				/* Found a free devnum. Plug it in here */
				new->devnum = last_devnum+1;
				list_add_tail(&new->list, &d->list);
				goto added;
			}
		} else if (d->devnum == new->devnum) {
			/* Required number taken */
			mutex_unlock(&blktrans_ref_mutex);
			return -EBUSY;
		} else if (d->devnum > new->devnum) {
			/* Required number was free */
			list_add_tail(&new->list, &d->list);
			goto added;
		}
		last_devnum = d->devnum;
	}

	ret = -EBUSY;
	if (new->devnum == -1)
		new->devnum = last_devnum+1;

	/* Check that the device and any partitions will get valid
	 * minor numbers and that the disk naming code below can cope
	 * with this number. */
	if (new->devnum > (MINORMASK >> tr->part_bits) ||
	    (tr->part_bits && new->devnum >= 27 * 26)) {
		mutex_unlock(&blktrans_ref_mutex);
		goto error1;
	}

	list_add_tail(&new->list, &tr->devs);
 added:
	mutex_unlock(&blktrans_ref_mutex);

	mutex_init(&new->lock);
	kref_init(&new->ref);
	if (!tr->writesect)
		new->readonly = 1;

	/* Create gendisk */
	ret = -ENOMEM;
	gd = alloc_disk(1 << tr->part_bits);

	if (!gd)
		goto error2;

	new->disk = gd;
	gd->private_data = new;
	gd->major = tr->major;
	gd->first_minor = (new->devnum) << tr->part_bits;
	gd->fops = &mtd_blktrans_ops;

	if (tr->part_bits)
		if (new->devnum < 26)
			snprintf(gd->disk_name, sizeof(gd->disk_name),
				 "%s%c", tr->name, 'a' + new->devnum);
		else
			snprintf(gd->disk_name, sizeof(gd->disk_name),
				 "%s%c%c", tr->name,
				 'a' - 1 + new->devnum / 26,
				 'a' + new->devnum % 26);
	else
		snprintf(gd->disk_name, sizeof(gd->disk_name),
			 "%s%d", tr->name, new->devnum);

	set_capacity(gd, (new->size * tr->blksize) >> 9);

	/* Create the request queue */


#if 0
	spin_lock_init(&new->queue_lock);
	new->rq = blk_init_queue(mtd_blktrans_request, &new->queue_lock);
#else
	new->rq = blk_alloc_queue(GFP_KERNEL);
	blk_queue_make_request(new->rq, ftl_make_request);
	
#endif
	
	
	init_device_queues(new);
			
	if (!new->rq)
		goto error3;

	new->rq->queuedata = new;
	blk_queue_logical_block_size(new->rq, tr->blksize);

	if (tr->discard)
		queue_flag_set_unlocked(QUEUE_FLAG_DISCARD,
					new->rq);

	gd->queue = new->rq;
	
#if 1
	/* Create processing thread */
	/* TODO: workqueue ? */
	for(i = 0; i < VIRGO_NUM_MAX_REQ_Q;i++)
	{
		new->thrd_arg[i].dev = new;
		new->thrd_arg[i].qno = i;
		new->thread[i] = kthread_run(mymtd_blktrans_thread, &(new->thrd_arg[i]),
			"%s%d_%d", tr->name, new->mtd->index,i);

		if (IS_ERR(new->thread[i])) {
			ret = PTR_ERR(new->thread[i]);
			goto error4;
		}
	}
#else
	new->thread[0] = kthread_run(mtd_blktrans_thread, new,
					"%s%d", tr->name, new->mtd->index);
	if (IS_ERR(new->thread)) {
		ret = PTR_ERR(new->thread);
		goto error4;
	}
					
#endif
	gd->driverfs_dev = &new->mtd->dev;
	
	if (new->readonly)
		set_disk_ro(gd, 1);

	add_disk(gd);

	if (new->disk_attributes) {
		ret = sysfs_create_group(&disk_to_dev(gd)->kobj,
					new->disk_attributes);
		WARN_ON(ret);
	}
	return 0;
error4:
	blk_cleanup_queue(new->rq);
error3:
	put_disk(new->disk);
error2:
	list_del(&new->list);
error1:
	return ret;
}


		
		
int del_mymtd_blktrans_dev(struct mymtd_blktrans_dev *old)
{
	unsigned long flags;
	int i;

	if (mutex_trylock(&mtd_table_mutex)) {
		mutex_unlock(&mtd_table_mutex);
		BUG();
	
	}

	deinit_device_queues(old);
	if (old->disk_attributes)
		sysfs_remove_group(&disk_to_dev(old->disk)->kobj,
						old->disk_attributes);

	/* Stop new requests to arrive */
	del_gendisk(old->disk);


	/* Stop the thread */
	for(i = 0;i < VIRGO_NUM_MAX_REQ_Q;i++)
		kthread_stop(old->thread[i]);

	/* kfree(struct thread_arg_data *ptr);
	 */
	/* Kill current requests */
	/* how to handle currenty queued requests?*/
#if 0
	spin_lock_irqsave(&old->queue_lock, flags);
	old->rq->queuedata = NULL;
	blk_start_queue(old->rq);
	spin_unlock_irqrestore(&old->queue_lock, flags);
#endif

	/* If the device is currently open, tell trans driver to close it,
		then put mtd device, and don't touch it again */
	mutex_lock(&old->lock);
	if (old->open) {
		if (old->tr->release)
			old->tr->release(old);
		__put_mtd_device(old->mtd);
	}

	old->mtd = NULL;

	mutex_unlock(&old->lock);
	blktrans_dev_put(old);
	return 0;
}

static void blktrans_notify_remove(struct mtd_info *mtd)
{
	struct mtd_blktrans_ops *tr;
	struct mymtd_blktrans_dev *dev, *next;

	list_for_each_entry(tr, &blktrans_majors, list)
		list_for_each_entry_safe(dev, next, &tr->devs, list)
			if (dev->mtd == mtd)
				tr->remove_dev(dev);
}

static void blktrans_notify_add(struct mtd_info *mtd)
{
	struct mtd_blktrans_ops *tr;

	if (mtd->type == MTD_ABSENT)
		return;

	list_for_each_entry(tr, &blktrans_majors, list)
		tr->add_mtd(tr, mtd);
}

static struct mtd_notifier blktrans_notifier = {
	.add = blktrans_notify_add,
	.remove = blktrans_notify_remove,
};


int register_mymtd_blktrans(struct mtd_blktrans_ops *tr)
{
	struct mtd_info *mtd;
	int ret;

	/* Register the notifier if/when the first device type is
	   registered, to prevent the link/init ordering from fucking
	   us over. */
	if (!blktrans_notifier.list.next)
		register_mtd_user(&blktrans_notifier);


	mutex_lock(&mtd_table_mutex);

	ret = register_blkdev(tr->major, tr->name);
	if (ret < 0) {
		printk(KERN_WARNING "Unable to register %s block device on major %d: %d\n",
		       tr->name, tr->major, ret);
		mutex_unlock(&mtd_table_mutex);
		return ret;
	}

	if (ret)
		tr->major = ret;

	tr->blkshift = ffs(tr->blksize) - 1;

	INIT_LIST_HEAD(&tr->devs);
	list_add(&tr->list, &blktrans_majors);

	mtd_for_each_device(mtd)
		if (mtd->type != MTD_ABSENT)
			tr->add_mtd(tr, mtd);

	mutex_unlock(&mtd_table_mutex);
	return 0;
}

int deregister_mymtd_blktrans(struct mtd_blktrans_ops *tr)
{
	struct mymtd_blktrans_dev *dev, *next;

	mutex_lock(&mtd_table_mutex);

	/* Remove it from the list of active majors */
	list_del(&tr->list);

	list_for_each_entry_safe(dev, next, &tr->devs, list)
		tr->remove_dev(dev);

	unregister_blkdev(tr->major, tr->name);
	mutex_unlock(&mtd_table_mutex);

	BUG_ON(!list_empty(&tr->devs));
	return 0;
}

//static void __exit mymtd_blktrans_exit(void)
void mymtd_blktrans_exit(void)
{
	/* No race here -- if someone's currently in register_mtd_blktrans
	   we're screwed anyway. */
	if (blktrans_notifier.list.next)
		unregister_mtd_user(&blktrans_notifier);
}

//module_exit(mymtd_blktrans_exit);

//EXPORT_SYMBOL_GPL(register_mymtd_blktrans);
//EXPORT_SYMBOL_GPL(deregister_mymtd_blktrans);
//EXPORT_SYMBOL_GPL(add_mymtd_blktrans_dev);
//EXPORT_SYMBOL_GPL(del_mymtd_blktrans_dev);

MODULE_AUTHOR("Srimugunthan");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Common interface to block layer for MTD 'translation layers'");
