# 内核模块 Makefile
# 在装有对应内核头文件的机器上执行：make
#
# 真实 PCIe 形态（挂上 pci_driver 骨架）：
#   make CFLAGS_MODULE=-DACCEL_REAL_PCI

obj-m += accel_kmd.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules $(CFLAGS_MODULE)

clean:
	@if [ -d $(KERNELDIR) ]; then \
		$(MAKE) -C $(KERNELDIR) M=$(PWD) clean; \
	else \
		rm -f *.ko *.o *.mod *.mod.c .*.cmd *.order *.symvers; \
		echo "headers missing, cleaned local artifacts only"; \
	fi
