/*
 * Ciciec2026 SoC machine emulation (open-la500 CPU + 1MB SRAM + 16550 UART).
 *
 * Memory layout:
 *   0x1c000000 - 0x1c0fffff  SRAM (1 MB)
 *
 * Peripherals:
 *   16550 UART at 0x1f000000
 *
 * Copyright (C) 2024 Loongson Technology Corporation Limited
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "elf.h"
#include "hw/loader.h"
#include "hw/char/serial.h"
#include "exec/address-spaces.h"
#include "sysemu/reset.h"
#include "sysemu/sysemu.h"
#include "qemu/units.h"
#include "cpu.h"

/* SoC physical addresses */
#define CICIEC_SRAM_BASE  0x1c000000ULL
#define CICIEC_SRAM_SIZE  (8 * MiB)
#define CICIEC_UART_BASE  0x1f000000ULL

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

typedef struct CiciecResetData {
    LoongArchCPU *cpu;
    uint64_t vector;
} CiciecResetData;

static void ciciec_main_cpu_reset(void *opaque)
{
    CiciecResetData *s = (CiciecResetData *)opaque;
    CPULoongArchState *env = &s->cpu->env;

    cpu_reset(CPU(s->cpu));
    env->pc = s->vector;

    /*
     * Use direct address mode (DA=1, PG=0) so VA==PA for all accesses.
     * The SDK's start.S will configure DMW and switch to page mode.
     */
    env->CSR_CRMD = 0xa8;
}

static int64_t ciciec_load_kernel(MachineState *machine)
{
    const char *kernel_filename = machine->kernel_filename;
    int64_t entry, kernel_low;
    uint64_t kernel_high;
    ssize_t kernel_size;

    /* Try ELF first */
    kernel_size = load_elf(kernel_filename, NULL,
                           NULL, NULL,
                           (uint64_t *)&entry, (uint64_t *)&kernel_low,
                           (uint64_t *)&kernel_high, NULL, 0,
                           EM_LOONGARCH, 1, 0);
    if (kernel_size > 0) {
        return entry;
    }

    /* Fallback: load as raw binary at SRAM base */
    kernel_size = load_image_targphys(kernel_filename,
                                      CICIEC_SRAM_BASE,
                                      CICIEC_SRAM_SIZE);
    if (kernel_size < 0) {
        fprintf(stderr, "qemu: could not load kernel '%s'\n",
                kernel_filename);
        exit(1);
    }

    /* Raw binary loads at default reset vector, return 0 to keep default */
    return 0;
}

static void ciciec_soc_init(MachineState *machine)
{
    MemoryRegion *address_space_mem = get_system_memory();
    LoongArchCPU *cpu;
    CPULoongArchState *env;
    CiciecResetData *reset_info;

    /* CPU init */
    cpu = LOONGARCH_CPU(cpu_create(machine->cpu_type));
    qdev_init_gpio_in(DEVICE(cpu), la32_cpu_set_irq, N_IRQS);
    env = &cpu->env;
    env->CSR_TID |= 0;

    reset_info = g_new0(CiciecResetData, 1);
    reset_info->cpu = cpu;
    reset_info->vector = CICIEC_SRAM_BASE;
    qemu_register_reset(ciciec_main_cpu_reset, reset_info);

    timer_init_ns(&cpu->timer, QEMU_CLOCK_VIRTUAL,
                  &loongarch_constant_timer_cb, cpu);

    /* SRAM: 0x1c000000 - 0x1c0fffff */
    MemoryRegion *sram = g_new(MemoryRegion, 1);
    memory_region_init_ram(sram, NULL, "ciciec_soc.sram",
                           CICIEC_SRAM_SIZE, &error_fatal);
    memory_region_add_subregion(address_space_mem, CICIEC_SRAM_BASE, sram);

    /* 16550 UART at 0x1f000000 */
    DeviceState *cpudev = DEVICE(qemu_get_cpu(0));
    serial_mm_init(address_space_mem, CICIEC_UART_BASE, 0,
                   qdev_get_gpio_in(cpudev, 3), 115200,
                   serial_hd(0), DEVICE_NATIVE_ENDIAN);

    /* Load kernel if provided */
    if (machine->kernel_filename) {
        int64_t entry = ciciec_load_kernel(machine);
        if (entry) {
            reset_info->vector = entry;
        }
    }
}

static void ciciec_soc_machine_init(MachineClass *mc)
{
    mc->desc = "Ciciec2026 SoC (open-la500 + 1MB SRAM + 16550 UART)";
    mc->init = ciciec_soc_init;
    mc->max_cpus = 1;
    mc->min_cpus = 1;
    mc->default_cpu_type = LOONGARCH_CPU_TYPE_NAME("la32");
    mc->no_floppy = 1;
    mc->no_parallel = 1;
}

DEFINE_MACHINE("ciciec_soc", ciciec_soc_machine_init)
