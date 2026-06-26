/*
 * LoongArch32 FPGA board emulation.
 *
 * Memory layout:
 *   0x80000000 - 0x803fffff  BaseRAM (4 MB)
 *   0x80400000 - 0x807fffff  ExtRAM  (4 MB)
 *
 * Peripherals:
 *   FPGA UART at 0xBFD003F8 (data) / 0xBFD003FC (status)
 *
 * Copyright (C) 2024 Loongson Technology Corporation Limited
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "elf.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/sysbus.h"
#include "chardev/char-fe.h"
#include "exec/address-spaces.h"
#include "sysemu/reset.h"
#include "sysemu/sysemu.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qemu/cutils.h"
#include "cpu.h"

/* ===================================================================== */
/*  FPGA UART Device                                                      */
/* ===================================================================== */

#define TYPE_FPGA_UART "fpga-uart"
OBJECT_DECLARE_SIMPLE_TYPE(FpgaUartState, FPGA_UART)

struct FpgaUartState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    CharBackend chr;

    uint8_t data;       /* data register (offset 0) */
    uint8_t status;     /* status register (offset 4):
                         *   bit 0: TX idle (always 1 in this model)
                         *   bit 1: RX ready (1 = data received) */
};

#define UART_ST_TX_IDLE   (1 << 0)
#define UART_ST_RX_READY  (1 << 1)

static uint64_t fpga_uart_read(void *opaque, hwaddr addr, unsigned size)
{
    FpgaUartState *s = opaque;

    switch (addr) {
    case 0: /* data register — receive byte */
        s->status &= ~UART_ST_RX_READY;
        qemu_chr_fe_accept_input(&s->chr);
        return s->data;
    case 4: /* status register */
        return s->status;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "fpga-uart: read at invalid offset 0x%" HWADDR_PRIx "\n",
                      addr);
        return 0;
    }
}

static void fpga_uart_write(void *opaque, hwaddr addr,
                            uint64_t value, unsigned size)
{
    FpgaUartState *s = opaque;
    uint8_t ch = value & 0xff;

    switch (addr) {
    case 0: /* data register — transmit byte */
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
        break;
    case 4: /* status register — read-only, ignore writes */
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "fpga-uart: write at invalid offset 0x%" HWADDR_PRIx "\n",
                      addr);
        break;
    }
}

static const MemoryRegionOps fpga_uart_ops = {
    .read = fpga_uart_read,
    .write = fpga_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static int fpga_uart_can_receive(void *opaque)
{
    FpgaUartState *s = opaque;
    return !(s->status & UART_ST_RX_READY);
}

static void fpga_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    FpgaUartState *s = opaque;

    s->data = *buf;
    s->status |= UART_ST_RX_READY;
}

static void fpga_uart_event(void *opaque, QEMUChrEvent event)
{
}

static void fpga_uart_reset(DeviceState *dev)
{
    FpgaUartState *s = FPGA_UART(dev);

    s->data = 0;
    s->status = UART_ST_TX_IDLE;
}

static void fpga_uart_realize(DeviceState *dev, Error **errp)
{
    FpgaUartState *s = FPGA_UART(dev);

    qemu_chr_fe_set_handlers(&s->chr,
                             fpga_uart_can_receive,
                             fpga_uart_receive,
                             fpga_uart_event,
                             NULL, s, NULL, true);
}

static void fpga_uart_init(Object *obj)
{
    FpgaUartState *s = FPGA_UART(obj);

    memory_region_init_io(&s->mmio, obj, &fpga_uart_ops, s,
                          TYPE_FPGA_UART, 0x8);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static Property fpga_uart_properties[] = {
    DEFINE_PROP_CHR("chardev", FpgaUartState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void fpga_uart_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = fpga_uart_realize;
    dc->reset = fpga_uart_reset;
    device_class_set_props(dc, fpga_uart_properties);
}

static const TypeInfo fpga_uart_info = {
    .name = TYPE_FPGA_UART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(FpgaUartState),
    .instance_init = fpga_uart_init,
    .class_init = fpga_uart_class_init,
};

/* ===================================================================== */
/*  FPGA Machine                                                          */
/* ===================================================================== */

#define FPGA_BASERAM_BASE  0x80000000ULL
#define FPGA_BASERAM_SIZE  (4 * MiB)
#define FPGA_EXTRAM_BASE   0x80400000ULL
#define FPGA_EXTRAM_SIZE   (4 * MiB)
#define FPGA_UART_BASE     0xBFD003F8ULL

static void la32_cpu_set_irq(void *opaque, int irq, int level)
{
    LoongArchCPU *cpu = opaque;
    CPULoongArchState *env = &cpu->env;
    CPUState *cs = CPU(cpu);

    if (irq < 0 || irq > N_IRQS) {
        return;
    }
    if (level) {
        env->CSR_ESTAT |= 1 << irq;
    } else {
        env->CSR_ESTAT &= ~(1 << irq);
    }
    if (FIELD_EX64(env->CSR_ESTAT, CSR_ESTAT, IS)) {
        cpu_interrupt(cs, CPU_INTERRUPT_HARD);
    } else {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    }
}

static uint64_t cpu_la32_KPn_to_phys(void *opaque, uint64_t addr)
{
    return addr & 0x1fffffffUL;
}

typedef struct FpgaResetData {
    LoongArchCPU *cpu;
    uint64_t vector;
} FpgaResetData;

static void fpga_main_cpu_reset(void *opaque)
{
    FpgaResetData *s = (FpgaResetData *)opaque;
    CPULoongArchState *env = &s->cpu->env;

    cpu_reset(CPU(s->cpu));
    env->pc = s->vector;

    /*
     * After cpu_reset, CSR_CRMD has DA=1, PG=0 — direct address mode,
     * meaning VA == PA. No DMW configuration needed.
     * For kernel ELF boot, configure DMW windows to map the
     * kernel's expected virtual address space to physical 0x80000000.
     */
    if (s->vector != FPGA_BASERAM_BASE) {
        /* Kernel boot: set up DMW to map VA 0x80000000..0x9fffffff
         * and 0xa0000000..0xbfffffff to PA 0x80000000..0x9fffffff */
        env->CSR_DMW[0] = 0xa8000011;
        env->CSR_DMW[1] = 0x88000011;
        env->CSR_CRMD   = 0xb0;
    }
}

static int64_t fpga_load_kernel(MachineState *machine)
{
    const char *kernel_filename = machine->kernel_filename;
    ram_addr_t ram_size = machine->ram_size;
    int64_t entry, kernel_low;
    uint64_t kernel_high;
    ssize_t kernel_size;

    if (getenv("BOOTROM")) {
        uint64_t load_addr;
        qemu_strtoul(getenv("BOOTROM"), 0, 0, &load_addr);
        kernel_size = load_image_targphys(kernel_filename, load_addr,
                                          ram_size - (load_addr - FPGA_BASERAM_BASE));
        return 0;  /* use default reset vector */
    }

    kernel_size = load_elf(kernel_filename, NULL,
                           cpu_la32_KPn_to_phys, NULL,
                           (uint64_t *)&entry, (uint64_t *)&kernel_low,
                           (uint64_t *)&kernel_high, NULL, 0,
                           EM_LOONGARCH, 1, 0);
    if (kernel_size < 0) {
        fprintf(stderr, "qemu: could not load kernel '%s'\n",
                kernel_filename);
        exit(1);
    }

    /* Sign-extend 32-bit entry if in 0x80000000 range */
    if ((entry & ~0x7fffffffULL) == 0x80000000) {
        entry = (int32_t)entry;
    }

    return entry;
}

static void fpga_la32_init(MachineState *machine)
{
    MemoryRegion *address_space_mem = get_system_memory();
    LoongArchCPU *cpu;
    CPULoongArchState *env;
    FpgaResetData *reset_info;

    /* CPU init */
    cpu = LOONGARCH_CPU(cpu_create(machine->cpu_type));
    qdev_init_gpio_in(DEVICE(cpu), la32_cpu_set_irq, N_IRQS);
    env = &cpu->env;
    env->CSR_TID |= 0;

    reset_info = g_new0(FpgaResetData, 1);
    reset_info->cpu = cpu;
    reset_info->vector = FPGA_BASERAM_BASE;
    qemu_register_reset(fpga_main_cpu_reset, reset_info);

    timer_init_ns(&cpu->timer, QEMU_CLOCK_VIRTUAL,
                  &loongarch_constant_timer_cb, cpu);

    /* BaseRAM: 0x80000000 - 0x803fffff */
    MemoryRegion *baseram = g_new(MemoryRegion, 1);
    memory_region_init_ram(baseram, NULL, "fpga_la32.baseram",
                           FPGA_BASERAM_SIZE, &error_fatal);
    memory_region_add_subregion(address_space_mem, FPGA_BASERAM_BASE, baseram);

    /* ExtRAM: 0x80400000 - 0x807fffff */
    MemoryRegion *extram = g_new(MemoryRegion, 1);
    memory_region_init_ram(extram, NULL, "fpga_la32.extram",
                           FPGA_EXTRAM_SIZE, &error_fatal);
    memory_region_add_subregion(address_space_mem, FPGA_EXTRAM_BASE, extram);

    /* FPGA UART */
    DeviceState *uart_dev = qdev_new(TYPE_FPGA_UART);
    qdev_prop_set_chr(uart_dev, "chardev", serial_hd(0));
    SysBusDevice *uart_sbd = SYS_BUS_DEVICE(uart_dev);
    sysbus_realize_and_unref(uart_sbd, &error_fatal);
    MemoryRegion *uart_mr = sysbus_mmio_get_region(uart_sbd, 0);
    memory_region_add_subregion(address_space_mem, FPGA_UART_BASE, uart_mr);

    /* Load kernel if provided */
    if (machine->kernel_filename) {
        int64_t entry = fpga_load_kernel(machine);
        if (entry) {
            reset_info->vector = entry;
        }
    }
}

static void fpga_la32_machine_init(MachineClass *mc)
{
    mc->desc = "LoongArch32 FPGA board (BaseRAM + ExtRAM, no BIOS)";
    mc->init = fpga_la32_init;
    mc->max_cpus = 1;
    mc->min_cpus = 1;
    mc->default_cpu_type = LOONGARCH_CPU_TYPE_NAME("la32");
    mc->no_floppy = 1;
    mc->no_parallel = 1;
}

DEFINE_MACHINE("la32_fpga", fpga_la32_machine_init)

static void fpga_machine_register_types(void)
{
    type_register_static(&fpga_uart_info);
}

type_init(fpga_machine_register_types)
