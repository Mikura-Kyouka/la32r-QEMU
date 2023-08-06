/*
 * Loongson LS1GPA MultiMediaCard/SD/SDIO Controller emulation.
 *
 * Copyright (c) 2006 Openedhand Ltd.
 * Written by Andrzej Zaborowski <balrog@zabor.org>
 *
 * This code is licensed under the GPLv2.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/sd/sd.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "qemu/error-report.h"
#include "migration/vmstate.h"
#include "sysemu/dma.h"
#include "la32_mmci.h"

#define REG_FMT                "0x" TARGET_FMT_plx
#define TYPE_LS1GPA_MMCI "ls1gpa-mmci"
#define LS1GPA_MMCI(obj) OBJECT_CHECK(LS1GPA_MMCIState, (obj), TYPE_LS1GPA_MMCI)

#define TYPE_LS1GPA_MMCI_BUS "ls1gpa-mmci-bus"
#define LS1GPA_MMCI_BUS(obj) OBJECT_CHECK(SDBus, (obj), TYPE_LS1GPA_MMCI_BUS)

#define SDICON     0x00
#define SDIPRE     0x04
#define SDICMDARG  0x08
#define SDICMDCON  0x0c
#define SDICMDSTA  0x10
#define SDIRSP0    0x14
#define SDIRSP1    0x18
#define SDIRSP2    0x1C
#define SDIRSP3    0x20
#define SDIDTIMER  0x24
#define SDIBSIZE   0x28
#define SDIDATCON  0x2C
/* #define SDIDCON               (0x2C) */
#define SDIDATCNT  0x30
#define SDIDSTA               (0x34)
#define SDIFSTA    0x38
#define SDIINTMSK  0x3C
#define SDIWRDAT   0x40
#define SDISTAADD0 0x44
#define SDISTAADD1 0x48
#define SDISTAADD2 0x4c
#define SDISTAADD3 0x50
#define SDISTAADD4 0x54
#define SDISTAADD5 0x58
#define SDISTAADD6 0x5c
#define SDISTAADD7 0x60
#define SDIINTEN   0x64

#define SDICON_SDRESET        (1 << 8)
#define SDICON_MMCCLOCK       (1 << 5)
#define SDICON_BYTEORDER      (1 << 4)
#define SDICON_SDIOIRQ        (1 << 3)
#define SDICON_RWAITEN        (1 << 2)
#define SDICON_FIFORESET      (1 << 1)
#define SDICON_CLOCKTYPE      (1 << 0)

#define SDICMDCON_ABORT       (1 << 12)
#define SDICMDCON_WITHDATA    (1 << 11)
#define SDICMDCON_LONGRSP     (1 << 10)
#define SDICMDCON_WAITRSP     (1 << 9)
#define SDICMDCON_CMDSTART    (1 << 8)
#define SDICMDCON_SENDERHOST  (1 << 6)
#define SDICMDCON_INDEX       (0x3f)

#define SDICMDSTAT_CRCFAIL    (1 << 12)
#define SDICMDSTAT_CMDSENT    (1 << 11)
#define SDICMDSTAT_CMDTIMEOUT (1 << 10)
#define SDICMDSTAT_RSPFIN     (1 << 9)
#define SDICMDSTAT_XFERING    (1 << 8)
#define SDICMDSTAT_INDEX      (0xff)

#define S3C2440_SDIDCON_DS_BYTE       (0 << 22)
#define S3C2440_SDIDCON_DS_HALFWORD   (1 << 22)
#define S3C2440_SDIDCON_DS_WORD       (2 << 22)
#define SDIDCON_IRQPERIOD     (1 << 21)
#define SDIDCON_TXAFTERRESP   (1 << 20)
#define SDIDCON_RXAFTERCMD    (1 << 19)
#define SDIDCON_BUSYAFTERCMD  (1 << 18)
#define SDIDCON_BLOCKMODE     (1 << 17)
#define SDIDCON_WIDEBUS       (1 << 16)
#define MISC_CTRL_SDIO_DMAEN         (1 << 15)
#define SDIDCON_STOP          (1 << 14)
#define S3C2440_SDIDCON_DATSTART      (1 << 14)
#define SDIDCON_DATMODE       (3 << 12)
#define SDIDCON_BLKNUM        (0x7ff)

/* constants for SDIDCON_DATMODE */
#define SDIDCON_XFER_READY    (0 << 12)
#define SDIDCON_XFER_CHKSTART (1 << 12)
#define SDIDCON_XFER_RXSTART  (2 << 12)
#define SDIDCON_XFER_TXSTART  (3 << 12)

#define SDIDCON_BLKNUM_MASK   (0xFFF)
#define SDIDCNT_BLKNUM_SHIFT  (12)

#define SDIDSTA_RDYWAITREQ    (1 << 10)
#define SDIDSTA_SDIOIRQDETECT (1 << 9)
#define SDIDSTA_FIFOFAIL      (1 << 8)    /* reserved on 2440 */
#define SDIDSTA_CRCFAIL       (1 << 7)
#define SDIDSTA_RXCRCFAIL     (1 << 6)
#define SDIDSTA_DATATIMEOUT   (1 << 5)
#define SDIDSTA_XFERFINISH    (1 << 4)
#define SDIDSTA_BUSYFINISH    (1 << 3)
#define SDIDSTA_SBITERR       (1 << 2)    /* reserved on 2410a/2440 */
#define SDIDSTA_TXDATAON      (1 << 1)
#define SDIDSTA_RXDATAON      (1 << 0)

#define S3C2440_SDIFSTA_FIFORESET      (1 << 16)
#define S3C2440_SDIFSTA_FIFOFAIL       (3 << 14)  /* 3 is correct (2 bits) */
#define SDIFSTA_TFDET          (1 << 13)
#define SDIFSTA_RFDET          (1 << 12)
#define SDIFSTA_TFHALF         (1 << 11)
#define SDIFSTA_TFEMPTY        (1 << 10)
#define SDIFSTA_RFLAST         (1 << 9)
#define SDIFSTA_RFFULL         (1 << 8)
#define SDIFSTA_RFHALF         (1 << 7)
#define SDIFSTA_COUNTMASK      (0x7f)

/* #define SDIIMSK_RESPONSECRC    (1<<17) */
#define SDIIMSK_RESPONSEND     (1 << 14)
#define SDIIMSK_READWAIT       (1 << 13)
#define SDIIMSK_SDIOIRQ        (1 << 12)
#define SDIIMSK_FIFOFAIL       (1 << 11)


#define SDIIMSK_RESPONSECRC    (1 << 8)
#define SDIIMSK_CMDTIMEOUT     (1 << 7)
#define SDIIMSK_CMDSENT        (1 << 6)
#define SDIIMSK_PROGERR        (1 << 4)
#define SDIIMSK_TXCRCFAIL      (1 << 3)
#define SDIIMSK_RXCRCFAIL      (1 << 2)
#define SDIIMSK_DATATIMEOUT    (1 << 1)
#define SDIIMSK_DATAFINISH     (1 << 0)

#define SDIIMSK_BUSYFINISH     (1 << 6)
#define SDIIMSK_SBITERR        (1 << 5)   /* reserved 2440/2410a */
#define SDIIMSK_TXFIFOHALF     (1 << 4)
#define SDIIMSK_TXFIFOEMPTY    (1 << 3)
#define SDIIMSK_RXFIFOLAST     (1 << 2)
#define SDIIMSK_RXFIFOFULL     (1 << 1)
#define SDIIMSK_RXFIFOHALF     (1 << 0)
typedef struct ls1gpa_sdio_regs {
        int sdicon;
        int sdipre;
        int sdicmdarg;
        int sdicmdcon;
        int sdicmdsta;
        int sdirsp0;
        int sdirsp1;
        int sdirsp2;
        int sdirsp3;
        int sdidtimer;
        int sdibsize;
        int sdidatcon;
        int sdidatcnt;
        int sdidsta;
        int sdifsta;
        int sdiintmsk;
        int sdiwrdat;
        int sdistaadd0;
        int sdistaadd1;
        int sdistaadd2;
        int sdistaadd3;
        int sdistaadd4;
        int sdistaadd5;
        int sdistaadd6;
        int sdistaadd7;
        int sdiinten;
} ls1gpa_sdio_regs_t;

struct dma_desc {
        uint32_t ordered;
        uint32_t saddr;
        uint32_t daddr;
        uint32_t length;
        uint32_t step_length;
        uint32_t step_times;
        uint32_t cmd;
        /*used by logic only*/
        uint32_t left;
        uint32_t active;
        uint32_t nextaddr;
};

enum {
        DMA_INT_MASK =  1,
        DMA_INT = 2,
        DMA_SINGLE_TRANS_OVER = 4,
        DMA_TRANS_OVER = 8,
        DMA_ORDER_EN = 1,
        DMA_READ_DDR = 0x1 << 12,
};

struct LS1GPA_MMCIState {
        SysBusDevice parent_obj;

        MemoryRegion iomem;
        qemu_irq irq;
        qemu_irq inserted;
        qemu_irq readonly;

        BlockBackend *blk;
        SDBus sdbus;

        int32_t blksize;
        int32_t blknum;
        uint32_t intmask;
        uint32_t intreq;
        int dma_int_status;
        int iolen;
        int32_t cmd;
        uint32_t arg;
        uint8_t *ioaddr;

        struct dma_desc dma_desc;
        uint8_t fifo_buffer[0x1000];

        int32_t cmdreq;

        union {
                AddressSpace *as;
                void *as_ptr;
        };

        union {
                ls1gpa_sdio_regs_t sdioregs;
                uint32_t regdata[0x60 / 4];
        };
};


static const VMStateDescription vmstate_ls1gpa_mmci = {
        .name = "ls1gpa-mmci",
        .version_id = 2,
        .minimum_version_id = 2,
        .fields = (VMStateField[])
        {
                VMSTATE_END_OF_LIST()
        }
};

LS1GPA_MMCIState *ls1gp_sdio;
static int dma_readsdio(LS1GPA_MMCIState *s);
static int dma_writesdio(LS1GPA_MMCIState *s);
static void ls1gpa_mmci_end_transfer(LS1GPA_MMCIState *s);

static void ls1gpa_mmci_data_transfer(LS1GPA_MMCIState *s);

#define DPRINT_L1 printf
#define ERRPRINT printf
#define min(x, y) ((x) < (y) ? (x) : (y))
static void ls1gpa_mmci_update_irq(LS1GPA_MMCIState *s)
{
        /* edge interrupt */
        if (s->sdioregs.sdiintmsk) {
                qemu_irq_raise(s->irq);
        } else {
                qemu_irq_lower(s->irq);
        }

}

static void ls1gpa_mmci_send_command(LS1GPA_MMCIState *s)
{
        SDRequest request;
        uint8_t response[16];
        int rlen;

        s->sdioregs.sdicmdsta = 0;
        s->sdioregs.sdiintmsk = 0;
        if (s->iolen) {
                printf("bug iolen is %d\n", s->iolen);
        }
        s->iolen = 0;
        s->ioaddr = s->fifo_buffer;
        request.cmd = s->sdioregs.sdicmdcon & SDICMDCON_INDEX;
        request.arg = s->sdioregs.sdicmdarg;
        request.crc = 0;        /* FIXME */
        //DPRINT_L1("sending CMD%u ARG[0x%08x] pc=0x%lx\n", request.cmd,
        //request.arg, (long)mypc);
        rlen = sdbus_do_command(&s->sdbus, &request, response);
        printf("sdicmdcon 0x%x len 0x%x\n", s->sdioregs.sdicmdcon, rlen);

        if (s->sdioregs.sdicmdcon  & SDICMDCON_WAITRSP) {
                if (rlen == 4) {
                        s->sdioregs.sdirsp0 = (response[0] << 24) | (response[1]
                        << 16) | (response[2] << 8) | response[3];
                        s->sdioregs.sdirsp1 = s->sdioregs.sdirsp2 =
                        s->sdioregs.sdirsp3 = 0;
                        DPRINT_L1("Response: RSPREG[31..0]=0x%08x\n",
                        s->sdioregs.sdirsp0);
                        s->sdioregs.sdicmdsta |= SDICMDSTAT_RSPFIN;
                        s->sdioregs.sdiintmsk |= SDIIMSK_CMDSENT;
                } else if (rlen == 16) {
                        s->sdioregs.sdirsp3 = (response[11] << 24) |
                        (response[12] << 16) | (response[13] << 8) |
                        response[14];
                        s->sdioregs.sdirsp2 = (response[7] << 24) | (response[8]
                        << 16) | (response[9] << 8)  |  response[10];
                        s->sdioregs.sdirsp1 = (response[3] << 24) | (response[4]
                        << 16) | (response[5] << 8) | response[6];
                        s->sdioregs.sdirsp0 = (response[0] << 16) | (response[1]
                        << 8) |
                                              response[2];
                        DPRINT_L1("Response received:\n RSPREG[127..96]=0x%08x,"
                                  "RSPREG[95..64]=0x%08x,\n RSPREG[63..32]="
                                  "0x%08x, RSPREG[31..0]=0x%08x\n",
                                  s->sdioregs.sdirsp0, s->sdioregs.sdirsp1,
                                  s->sdioregs.sdirsp2, s->sdioregs.sdirsp3);
                        s->sdioregs.sdicmdsta |= SDICMDSTAT_RSPFIN;
                        s->sdioregs.sdiintmsk |= SDIIMSK_CMDSENT;
                } else {
                        ERRPRINT("Timeout waiting for command response\n");
                        s->sdioregs.sdicmdsta |= SDICMDSTAT_CMDTIMEOUT;
                        s->sdioregs.sdiintmsk |= SDIIMSK_CMDTIMEOUT;
                }

        } else {
                s->sdioregs.sdiintmsk |= SDIIMSK_CMDSENT;
        }

        if (s->sdioregs.sdibsize && (s->sdioregs.sdidatcon & SDIDCON_BLKNUM)) {
                s->blknum = s->sdioregs.sdidatcon & SDIDCON_BLKNUM;
                s->blksize = s->sdioregs.sdibsize;
                ls1gpa_mmci_data_transfer(s);
        }

        s->sdioregs.sdicmdsta |= SDICMDSTAT_CMDSENT;
        s->sdioregs.sdicmdcon &= ~SDICMDCON_CMDSTART;
        ls1gpa_mmci_update_irq(s);
}


void ls1gpa_sdio_set_dmaaddr(uint32_t val)
{
        LS1GPA_MMCIState *s = ls1gp_sdio;
        uint32_t dmaaddr;
        dmaaddr = val & ~0xf;

        if (val & 0x8) {
                if (s->dma_desc.active) {
                        s->dma_desc.nextaddr = dmaaddr;
                } else {
                        dma_memory_read(s->as, dmaaddr, (uint8_t *)&s->dma_desc,
                        4 * 7, MEMTXATTRS_UNSPECIFIED);
                        s->dma_desc.left = s->dma_desc.length * 4;
                        s->dma_desc.step_times--;
                        s->dma_desc.active = 1;
                        s->dma_desc.nextaddr = 0;
                        ls1gpa_mmci_data_transfer(s);
                }
        }

        if (val & 0x4) {
                dma_memory_write(s->as, dmaaddr, (uint8_t *)&s->dma_desc, 4 *
                7, MEMTXATTRS_UNSPECIFIED);
        }

}

static int dma_next(LS1GPA_MMCIState *s)
{

        if (!s->dma_desc.step_times) {
                if (s->dma_desc.cmd & DMA_INT_MASK) {
                        s->dma_desc.cmd |= DMA_INT;
                        s->dma_int_status = 1;
                        ls1gpa_mmci_update_irq(s);
                }


                if (s->dma_desc.ordered & DMA_ORDER_EN) {
                        dma_memory_read(s->as, s->dma_desc.ordered &
                        ~DMA_ORDER_EN, (uint8_t *)&s->dma_desc, 4 * 7, MEMTXATTRS_UNSPECIFIED);
                        s->dma_desc.left = s->dma_desc.length * 4;
                        s->dma_desc.step_times--;
                } else if (s->dma_desc.nextaddr) {
                        dma_memory_read(s->as, s->dma_desc.nextaddr, (uint8_t
                        *)&s->dma_desc, 4 * 7, MEMTXATTRS_UNSPECIFIED);
                        s->dma_desc.nextaddr = 0;
                        s->dma_desc.left = s->dma_desc.length * 4;
                } else {
                        s->dma_desc.cmd |= DMA_TRANS_OVER;
                        s->dma_desc.active = 0;
                }
        } else {
                s->dma_desc.step_times--;
                s->dma_desc.saddr += s->dma_desc.step_length * 4;
                s->dma_desc.left = s->dma_desc.length * 4;
        }
        return s->dma_desc.active;
}

static int dma_readsdio(LS1GPA_MMCIState *s)
{
        uint32_t copied;

        while (s->dma_desc.active && (s->blknum || s->iolen)
                        && !(s->dma_desc.cmd & DMA_READ_DDR)
                        && (s->sdioregs.sdicmdcon & SDICMDCON_CMDSTART)) {
                if (!s->iolen) {
                        int i;
                        for (i = 0; i < s->blksize; i++) {
                                sdbus_read_data(&s->sdbus, (&(s->fifo_buffer[i])), 1);
                        }
                        s->ioaddr = s->fifo_buffer;
                        s->blknum--;
                        s->iolen = s->blksize;
                }

                if (!s->dma_desc.left && !dma_next(s)) {
                        break;
                }

                copied = min(s->dma_desc.left, s->iolen);

                if (!copied) {
                        break;
                }

                dma_memory_write(s->as, s->dma_desc.saddr, s->ioaddr, copied, MEMTXATTRS_UNSPECIFIED);

                s->ioaddr += copied;
                s->iolen -= copied;
                s->dma_desc.saddr += copied;
                s->dma_desc.left -= copied;

                if (!s->dma_desc.left) {
                        dma_next(s);
                }

                if (!s->blknum && !s->iolen) {
                        ls1gpa_mmci_end_transfer(s);
                        break;
                }

        }

        return s->dma_desc.active;
}


static int dma_writesdio(LS1GPA_MMCIState *s)
{
        uint32_t wantsize, copied;
        wantsize = 0;

        while (s->dma_desc.active && (s->blknum || s->iolen)
                        && (s->dma_desc.cmd & DMA_READ_DDR)
                        && (s->sdioregs.sdicmdcon & SDICMDCON_CMDSTART)) {
                if (!s->dma_desc.left && !dma_next(s)) {
                        break;
                }

                wantsize = s->blksize - s->iolen;
                copied = min(s->dma_desc.left, wantsize);

                if (!copied) {
                        break;
                }

                dma_memory_read(s->as, s->dma_desc.saddr, s->ioaddr, copied, MEMTXATTRS_UNSPECIFIED);

                s->ioaddr += copied;
                s->iolen += copied;
                s->dma_desc.saddr += copied;
                s->dma_desc.left -= copied;

                if (!s->dma_desc.left) {
                        dma_next(s);
                }

                if (s->iolen == s->blksize) {
                        int i;
                        for (i = 0; i < s->blksize; i++) {
                                sdbus_write_data(&s->sdbus, (&(s->fifo_buffer[i])), 1);
                        }
                        s->iolen = 0;
                        s->ioaddr = s->fifo_buffer;
                        s->blknum--;
                }

                if (!s->blknum && !s->iolen) {
                        ls1gpa_mmci_end_transfer(s);
                        break;
                }
        }

        return s->dma_desc.active;
}


static void ls1gpa_mmci_end_transfer(LS1GPA_MMCIState *s)
{
        /* Automatically send CMD12 to stop transfer if AutoCMD12 enabled */
        if ((s->sdioregs.sdidatcon & SDIDCON_BLKNUM)
                        && (s->sdioregs.sdicmdcon & SDICMDCON_ABORT)) {
                SDRequest request;
                uint8_t response[16];

                request.cmd = 0x0C;
                request.arg = 0;
                DPRINT_L1("Automatically issue CMD%d %08x\n", request.cmd,
                request.arg);
                sdbus_do_command(&s->sdbus, &request, response);
                /* Auto CMD12 response goes to the upper Response register */
                s->sdioregs.sdirsp3 = (response[0] << 24) | (response[1] << 16)
                | (response[2] << 8) | response[3];
        }


        s->sdioregs.sdiintmsk |= SDIIMSK_DATAFINISH;
        ls1gpa_mmci_update_irq(s);
}

static void ls1gpa_mmci_data_transfer(LS1GPA_MMCIState *s)
{

        if (s->sdioregs.sdidatcon  & MISC_CTRL_SDIO_DMAEN) {
                if (s->dma_desc.active && (s->blknum || s->iolen) && s->blksize
                && (s->sdioregs.sdicmdcon & SDICMDCON_CMDSTART)) {
                        if (s->dma_desc.cmd & DMA_READ_DDR) {
                                dma_writesdio(s);
                        } else {
                                dma_readsdio(s);
                        }
                }
        }

}


static uint64_t ls1gpa_mmci_read(void *opaque, hwaddr offset, unsigned size)
{
        LS1GPA_MMCIState *s = (LS1GPA_MMCIState *) opaque;

        switch (offset) {
        case SDIINTMSK:
                return s->sdioregs.sdiintmsk;
        default:
                return s->regdata[offset / 4];
        }

        return 0;
}

static void ls1gpa_mmci_write(void *opaque,
                              hwaddr offset, uint64_t value, unsigned size)
{
        LS1GPA_MMCIState *s = (LS1GPA_MMCIState *) opaque;

        switch (offset) {
        case SDIINTMSK:
                s->sdioregs.sdiintmsk &= ~value;
                ls1gpa_mmci_update_irq(s);
                break;
        case SDICMDCON:
                s->regdata[offset / 4] = value;
                if (value & SDICMDCON_CMDSTART) {
                        ls1gpa_mmci_send_command(s);
                }
                break;
        default:
                s->regdata[offset / 4] = value;
        }
}

static const MemoryRegionOps ls1gpa_mmci_ops = {
        .read = ls1gpa_mmci_read,
        .write = ls1gpa_mmci_write,
        .endianness = DEVICE_NATIVE_ENDIAN,
};

void *ls1gpa_mmci_init(MemoryRegion *sysmem,
                       hwaddr base,
                       BlockBackend *blk, qemu_irq irq)
{
        DeviceState *dev, *carddev;
        SysBusDevice *sbd;
        LS1GPA_MMCIState *s;
        Error *err = NULL;

        //dev = qdev_create(NULL, TYPE_LS1GPA_MMCI);
        /* create mmci dev and bind it to main sysbus */
        dev = qdev_new(TYPE_LS1GPA_MMCI);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

        /* cast to mmci device and set it's as and ioaddr */
        s = LS1GPA_MMCI(dev);
        if (!s->as) {
                s->as = &address_space_memory;
        }
        s->ioaddr = s->fifo_buffer;

        /* cast to sys-bus-device, set irq and memory-region */
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_connect_irq(sbd, 0, irq);
        memory_region_add_subregion(sysmem, base, sbd->mmio[0].memory);




        /* Create and plug in the sd card */
        carddev = qdev_new(TYPE_SD_CARD);
        qdev_prop_set_drive(carddev, "drive", blk);
        if (err) {
                error_report("failed to init SD card: %s",
                error_get_pretty(err));
                return NULL;
        }
        qdev_realize_and_unref(carddev, qdev_get_child_bus(DEVICE(dev), "sd-bus"),
                                &error_fatal);
        return s;
}

static void ls1gpa_mmci_set_inserted(DeviceState *dev, bool inserted)
{
        LS1GPA_MMCIState *s = LS1GPA_MMCI(dev);

        qemu_set_irq(s->inserted, inserted);
}

static void ls1gpa_mmci_set_readonly(DeviceState *dev, bool readonly)
{
        LS1GPA_MMCIState *s = LS1GPA_MMCI(dev);

        qemu_set_irq(s->readonly, readonly);
}

void ls1gpa_mmci_handlers(LS1GPA_MMCIState *s, qemu_irq readonly,
                          qemu_irq coverswitch)
{
        DeviceState *dev = DEVICE(s);

        s->readonly = readonly;
        s->inserted = coverswitch;

        ls1gpa_mmci_set_inserted(dev, sdbus_get_inserted(&s->sdbus));
        ls1gpa_mmci_set_readonly(dev, sdbus_get_readonly(&s->sdbus));
}

typedef struct initcmd {
        uint8_t cmd;
        uint32_t arg;
} initcmd_t;

static void ls1gpa_mmci_reset(DeviceState *d)
{
        if (getenv("SDBOOT")) {
                LS1GPA_MMCIState *s = LS1GPA_MMCI(d);
                SDRequest request;
                uint8_t response[16];
                int i;
                uint32_t rca = 0;
                initcmd_t initcmd[] = {
                        {0, 0},
                        {8, 0x1aa},
                        {55, 0},
                        {41, 0x40ff8000},
                        {2, 0},
                        {3, 0},
                        {7, 0},
                        {55, 0},
                        {6, 2}
                };

                for (i = 0; i < sizeof(initcmd) / sizeof(initcmd[0]); i++) {
                        request.cmd = initcmd[i].cmd;
                        request.crc = 0;        /* FIXME */

                        switch (initcmd[i].cmd) {
                        case 7:
                        case 55:
                                request.arg = rca;
                                break;
                        default:
                                request.arg = initcmd[i].arg;
                                break;
                        }


                        sdbus_do_command(&s->sdbus, &request, response);

                        switch (initcmd[i].cmd) {
                        case 3:
                                rca = (response[0] << 24) | (response[1] << 16);
                                break;
                        }
                }
        }

}

static void ls1gpa_mmci_instance_init(Object *obj)
{
        LS1GPA_MMCIState *s = LS1GPA_MMCI(obj);
        SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    /* DeviceState *dev = DEVICE(obj); */

        ls1gp_sdio = s;

        memory_region_init_io(&s->iomem, obj, &ls1gpa_mmci_ops, s,
                              "ls1gpa-mmci", 0x001000);
        sysbus_init_mmio(sbd, &s->iomem);
        sysbus_init_irq(sbd, &s->irq);

        qbus_init(&s->sdbus, sizeof(s->sdbus),
                            TYPE_LS1GPA_MMCI_BUS, DEVICE(obj), "sd-bus");
}

static void ls1gpa_mmci_class_init(ObjectClass *klass, void *data)
{
        DeviceClass *dc = DEVICE_CLASS(klass);

        dc->vmsd = &vmstate_ls1gpa_mmci;
        dc->reset = ls1gpa_mmci_reset;
}

static void ls1gpa_mmci_bus_class_init(ObjectClass *klass, void *data)
{
        SDBusClass *sbc = SD_BUS_CLASS(klass);

        sbc->set_inserted = ls1gpa_mmci_set_inserted;
        sbc->set_readonly = ls1gpa_mmci_set_readonly;
}

static const TypeInfo ls1gpa_mmci_info = {
        .name = TYPE_LS1GPA_MMCI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(LS1GPA_MMCIState),
        .instance_init = ls1gpa_mmci_instance_init,
        .class_init = ls1gpa_mmci_class_init,
};

static const TypeInfo ls1gpa_mmci_bus_info = {
        .name = TYPE_LS1GPA_MMCI_BUS,
        .parent = TYPE_SD_BUS,
        .instance_size = sizeof(SDBus),
        .class_init = ls1gpa_mmci_bus_class_init,
};

static void ls1gpa_mmci_register_types(void)
{
        type_register_static(&ls1gpa_mmci_info);
        type_register_static(&ls1gpa_mmci_bus_info);
}

type_init(ls1gpa_mmci_register_types)
