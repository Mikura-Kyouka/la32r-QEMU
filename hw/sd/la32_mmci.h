#ifndef __LS1GPA_MMCI_H__
#define __LS1GPA_MMCI_H__

struct LS1GPA_MMCIState;
typedef struct LS1GPA_MMCIState LS1GPA_MMCIState;
void *ls1gpa_mmci_init(MemoryRegion *sysmem,
                       hwaddr base,
                       BlockBackend *blk, qemu_irq irq);

void ls1gpa_mmci_handlers(LS1GPA_MMCIState *s, qemu_irq readonly,
                          qemu_irq coverswitch);

void ls1gpa_sdio_set_dmaaddr(uint32_t val);
//extern target_ulong mypc;
#endif
