# 顶层 Makefile
#   make sim    编译纯用户态模拟版（无需 root / 内核头文件），直接运行看闭环
#   make kmd    编译内核模块（需在装有 linux-headers 的虚拟机上执行）
#   make clean  清理产物

.PHONY: all sim umd kmd clean

all: sim

sim:
	$(MAKE) -C umd sim

umd:
	$(MAKE) -C umd umd

kmd:
	$(MAKE) -C kmd

clean:
	$(MAKE) -C umd clean
	$(MAKE) -C kmd clean
