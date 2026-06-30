# qemu-system-loongarch32 -M ciciec_soc 使用说明

## 概述

`ciciec_soc` 是 QEMU LA32R 分支中新增的机器类型，模拟 Ciciec2026 的 SoC。

该机器仅包含运行 bare-metal 程序所需的最基本外设：SRAM 和 16550 UART。适用于 SDK 中 `ciciec2026_loongson_preliminary/sdk/software/examples/` 下的测试程序。

## 内存布局

| 起始地址 | 大小 | 用途 |
|-----------|------|------|
| `0x1c000000` | 1 MB | SRAM（覆盖 linker script 中的 isram 512KB + dsram 512KB） |
| `0x1f000000` | 8 B | 16550 UART 寄存器 |

## 地址翻译

CPU 复位后处于**直接地址翻译模式**（DA=1, PG=0, CRMD=0xa8），此时 VA == PA，所有访存直接对应物理地址。

SDK 的 `start.S` 会在启动过程中配置 DMW 并切换到页模式：

| 窗口 | 值 | 映射 |
|------|-----|------|
| DMW0 | `0x00000019` | VA `0x00000000`-`0x1fffffff` → PA `0x00000000`-`0x1fffffff`（缓存） |
| DMW1 | `0xa0000009` | VA `0xa0000000`-`0xbfffffff` → PA `0x00000000`-`0x1fffffff`（非缓存） |

SDK 中 UART_BASE 定义为 `0xbf000000`（VA），通过 DMW1 映射到物理地址 `0x1f000000`。

## 编译 QEMU

```bash
cd la32r-QEMU
mkdir build && cd build
../configure --target-list=loongarch32-softmmu --disable-werror --enable-debug \
    --extra-cflags="-fpermissive"
make -j$(nproc)
```

## 运行

```bash
./build/qemu-system-loongarch32 -M ciciec_soc \
    -kernel /path/to/program.elf \
    -serial mon:stdio -nographic
```

## 16550 UART 寄存器

16550 标准寄存器布局，地址偏移如下（基址 `0x1f000000`，regshift=0）：

| 偏移 | 寄存器 | 说明 |
|------|--------|------|
| 0 | THR/RBR/DLL | 发送保持寄存器 / 接收缓冲寄存器 / 除数锁存 LSB |
| 1 | IER/DLM | 中断使能寄存器 / 除数锁存 MSB |
| 2 | IIR/FCR | 中断识别寄存器 / FIFO 控制寄存器 |
| 3 | LCR | 线路控制寄存器（DLAB=bit7） |
| 4 | MCR | 调制解调器控制寄存器 |
| 5 | LSR | 线路状态寄存器（bit5=THRE 发送就绪，bit0=DR 接收就绪） |
| 6 | MSR | 调制解调器状态寄存器 |

## 直接操作 UART 输出示例

```c
#define UART_BASE 0xbf000000  // VA via DMW1; or 0x1f000000 in direct mode

void uart_putc(char c) {
    // Wait for THRE (LSR bit 5 at offset 5)
    while (!(*(volatile unsigned char *)(UART_BASE + 5) & 0x20));
    *(volatile unsigned char *)UART_BASE = c;
}

void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}
```
