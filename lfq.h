#ifndef LFQ_H
#define LFQ_H

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


struct lfq_node_rcu {
	struct lfq_node_rcu *next;
	int dummy;

};

struct lfq_queue_rcu {
	struct lfq_node_rcu *head, *tail;
	void (*queue_call_rcu)(struct rcu_head *head,
	       void (*func)(struct rcu_head *head));
};


struct lfq_node_rcu_dummy {
	struct lfq_node_rcu parent;
	struct rcu_head head;
	struct lfq_queue_rcu *q;
};



struct cache_num_node {
	struct lfq_node_rcu list;
	struct rcu_head rcu;
	int value;
};

 struct lfq_node_rcu *make_dummy(struct lfq_queue_rcu *q,
				       struct lfq_node_rcu *next);
 void free_dummy_cb(struct rcu_head *head);
 void rcu_free_dummy(struct lfq_node_rcu *node);
 void free_dummy(struct lfq_node_rcu *node);
 void lfq_node_init_rcu(struct lfq_node_rcu *node);

 void lfq_init_rcu(struct lfq_queue_rcu *q,
			 void queue_call_rcu(struct rcu_head *head,
					     void (*func)(struct rcu_head *head)));
					     
 int lfq_destroy_rcu(struct lfq_queue_rcu *q);

 void lockfree_enqueue(struct lfq_queue_rcu *q,
			     struct lfq_node_rcu *node);
			     
 void enqueue_dummy(struct lfq_queue_rcu *q);

 struct lfq_node_rcu *lockfree_dequeue(struct lfq_queue_rcu *q);

void free_cache_num_node(struct rcu_head *head);
#endif