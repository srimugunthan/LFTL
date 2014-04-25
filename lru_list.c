
#include "my_blktrans.h"







/* usage:



	list_lru_init(&my_lru);
	for(i=5; i!=0; --i){
		tmp= (struct my_list *)kmalloc(sizeof(struct my_list), GFP_KERNEL);
		tmp->to = i;
		list_lru_add(&my_lru,  &(tmp->list));
	}

	list_for_each(pos, &my_lru.list){
		tmp= list_entry(pos, struct my_list, list);
		
		printk(KERN_INFO "to= %d \n", tmp->to);
		if(tmp->to == 1)
			tmp_hd = &(tmp->list);
			
	}
	printk(KERN_INFO " ");

	list_lru_touch(&my_lru,( tmp_hd));
	printk(KERN_INFO " ");
	list_for_each(pos, &my_lru.list){
		tmp= list_entry(pos, struct my_list, list);
		
		printk(KERN_INFO "to= %d \n", tmp->to);

			
	}
	printk(KERN_INFO " ");


	while((tmp = list_lru_deqhd(&my_lru)) != NULL)
	{
		printk(KERN_INFO "to= %d \n", tmp->to);
		kfree(tmp);
	}
	printk(KERN_INFO "lru count = %d", my_lru.nr_items);
*/


int list_lru_init(struct list_lru *lru)
{
	spin_lock_init(&lru->lock);
	INIT_LIST_HEAD(&lru->list);
	lru->nr_items = 0;

	return 0;
}



struct cache_buf_list *list_lru_del(struct list_lru *lru,
				    struct list_head *item)
{
	struct cache_buf_list *tmp = NULL;
	
	//if (!list_empty(item))
	{
		tmp = list_entry(item,	struct cache_buf_list,list);
		list_del(item);
		lru->nr_items--;
		
		return tmp;
	}
	
	return tmp;
	
}




int  list_lru_add(struct list_lru *lru,
		  struct list_head *item)
{
	
	list_add_tail(item, &lru->list);
	lru->nr_items++;

	
	return 0;
}





struct cache_buf_list *list_lru_deqhd(struct list_lru *lru)
{
	struct cache_buf_list *tmp = NULL;
//	spin_lock(&lru->lock);
	if (!list_empty(&lru->list)) {
		tmp= list_first_entry(&lru->list, struct cache_buf_list, list);
		list_del(&tmp->list);
		lru->nr_items--;

	}
//	spin_unlock(&lru->lock);
	return tmp;
}


void list_lru_touch(struct list_lru *lru,  struct list_head *item)
{

	spin_lock(&lru->lock);

	list_move_tail(item, &lru->list);
	spin_unlock(&lru->lock);
	
	
}
