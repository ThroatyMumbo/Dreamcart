#include <kos.h>
#include <arch/mmu.h>
#include "hostmmu.h"
#include "dcmmu.h"
#include "tmpl.h"
#include "view.h"

#define TEA (*(volatile uint32_t *)0xFF00000Cu)

static uint32_t bias;
uint32_t g_view_bias, g_guest_sp;
void (*dcmmu_on_fault)(uint32_t pc, uint32_t pr);

static void fault(irq_t code, irq_context_t *ctx, void *data) {
    (void)data;
    uint32_t tea = TEA;
    printf("hostmmu: TLB fault %03lx at guest %08lx (host %08lx), pc %08lx\n", (unsigned long)code,
           (unsigned long)(tea - bias), (unsigned long)tea, (unsigned long)ctx->pc);
    for (int i = 0; i < 16; i++) printf("r%d %08lx%s", i, (unsigned long)ctx->r[i], i % 8 == 7 ? "\n" : " ");
    if (dcmmu_on_fault) dcmmu_on_fault(ctx->pc, ctx->pr);
    fflush(stdout);
    arch_abort();
}

void dcmmu_init(void *rdram) {
    static const irq_t codes[] = { EXC_DTLB_MISS_READ, EXC_DTLB_MISS_WRITE, EXC_DTLB_PV_READ, EXC_DTLB_PV_WRITE,
                                   EXC_INITIAL_PAGE_WRITE };
    bias = g_view_bias = t_rdram_bias = (uint32_t)rdram - RDRAM_LO;
    g_guest_sp = GUEST_STACK_TOP;
    for (unsigned i = 0; i < sizeof codes / sizeof codes[0]; i++) irq_set_handler(codes[i], fault, NULL);
    mmu_init_basic();
    hostmmu_reserve_top(2);
}
