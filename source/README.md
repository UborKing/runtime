# 加速卡 Runtime / UMD-KMD 最小骨架

一个对标商用加速器软件栈的最小可运行 Demo，打通 **用户态 Runtime(UMD) ↔ 内核驱动(KMD) ↔ 模拟硬件寄存器/命令队列** 整条链路。
无真实板卡即可在虚拟机上开发验证；代码里保留了接真实 PCIe 设备的完整钩子。

## 1. 目录结构

```
accel-runtime-demo/
├── common/
│   └── accel_ioctl.h      # UMD/KMD 共享 ABI：ioctl 号、数据结构、寄存器/opcode 约定
├── kmd/
│   ├── accel_kmd.c        # 内核态驱动：字符设备、寄存器、命令环形队列、fence、PCI 骨架
│   └── Makefile            # 内核模块构建
├── umd/
│   ├── accel_umd.c        # 用户态 Runtime：open/ioctl 提交命令、等待 fence（对接 KMD）
│   ├── accel_sim.c        # 纯用户态模拟版：无设备/无 root 也能跑通同一套流程
│   └── Makefile
└── Makefile                # 顶层入口
```

## 2. 分层与职责对照

| 层 | 本骨架实现 | 真实加速卡对应物 |
|----|-----------|------------------|
| UMD（用户态 Runtime） | `accel_umd.c`：打开设备、打包命令、提交、等 fence | API 封装、命令流、内存池、流调度、事件/semaphore |
| KMD（内核驱动） | `accel_kmd.c`：字符设备、ioctl 分发、寄存器、环形队列、fence | PCI 枚举、BAR/MMIO、中断、DMA、dma-buf、进程隔离 |
| 硬件（模拟） | 内核内存模拟的寄存器 + 队列完成标记 | BAR 空间 MMIO、硬件命令队列、doorbell、中断 |

### 关键接口（`common/accel_ioctl.h`）

| ioctl | 方向 | 作用 |
|-------|------|------|
| `ACCEL_IOCTL_WRITE_REG` | UMD→KMD | 写 MMIO 寄存器（真实形态写 BAR） |
| `ACCEL_IOCTL_READ_REG`  | UMD→KMD | 读 MMIO 寄存器 |
| `ACCEL_IOCTL_SUBMIT_CMD`| UMD→KMD | 提交命令描述符，返回 fence 序号 |
| `ACCEL_IOCTL_WAIT_FENCE`| UMD→KMD | 阻塞等待某 fence 完成（带超时） |

## 3. 快速验证（当前无 root / 无内核头文件环境）

只编译纯用户态模拟版，跑通队列 + fence 闭环：

```bash
make sim
./umd/accel_sim
```

预期输出：

```
[SIM] device version = 0x00010000
[SIM] status reg readback = 0x00000001
[SIM] submitted cmd seq=1 op=ADD 10+32
[SIM] fence seq=1 completed=1
[SIM] submitted cmd seq=2 op=ADD 11+32
[SIM] fence seq=2 completed=1
[SIM] round-trip OK
```

正式 UMD 用户态程序也可单独编译（仅编译检查，运行需先加载 KMD）：

```bash
make umd
```

## 4. 在 VMware Ubuntu 虚拟机上编译加载 KMD

> 前置：虚拟机有 root、已装对应内核头文件。

### 4.1 安装内核头文件与编译工具

```bash
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r)
```

### 4.2 编译内核模块（虚拟设备形态，默认）

```bash
make kmd
# 或进入 kmd 目录：make
```

生成 `kmd/accel_kmd.ko`。

### 4.3 加载 / 卸载

```bash
sudo insmod kmd/accel_kmd.ko
ls -l /dev/accel0          # 确认设备节点生成
dmesg | tail               # 查看 "accel_kmd: loaded"

sudo ./umd/accel_umd       # 运行正式 UMD

sudo rmmod accel_kmd      # 卸载
```

### 4.4 接真实 PCIe 设备（骨架形态）

代码中 `#ifdef ACCEL_REAL_PCI` 已预留：`pci_driver`、`probe/remove`、`pci_enable_device`、
`pci_request_regions`、`pci_ioremap_bar`、`dma_alloc_coherent`。编译时打开开关：

```bash
make kmd CFLAGS_MODULE=-DACCEL_REAL_PCI
```

把 `accel_pci_ids` 里的 `PCI_DEVICE(0x1234, 0x5678)` 换成真实厂商 ID / 设备 ID。

## 5. QEMU 虚拟 PCIe 设备说明

在没有真实板卡时，可用 QEMU 挂一个自定义/闲置 PCI 设备做联调：

- 本骨架默认走「字符设备 + 内存模拟」形态，**不需要 QEMU 设备即可 insmod**，先验证 UMD/KMD 接口与流程。
- 需要练习真实 `probe/BAR/DMA` 路径时，再用 QEMU 透传或自定义 PCI 设备，并以 `ACCEL_REAL_PCI` 形态编译 KMD。
- 典型联调关注点：BAR 大小与偏移、MMIO 字节序、DMA 地址（CPU 物理地址 vs IOVA）、cache 一致性。

## 6. 已知简化与后续扩展点

骨架为了最小闭环做了简化，下面是对照真实系统的演进方向（也是面试深挖点）：

1. **同步机制**：当前 `WAIT_FENCE` 是短轮询；真实 KMD 用 `wait_queue` + 中断底半（tasklet/threaded irq）唤醒。
2. **命令队列**：当前单队列、满即丢；演进为多队列（多 Stream）、无锁环形缓冲、背压限流。
3. **DMA / 零拷贝**：接入 `dma_buf`、`dma_map_sg`，实现 Host↔Device 零拷贝共享。
4. **内存管理**：设备内存池（buddy/slab）、延迟释放、碎片合并、统一内存缺页迁移。
5. **mmap**：补 `remap_pfn_range`（BAR）/ `dma_mmap_coherent`（DMA 缓冲）实现用户态直接映射。
6. **性能观测**：trace 打点统计命令提交延迟、DMA 起止、队列阻塞时长。

## 7. 面试可讲解的要点

- **UMD 与 KMD 为什么拆分**：用户态做 API/调度/内存池，内核态隔离硬件、受保护资源、中断与 DMA；ioctl 是主通道，mmap 用于高性能零拷贝。
- **doorbell 的作用**：命令入队后写寄存器通知硬件取命令，避免轮询。
- **fence 同步**：每条命令分配序号，硬件完成后更新完成序号；UMD 按序号等待，实现流内/流间依赖。
- **cache 一致性**：DMA coherent 内存无一致性问题；streaming DMA 需 `dma_sync_*` 维护 cache。
- **背压**：队列满时 UMD 阻塞或返回压力，防止 Host 提交过快撑爆设备队列。
