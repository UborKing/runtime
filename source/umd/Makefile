CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2 -std=c11

# accel_umd：对接真实 KMD（ioctl），需在 insmod 后运行
# accel_sim：纯用户态模拟，无设备也能编译运行
all: umd sim

umd: accel_umd.c ../common/accel_ioctl.h
	$(CC) $(CFLAGS) accel_umd.c -o accel_umd

sim: accel_sim.c ../common/accel_ioctl.h
	$(CC) $(CFLAGS) accel_sim.c -o accel_sim

clean:
	rm -f accel_umd accel_sim
