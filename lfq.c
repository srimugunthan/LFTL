/*
 * Lock-Free RCU Queue
 *
 * Copyright 2010 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Lock-free RCU queue. Enqueue and dequeue operations hold a RCU read
 * lock to deal with cmpxchg ABA problem. This queue is *not* circular:
 * head points to the oldest node, tail points to the newest node.
 * A dummy node is kept to ensure enqueue and dequeue can always proceed
 * concurrently. Keeping a separate head and tail helps with large
 * queues: enqueue and dequeue can proceed concurrently without
 * wrestling for exclusive access to the same variables.
 *
 * Dequeue retry if it detects that it would be dequeueing the last node
 * (it means a dummy node dequeue-requeue is in progress). This ensures
 * that there is always at least one node in the queue.
 *
 * In the dequeue operation, we internally reallocate the dummy node
 * upon dequeue/requeue and use call_rcu to free the old one after a
 * grace period.
 */



#include "lfq.h"

/* begins the Mathieu desonoyers RCU based lock free queue */
 struct lfq_node_rcu *make_dummy(struct lfq_queue_rcu *q,
				       struct lfq_node_rcu *next)
{
	struct lfq_node_rcu_dummy *dummy;

	dummy = kmalloc(sizeof(struct lfq_node_rcu_dummy),GFP_KERNEL);
	if(dummy == NULL)
	{
		printk(KERN_INFO "kmalloc fail");
		BUG();
	}
	dummy->parent.next = next;
	dummy->parent.dummy = 1;
	dummy->q = q;
	return &dummy->parent;
}

 void free_dummy_cb(struct rcu_head *head)
{
	struct lfq_node_rcu_dummy *dummy =
			container_of(head, struct lfq_node_rcu_dummy, head);
	kfree(dummy);
}

 void rcu_free_dummy(struct lfq_node_rcu *node)
{
	struct lfq_node_rcu_dummy *dummy;

	if(node->dummy == NULL)
	{
		printk(KERN_INFO "rcu_free_dummy : asking to free a NULL ptr");
		BUG();
	}
	dummy = container_of(node, struct lfq_node_rcu_dummy, parent);
	dummy->q->queue_call_rcu(&dummy->head, free_dummy_cb);
}

void free_dummy(struct lfq_node_rcu *node)
{
	struct lfq_node_rcu_dummy *dummy;

	if(node->dummy == NULL)
	{
		printk(KERN_INFO "free_dummy : asking to free a NULL ptr");
		BUG();
	}
	dummy = container_of(node, struct lfq_node_rcu_dummy, parent);
	kfree(dummy);
}

 void lfq_node_init_rcu(struct lfq_node_rcu *node)
{
	node->next = NULL;
	node->dummy = 0;
}

 void lfq_init_rcu(struct lfq_queue_rcu *q,
			 void queue_call_rcu(struct rcu_head *head,
					     void (*func)(struct rcu_head *head)))
{
	q->tail = make_dummy(q, NULL);
	q->head = q->tail;
	q->queue_call_rcu = queue_call_rcu;
}

/*
 * The queue should be emptied before calling destroy.
 *
 * Return 0 on success, -EPERM if queue is not empty.
 */
int lfq_destroy_rcu(struct lfq_queue_rcu *q)
{
	struct lfq_node_rcu *head;

	head = rcu_dereference(q->head);
	if (!(head->dummy && head->next == NULL))
		return -EPERM;	/* not empty */
	free_dummy(head);
	return 0;
}

/* 
 * R = cmpxchg(A,C,B) : return value R is equal to C then exchange is done.
 *
 * old_tail = cmpxchg(&cpu_buffer->tail_page,  tail_page, next_page);
 * if (old_tail == tail_page)
 * 	ret = 1;
 *
 */
 
/*
 * Should be called under rcu read lock critical section.
 */
void lockfree_enqueue(struct lfq_queue_rcu *q,
			     struct lfq_node_rcu *node)
{
	/*
	* uatomic_cmpxchg() implicit memory barrier orders earlier stores to
	* node before publication.
	*/

	for (;;) {
		struct lfq_node_rcu *tail, *next;

		tail = rcu_dereference(q->tail);
		next = cmpxchg(&tail->next, NULL, node);
		if (next == NULL) {
			/*
			* Tail was at the end of queue, we successfully
			* appended to it. Now move tail (another
			* enqueue might beat us to it, that's fine).
			*/
			(void)cmpxchg(&q->tail, tail, node);
			return;
		} else {
			/*
			* Failure to append to current tail.
			* Help moving tail further and retry.
			*/
			(void)cmpxchg(&q->tail, tail, next);
			continue;
		}
	}
}

void enqueue_dummy(struct lfq_queue_rcu *q)
{
	struct lfq_node_rcu *node;

	/* We need to reallocate to protect from ABA. */
	node = make_dummy(q, NULL);
	lockfree_enqueue(q, node);
}

/*
 * Should be called under rcu read lock critical section.
 *
 * The caller must wait for a grace period to pass before freeing the returned
 * node or modifying the lfq_node_rcu structure.
 * Returns NULL if queue is empty.
 */
 struct lfq_node_rcu *lockfree_dequeue(struct lfq_queue_rcu *q)
{
	for (;;) {
		struct lfq_node_rcu *head, *next;

		head = rcu_dereference(q->head);
		next = rcu_dereference(head->next);
		if (head->dummy && next == NULL)
			return NULL;	/* empty */
		/*
		* We never, ever allow dequeue to get to a state where
		* the queue is empty (we need at least one node in the
		* queue). This is ensured by checking if the head next
		* is NULL, which means we need to enqueue a dummy node
		* before we can hope dequeuing anything.
		*/
		if (!next) {
			enqueue_dummy(q);
			next = rcu_dereference(head->next);
		}
		if (cmpxchg(&q->head, head, next) != head)
			continue;	/* Concurrently pushed. */
		if (head->dummy) {
			/* Free dummy after grace period. */
			rcu_free_dummy(head);
			continue;	/* try again */
		}
		return head;
	}
}
#if 0
/* have defined it in myftl.c */
void free_cache_num_node(struct rcu_head *head)
{
	struct cache_num_node *node =
			container_of(head, struct cache_num_node, rcu);
	kfree(node);
}	

#endif						 
/* end of lock free queue*/					
