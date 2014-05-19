# Comment/uncomment the following line to enable/disable debugging
# DEBUG = y

ifeq ($(DEBUG),y)
	DEBFLAGS = -O -G -DSCULLPG_DEBUG # "-O" is needed to expand inlines
else
	DEBFLAGS = -O2
endif

EXTRA_CFLAGS += $(DEBFLAGS) -I$(LDDINC)

TARGET = scullpg

ifneq ($(KERNELRELEASE),)

scullpg-objs := main.o mmap.o

obj-m := scullpg.o

else

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD	   := $(shell pwd)

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) LDDINC=$(PWD) modules
endif

install:
	install -d $(INSTALLDIR)
	install -c $(TARGET).o $(INSTALLDIR)

clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions

depend .depend dep:
	$(CC) $(EXTRA_CFLAGS) -M *.c > .depend

ifeq (.depend,$(wildcard .depend))
include .depend
endif
