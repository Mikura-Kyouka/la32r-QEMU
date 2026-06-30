# qemu-system-loongarch32 for FPGA 使用说明

------

## 概述

`la32_fpga` 机器类型模拟一个运行在 FPGA 板上的 LoongArch32 Reduced（LA32R）CPU，适用于裸机程序及监控程序（supervisor）调试。该机器类型**无 bootloader**，CPU 上电后直接从 BaseRAM 启动，使用直接地址翻译模式（DA=1, PG=0），虚拟地址等于物理地址。

### 内存布局

| 地址范围 | 大小 | 说明 |
| -------- | :--: | :--- |
| 0x80000000 - 0x803fffff | 4 MB | BaseRAM（R/W） |
| 0x80400000 - 0x807fffff | 4 MB | ExtRAM（R/W） |
| 0xBFD003F8 - 0xBFD003FF | 8 B  | FPGA UART（MMIO） |

### FPGA UART 寄存器

| 偏移 | 访问 | 说明 |
| ---- | :--: | :--- |
| 0x0 (0xBFD003F8) | R/W | 数据寄存器：写 = 发送字节，读 = 接收字节 |
| 0x4 (0xBFD003FC) | R   | 状态寄存器：bit 0 = TX idle（始终为 1），bit 1 = RX ready |

UART 无中断引脚，需轮询状态寄存器。通过 QEMU 的 `-serial` 参数连接至主机终端、文件或 TCP socket。

### 地址翻译模式

FPGA 板使用**直接地址翻译模式**（CRMD.DA=1, CRMD.PG=0），VA == PA。复位后 CRMD 为 `0xa8`（DA=1, PG=0, DATF=1, DATM=1, PLV=0, IE=0），不配置 DMW。

## 编译

### 编译 QEMU

```bash
cd la32r-QEMU
mkdir build && cd build
../configure --target-list=loongarch32-softmmu --disable-werror --enable-debug \
    --disable-linux-io-uring --disable-bpf \
    --extra-cflags="-Wno-overflow -fpermissive" --disable-xkbcommon

# 如果系统已安装 qemu-keymap，需要先想办法临时隐藏，否则 keymap 生成可能失败

make -j$(nproc)
```

> `-fpermissive` 是因为 GCC 14+ 将 `-Wincompatible-pointer-types` 升级为错误；旧版 GCC 上不需要此选项。
> `-Wno-overflow` 抑制部分 64 位常量在 32 位编译时的溢出警告。
> `--disable-linux-io-uring` 和 `--disable-bpf` 是因为测试环境宿主内核头文件不全。
> `--disable-xkbcommon` 避免 VNC/图形相关依赖。

编译完成后，可执行文件位于 `build/qemu-system-loongarch32`。

### 编译监控程序（Kernel）

```bash
cd supervisor-compiled/kernel

# 确认 Makefile 中 ON_FPGA=y（默认即为 y，使用 -DMACH_FPGA）
# 编译
make

# 产物：kernel.elf（ELF 可执行文件）、kernel.bin（二进制镜像）
```

> 需要 LoongArch32R 工具链。下载地址：
> <https://gitee.com/loongson-edu/la32r-toolchains/releases/tag/v0.0.2>
>
> 解压后设置环境变量：
> `export GCCPREFIX=/path/to/loongarch32r-linux-gnusf/bin/loongarch32r-linux-gnusf-`

## 运行

### 终端模式（直接交互）

```bash
./build/qemu-system-loongarch32 \
    -M la32_fpga \
    -kernel /path/to/supervisor-compiled/kernel/kernel.elf \
    -nographic \
    -serial mon:stdio
```

启动后将直接看到监控程序输出：

```
MONITOR for Loongarch32 - initialized.
```

此时监控程序在等待串口命令输入。Term 命令（R/D/A/G/U/F/T/Q）可以直接在终端中输入。

> **注意**：`-serial mon:stdio` 将串口和 QEMU monitor 复用在同一终端。输入 QEMU monitor 命令需按 `Ctrl-A C` 切换。如果不需要 monitor，可用 `-serial stdio -monitor none`。

### 搭配 Term 程序使用

Term 是监控程序的上位机交互程序（Python），提供更友好的用户界面。Term 通过 TCP socket 与 QEMU 的串口通信。

**方式一：socat 桥接（推荐）**

```bash
# 终端 1 — 启动 QEMU，串口使用 Unix socket
./build/qemu-system-loongarch32 \
    -M la32_fpga \
    -kernel /path/to/supervisor-compiled/kernel/kernel.elf \
    -nographic \
    -serial unix:/tmp/qemu-serial.sock,server=on,wait=on \
    -monitor none

# 终端 2 — 用 socat 将 Unix socket 转成 TCP
socat TCP-LISTEN:6666,reuseaddr,fork UNIX-CONNECT:/tmp/qemu-serial.sock

# 终端 3 — 运行 Term 连接
cd supervisor-compiled/term
python3 term.py -t 127.0.0.1:6666
```

> `server=on,wait=on` 使 QEMU 等待客户端连接后才开始执行，保证不会丢失初始输出。

**方式二：QEMU 内置 TCP server**

```bash
# 终端 1 — 启动 QEMU，串口直接监听 TCP
./build/qemu-system-loongarch32 \
    -M la32_fpga \
    -kernel /path/to/supervisor-compiled/kernel/kernel.elf \
    -nographic \
    -serial tcp:127.0.0.1:6666,server=on,wait=on \
    -monitor none

# 终端 2 — 运行 Term 连接
cd supervisor-compiled/term
python3 term.py -t 127.0.0.1:6666
```

> `wait=on` 确保 QEMU 等待 Term 连接后再启动，不会丢失 "MONITOR for Loongarch32 - initialized." 的欢迎信息。

### GDB 调试模式

```bash
./build/qemu-system-loongarch32 \
    -M la32_fpga \
    -kernel /path/to/supervisor-compiled/kernel/kernel.elf \
    -nographic \
    -serial mon:stdio \
    -S -gdb tcp::5295
```

在另一个终端中，使用 LoongArch32R GDB 连接：

```
(gdb) target remote :5295
(gdb) b START
(gdb) c
```

### 加载裸机二进制程序

如果程序没有 ELF 头（纯二进制），可以通过环境变量 `BOOTROM` 指定加载地址：

```bash
BOOTROM=0x80000000 ./build/qemu-system-loongarch32 \
    -M la32_fpga \
    -kernel /path/to/baremetal.bin \
    -nographic \
    -serial mon:stdio
```

### 命令行参数

| 参数 | 说明 |
| ---- | :--- |
| `-M la32_fpga` | 选择 FPGA 机器类型（必选） |
| `-kernel <file>` | 加载内核/裸机程序（ELF 或二进制） |
| `-nographic` | 无图形输出 |
| `-serial mon:stdio` | 串口连接至终端 stdio，同时复用 QEMU monitor |
| `-serial stdio -monitor none` | 串口独占 stdio，禁用 monitor |
| `-serial tcp:<ip>:<port>,server=on,wait=on` | 串口监听 TCP，等待客户端连接 |
| `-serial unix:<path>,server=on,wait=on` | 串口监听 Unix socket |
| `-serial file:<path>` | 串口输出写入文件（无法交互输入） |
| `-gdb tcp:<port>` | 开启 GDB 远程调试 |
| `-S` | 启动时不自动运行，等待 GDB 连接 |
| `-d int -D int.log` | 记录中断/异常调试信息到文件 |

## 监控程序（Term）命令

连接 Term 后，支持以下命令：

| 命令 | 功能 |
| ---- | :--- |
| `R` | 显示用户程序寄存器值（\$r1 至 \$r31） |
| `D` | 显示从指定地址开始的内存数据 |
| `A` | 逐行输入汇编指令或数据，写入指定地址 |
| `F` | 从文件读入汇编指令或数据，写入指定地址 |
| `U` | 从指定地址反汇编指定数量的指令 |
| `G` | 从指定地址开始执行用户程序 |
| `T` | 查看指定 TLB 条目（需 Kernel 支持 TLB） |
| `Q` | 退出 Term |

### 用户程序示例

Term 中依次输入以下命令：

```
>> a
>>addr: 0x80100000
one instruction per line, empty line to end.
[0x80100000] ori $r4,$r0,5
[0x80100004] xor $r12,$r12,$r12
[0x80100008] xor $r13,$r13,$r13
[0x8010000c] add.w $r13,$r13,$r12
[0x80100010] addi.w $r12,$r12,1
[0x80100014] bne $r4,$r12,-8
[0x80100018] jr $r1
[0x8010001c]
>> g
>>addr: 0x80100000
```

运行结束后用 `R` 查看寄存器，`$r13` 应为 `0x0000000a`（即 1+2+3+4=10）。

用户程序内存空间：

| 地址范围 | 用途 |
| -------- | :--- |
| 0x80100000 - 0x803FFFFF | 用户代码区 |
| 0x80400000 - 0x807EFFFF | 用户数据区 |

## 注意事项

- `-M la32_fpga` 仅支持**单核**（`max_cpus = 1`），不支持 `-smp` 参数
- 无 BIOS ROM，复位向量为 `0x80000000`（BaseRAM 起始地址）
- 无 SD 卡、无网络设备、无中断控制器、无 PCI 总线
- CPU 始终运行在直接地址翻译模式下（VA == PA）
- 监控程序 FPGA 版本（`ON_FPGA=y`）使用 `-DMACH_FPGA` 编译，与标准版本不兼容
- 串口通信无硬件流控，依赖轮询状态寄存器
- 编译时如果 `qemu-keymap` 导致 `pc-bios/keymaps/ar` 生成失败，用空 stub 替代或临时隐藏系统 `qemu-keymap`

## 构建环境说明

本文档基于以下环境验证（AOSC OS on LoongArch64，GCC 15.3.0）：

```bash
../configure --target-list=loongarch32-softmmu --disable-werror --enable-debug \
    --disable-linux-io-uring --disable-bpf \
    --extra-cflags="-Wno-overflow -fpermissive" --disable-xkbcommon
make -j$(nproc)
```
