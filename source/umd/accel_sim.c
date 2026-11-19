/*
 * accel_sim.c - 纯用户态模拟版（不依赖内核设备 / root / 内核头文件）
 *
 * 目的：在没有真实加速卡、也无法 insmod 的环境里，先把 UMD 的调用链路
 * 与命令队列 / fence 同步逻辑跑通，验证数据结构与流程闭环。
 * 它复刻了 accel_kmd.c 里的队列与 fence 语义；等进入虚拟机加载 KMD 后，
 * 同一套 UMD 流程改走 ioctl（见 accel_umd.c）。
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../common/accel_ioctl.h"

struct sim_dev {
    uint32_t regs[ACCEL_REGS_NUM];
    struct accel_cmd ring[ACCEL_QUEUE_DEPTH];
    uint32_t head, tail, next_seq, completed_seq;
};

static struct sim_dev d;

static uint32_t sim_submit(struct accel_cmd *cmd)
{
    uint32_t seq = d.next_seq++;
    cmd->seq = seq;

    if ((d.tail + 1) % ACCEL_QUEUE_DEPTH == d.head)
        return 0;

    d.ring[d.tail] = *cmd;
    d.tail = (d.tail + 1) % ACCEL_QUEUE_DEPTH;

    /* 模拟硬件执行完成（真实路径由中断回收） */
    d.completed_seq = seq;
    d.regs[ACCEL_REG_DOORBELL / 4] = seq;
    return seq;
}

static int sim_wait_fence(struct accel_fence *f)
{
    if (d.completed_seq >= f->seq) {
        f->completed = 1;
        return 0;
    }
    f->completed = 0;
    return -1;
}

int main(void)
{
    memset(&d, 0, sizeof(d));
    d.next_seq = 1;
    d.regs[ACCEL_REG_VERSION / 4] = 0x00010000;

    printf("[SIM] device version = 0x%08x\n",
           d.regs[ACCEL_REG_VERSION / 4]);

    d.regs[ACCEL_REG_STATUS / 4] = 0x1;
    printf("[SIM] status reg readback = 0x%08x\n",
           d.regs[ACCEL_REG_STATUS / 4]);

    for (int i = 0; i < 2; i++) {
        struct accel_cmd cmd = {
            .opcode = ACCEL_OP_ADD,
            .arg0 = 10 + i,
            .arg1 = 32,
        };
        uint32_t seq = sim_submit(&cmd);
        if (!seq) {
            fprintf(stderr, "[SIM] queue full\n");
            return 1;
        }
        printf("[SIM] submitted cmd seq=%u op=ADD %u+%u\n",
               cmd.seq, cmd.arg0, cmd.arg1);

        struct accel_fence f = { .seq = cmd.seq, .timeout_ms = 1000 };
        sim_wait_fence(&f);
        printf("[SIM] fence seq=%u completed=%u\n", f.seq, f.completed);
    }

    printf("[SIM] round-trip OK\n");
    return 0;
}
