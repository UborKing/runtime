/*
 * accel_ioctl.h - UMD / KMD 共享接口定义
 *
 * 这是用户态 Runtime(UMD) 与内核驱动(KMD) 之间的 ABI 契约：
 *  - ioctl 命令号
 *  - 互通的数据结构
 *  - 寄存器 / 命令 opcode 的语义约定
 *
 * 真实加速卡上：寄存器 = BAR 空间 MMIO；命令 = 写入硬件命令队列；
 * 本骨架在无硬件环境下用内核内存模拟这些行为，接口保持一致。
 */
#ifndef ACCEL_IOCTL_H
#define ACCEL_IOCTL_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#include <linux/ioctl.h>
#endif

#define ACCEL_DEV_NAME    "accel0"
#define ACCEL_CLASS_NAME  "accel"

/* 模拟寄存器数量（对应真实 BAR 空间中的一组 MMIO 寄存器） */
#define ACCEL_REGS_NUM      16
/* 命令队列深度（环形队列） */
#define ACCEL_QUEUE_DEPTH   64

/* ---- 寄存器偏移（真实硬件中固化在 BAR 映射内） ---- */
#define ACCEL_REG_DOORBELL   0x00  /* 写：通知硬件有新命令入队 */
#define ACCEL_REG_FENCE_SEQ  0x04  /* 读：当前已完成到的 fence 序号 */
#define ACCEL_REG_STATUS     0x08  /* 读：设备状态位 */
#define ACCEL_REG_VERSION    0x0c  /* 读：驱动/固件版本 */

/* ---- 命令 opcode（UMD 打包、KMD/硬件执行） ---- */
enum accel_opcode {
    ACCEL_OP_NOP = 0,
    ACCEL_OP_ADD,        /* arg0 + arg1，结果回填 arg1 */
    ACCEL_OP_COPY,       /* 内存拷贝（骨架仅占位） */
    ACCEL_OP_MAX
};

/* 读写单个模拟寄存器 */
struct accel_reg {
    uint32_t offset;
    uint32_t value;      /* READ: 输出；WRITE: 输入 */
};

/* 一条提交给设备的命令描述符 */
struct accel_cmd {
    uint32_t opcode;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t seq;        /* 输出：KMD/硬件分配的 fence 序号 */
};

/* fence 同步等待 */
struct accel_fence {
    uint32_t seq;
    uint32_t timeout_ms;
    uint32_t completed;  /* 输出：1=已完成，0=超时/未完成 */
};

#define ACCEL_IOCTL_BASE  'a'
#define ACCEL_IOCTL_WRITE_REG  _IOW(ACCEL_IOCTL_BASE, 0x01, struct accel_reg)
#define ACCEL_IOCTL_READ_REG   _IOWR(ACCEL_IOCTL_BASE, 0x02, struct accel_reg)
#define ACCEL_IOCTL_SUBMIT_CMD _IOWR(ACCEL_IOCTL_BASE, 0x03, struct accel_cmd)
#define ACCEL_IOCTL_WAIT_FENCE _IOWR(ACCEL_IOCTL_BASE, 0x04, struct accel_fence)

#endif /* ACCEL_IOCTL_H */
