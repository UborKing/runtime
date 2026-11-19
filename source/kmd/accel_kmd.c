/*
 * accel_kmd.c - 加速卡内核态驱动 (KMD) 最小骨架
 *
 * 两种构建形态：
 *   1) 默认「虚拟设备」形态：不依赖真实 PCIe 硬件，注册一个字符设备，
 *      用内核内存模拟 BAR 寄存器与命令队列，便于在普通虚拟机上 insmod 验证链路。
 *   2) 定义 ACCEL_REAL_PCI 后，挂上真实 pci_driver 骨架：probe 中使能设备、
 *      请求 BAR、ioremap 映射寄存器、dma_alloc_coherent 分配一致内存。
 *
 * 真实 KMD 职责对照（本骨架逐一留了钩子与注释）：
 *   - PCI 枚举 / BAR 映射 / MMIO 访问
 *   - 命令队列管理、doorbell、fence/中断同步
 *   - DMA 内存分配、dma-buf 零拷贝（后续扩展点）
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/ioctl.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>

#include "../common/accel_ioctl.h"

#define DRV_NAME "accel_kmd"

/* ------------------------------------------------------------------ */
/* 设备私有状态                                                        */
/* ------------------------------------------------------------------ */
struct accel_dev {
    dev_t devno;
    struct cdev cdev;
    struct class *class;
    struct device *device;

    /* 模拟 MMIO 寄存器（真实场景来自 pci_ioremap_bar 的 BAR 映射） */
    uint32_t regs[ACCEL_REGS_NUM];

    /* 命令环形队列 */
    struct accel_cmd ring[ACCEL_QUEUE_DEPTH];
    uint32_t head;          /* 硬件/KMD 消费位置 */
    uint32_t tail;          /* UMD 提交位置 */
    uint32_t next_seq;      /* 下一个待分配的 fence 序号 */
    uint32_t completed_seq; /* 已完成到的 fence 序号 */

    struct mutex lock;

#ifdef ACCEL_REAL_PCI
    struct pci_dev *pdev;
    void __iomem *bar0;            /* BAR0 映射后的寄存器基址 */
    dma_addr_t dma_handle;          /* 一致内存 DMA 地址 */
    void *dma_cpu_addr;             /* 一致内存 CPU 侧地址 */
#endif
};

static struct accel_dev *g_dev;

/* ------------------------------------------------------------------ */
/* 寄存器读写（虚拟形态：直接操作内存；真实形态：ioread32/iowrite32）    */
/* ------------------------------------------------------------------ */
static uint32_t accel_reg_read(struct accel_dev *dev, uint32_t offset)
{
    if (offset >= ACCEL_REGS_NUM * 4)
        return 0;
#ifdef ACCEL_REAL_PCI
    return ioread32(dev->bar0 + offset);
#else
    return dev->regs[offset / 4];
#endif
}

static void accel_reg_write(struct accel_dev *dev, uint32_t offset, uint32_t value)
{
    if (offset >= ACCEL_REGS_NUM * 4)
        return;
#ifdef ACCEL_REAL_PCI
    iowrite32(value, dev->bar0 + offset);
#else
    dev->regs[offset / 4] = value;
#endif
}

/* ------------------------------------------------------------------ */
/* 命令处理（骨架：入队后立即模拟完成；真实场景由硬件执行 + 中断回收） */
/* ------------------------------------------------------------------ */
static uint32_t accel_submit(struct accel_dev *dev, struct accel_cmd *cmd)
{
    uint32_t seq;

    mutex_lock(&dev->lock);
    seq = dev->next_seq++;
    cmd->seq = seq;

    /* 环形队列满则丢弃（真实 KMD 会阻塞/背压，见 README 调度章节） */
    if ((dev->tail + 1) % ACCEL_QUEUE_DEPTH == dev->head) {
        mutex_unlock(&dev->lock);
        return 0;
    }

    dev->ring[dev->tail] = *cmd;
    dev->tail = (dev->tail + 1) % ACCEL_QUEUE_DEPTH;

    /* 模拟「硬件执行完成」：真实路径里由中断底半/tasklet 更新 completed_seq，
     * 并唤醒等待 fence 的进程（wait_queue）。 */
    dev->completed_seq = seq;
    mutex_unlock(&dev->lock);

    /* doorbell：写寄存器通知设备有新命令（真实形态写 BAR 的 doorbell 位） */
    accel_reg_write(dev, ACCEL_REG_DOORBELL, seq);
    return seq;
}

static int accel_wait_fence(struct accel_dev *dev, struct accel_fence *fence)
{
    uint32_t elapsed = 0;

    while (elapsed < fence->timeout_ms) {
        mutex_lock(&dev->lock);
        if (dev->completed_seq >= fence->seq) {
            mutex_unlock(&dev->lock);
            fence->completed = 1;
            return 0;
        }
        mutex_unlock(&dev->lock);

        /* 真实 KMD：wait_event_interruptible_timeout 进入睡眠，由中断唤醒；
         * 骨架用短轮询演示同步语义。 */
        msleep(1);
        elapsed++;
    }
    fence->completed = 0;
    return -ETIMEDOUT;
}

/* ------------------------------------------------------------------ */
/* file_operations / ioctl 分发                                        */
/* ------------------------------------------------------------------ */
static long accel_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct accel_dev *dev = filp->private_data;
    int ret = 0;

    switch (cmd) {
    case ACCEL_IOCTL_WRITE_REG: {
        struct accel_reg r;
        if (copy_from_user(&r, (void __user *)arg, sizeof(r)))
            return -EFAULT;
        accel_reg_write(dev, r.offset, r.value);
        break;
    }
    case ACCEL_IOCTL_READ_REG: {
        struct accel_reg r;
        if (copy_from_user(&r, (void __user *)arg, sizeof(r)))
            return -EFAULT;
        r.value = accel_reg_read(dev, r.offset);
        if (copy_to_user((void __user *)arg, &r, sizeof(r)))
            return -EFAULT;
        break;
    }
    case ACCEL_IOCTL_SUBMIT_CMD: {
        struct accel_cmd c;
        if (copy_from_user(&c, (void __user *)arg, sizeof(c)))
            return -EFAULT;
        if (c.opcode >= ACCEL_OP_MAX)
            return -EINVAL;
        if (!accel_submit(dev, &c))
            return -ENOSPC;
        if (copy_to_user((void __user *)arg, &c, sizeof(c)))
            return -EFAULT;
        break;
    }
    case ACCEL_IOCTL_WAIT_FENCE: {
        struct accel_fence f;
        if (copy_from_user(&f, (void __user *)arg, sizeof(f)))
            return -EFAULT;
        ret = accel_wait_fence(dev, &f);
        if (copy_to_user((void __user *)arg, &f, sizeof(f)))
            return -EFAULT;
        break;
    }
    default:
        return -ENOTTY;
    }
    return ret;
}

static int accel_open(struct inode *inode, struct file *filp)
{
    filp->private_data = g_dev;
    return 0;
}

static int accel_release(struct inode *inode, struct file *filp)
{
    filp->private_data = NULL;
    return 0;
}

/*
 * mmap 骨架：把设备寄存器/缓冲区映射到用户态。
 * 真实场景：BAR 空间用 remap_pfn_range(BAR 物理页)；
 *           DMA 缓冲用 dma_mmap_coherent；dma-buf 走 mmap 回调。
 * 虚拟形态下此处仅占位，UMD 通过 ioctl 访问寄存器即可。
 */
static int accel_mmap(struct file *filp, struct vm_area_struct *vma)
{
    /* TODO(real hw): remap_pfn_range / dma_mmap_coherent */
    return -ENOSYS;
}

static struct file_operations accel_fops = {
    .owner          = THIS_MODULE,
    .open           = accel_open,
    .release        = accel_release,
    .unlocked_ioctl = accel_ioctl,
    .mmap           = accel_mmap,
};

/* ------------------------------------------------------------------ */
/* 真实 PCIe 驱动骨架（ACCEL_REAL_PCI 时启用）                          */
/* ------------------------------------------------------------------ */
#ifdef ACCEL_REAL_PCI
static const struct pci_device_id accel_pci_ids[] = {
    { PCI_DEVICE(0x1234, 0x5678) },   /* 替换为真实厂商/设备 ID */
    { 0, }
};
MODULE_DEVICE_TABLE(pci, accel_pci_ids);

static int accel_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct accel_dev *dev = g_dev;
    int ret;

    ret = pci_enable_device(pdev);
    if (ret)
        return ret;
    pci_set_master(pdev);

    ret = pci_request_regions(pdev, DRV_NAME);
    if (ret)
        goto err_disable;

    /* 映射 BAR0：之后寄存器全部走 ioread32/iowrite32 */
    dev->bar0 = pci_ioremap_bar(pdev, 0);
    if (!dev->bar0) {
        ret = -ENOMEM;
        goto err_release;
    }

    /* 分配 DMA 一致内存（无 cache 一致性问题，适合命令描述符） */
    dev->dma_cpu_addr = dma_alloc_coherent(&pdev->dev, PAGE_SIZE,
                                           &dev->dma_handle, GFP_KERNEL);
    if (!dev->dma_cpu_addr) {
        ret = -ENOMEM;
        goto err_unmap;
    }

    dev->pdev = pdev;
    pci_set_drvdata(pdev, dev);
    return 0;

err_unmap:
    iounmap(dev->bar0);
err_release:
    pci_release_regions(pdev);
err_disable:
    pci_disable_device(pdev);
    return ret;
}

static void accel_pci_remove(struct pci_dev *pdev)
{
    struct accel_dev *dev = pci_get_drvdata(pdev);

    if (dev->dma_cpu_addr)
        dma_free_coherent(&pdev->dev, PAGE_SIZE,
                         dev->dma_cpu_addr, dev->dma_handle);
    if (dev->bar0)
        iounmap(dev->bar0);
    pci_release_regions(pdev);
    pci_disable_device(pdev);
}

static struct pci_driver accel_pci_driver = {
    .name     = DRV_NAME,
    .id_table = accel_pci_ids,
    .probe    = accel_pci_probe,
    .remove   = accel_pci_remove,
};
#endif /* ACCEL_REAL_PCI */

/* ------------------------------------------------------------------ */
/* 模块初始化 / 退出                                                    */
/* ------------------------------------------------------------------ */
static int __init accel_kmd_init(void)
{
    int ret;

    g_dev = kzalloc(sizeof(*g_dev), GFP_KERNEL);
    if (!g_dev)
        return -ENOMEM;

    mutex_init(&g_dev->lock);
    g_dev->next_seq = 1;
    g_dev->completed_seq = 0;

    /* 版本寄存器占位 */
    g_dev->regs[ACCEL_REG_VERSION / 4] = 0x00010000;

    ret = alloc_chrdev_region(&g_dev->devno, 0, 1, ACCEL_DEV_NAME);
    if (ret)
        goto err_free;

    cdev_init(&g_dev->cdev, &accel_fops);
    g_dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&g_dev->cdev, g_dev->devno, 1);
    if (ret)
        goto err_unreg;

    g_dev->class = class_create(THIS_MODULE, ACCEL_CLASS_NAME);
    if (IS_ERR(g_dev->class)) {
        ret = PTR_ERR(g_dev->class);
        goto err_cdev;
    }

    g_dev->device = device_create(g_dev->class, NULL, g_dev->devno,
                                   NULL, ACCEL_DEV_NAME);
    if (IS_ERR(g_dev->device)) {
        ret = PTR_ERR(g_dev->device);
        goto err_class;
    }

#ifdef ACCEL_REAL_PCI
    ret = pci_register_driver(&accel_pci_driver);
    if (ret)
        goto err_device;
#endif

    pr_info(DRV_NAME ": loaded, dev=%s\n", ACCEL_DEV_NAME);
    return 0;

#ifdef ACCEL_REAL_PCI
err_device:
    device_destroy(g_dev->class, g_dev->devno);
#endif
err_class:
    class_destroy(g_dev->class);
err_cdev:
    cdev_del(&g_dev->cdev);
err_unreg:
    unregister_chrdev_region(g_dev->devno, 1);
err_free:
    kfree(g_dev);
    return ret;
}

static void __exit accel_kmd_exit(void)
{
#ifdef ACCEL_REAL_PCI
    pci_unregister_driver(&accel_pci_driver);
#endif
    device_destroy(g_dev->class, g_dev->devno);
    class_destroy(g_dev->class);
    cdev_del(&g_dev->cdev);
    unregister_chrdev_region(g_dev->devno, 1);
    kfree(g_dev);
    pr_info(DRV_NAME ": unloaded\n");
}

module_init(accel_kmd_init);
module_exit(accel_kmd_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("accel-runtime-demo");
MODULE_DESCRIPTION("Minimal KMD skeleton for an accelerator device");
MODULE_VERSION("0.1");
