obj-m = my_ftl.o
my_ftl-objs :=  lfq.o lru_list.o my_blkdevs.o myftl.o

all:
	make  -C /lib/modules/`uname -r`/build M=$(PWD) modules

clean:
	make   -C /lib/modules/`uname -r`/build M=$(PWD) clean
	touch my_blkdevs.c myftl.c my_blktrans.h
