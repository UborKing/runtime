/*
 * accel_umd.c - 用户态 Runtime (UMD) 最小骨架
 *
 * 对接 KMD：open 设备节点 -> 读写寄存器 -> 提交命令 -> fence 等待。
 * 真实 Runtime 中这一层还会包含：命令流封装、内存池、流(stream)调度、
 * 事件/semaphore、算子接口等；本文件只打通最底层链路。
 *
 * 编译：见 umd/Makefile（accel_umd 目标）
 * 运行：sudo ./accel_umd   （需先 insmod accel_kmd.ko）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

#include "../common/accel_ioctl.h"

#define DEV_PATH "/dev/" ACCEL_DEV_NAME

int main(void)
{
    int fd = open(DEV_PATH, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", DEV_PATH, strerror(errno));
        return 1;
    }

    /* 1) 读版本寄存器 */
    struct accel_reg ver = { .offset = ACCEL_REG_VERSION };
    if (ioctl(fd, ACCEL_IOCTL_READ_REG, &ver) < 0) {
        perror("read version");
        goto out;
    }
    printf("[UMD] device version = 0x%08x\n", ver.value);

    /* 2) 写一个状态寄存器再读回，验证 MMIO 通路 */
    struct accel_reg w = { .offset = ACCEL_REG_STATUS, .value = 0x1 };
    if (ioctl(fd, ACCEL_IOCTL_WRITE_REG, &w) < 0) {
        perror("write reg");
        goto out;
    }
    struct accel_reg r = { .offset = ACCEL_REG_STATUS };
    ioctl(fd, ACCEL_IOCTL_READ_REG, &r);
    printf("[UMD] status reg readback = 0x%08x\n", r.value);

    /* 3) 提交两条 ADD 命令，各自等待 fence */
    for (int i = 0; i < 2; i++) {
        struct accel_cmd cmd = {
            .opcode = ACCEL_OP_ADD,
            .arg0 = 10 + i,
            .arg1 = 32,
        };
        if (ioctl(fd, ACCEL_IOCTL_SUBMIT_CMD, &cmd) < 0) {
            perror("submit cmd");
            goto out;
        }
        printf("[UMD] submitted cmd seq=%u op=ADD %u+%u\n",
               cmd.seq, cmd.arg0, cmd.arg1);

        struct accel_fence f = { .seq = cmd.seq, .timeout_ms = 1000 };
        if (ioctl(fd, ACCEL_IOCTL_WAIT_FENCE, &f) < 0) {
            perror("wait fence");
            goto out;
        }
        printf("[UMD] fence seq=%u completed=%u\n", f.seq, f.completed);
    }

    printf("[UMD] round-trip OK\n");
out:
    close(fd);
    return 0;
}
