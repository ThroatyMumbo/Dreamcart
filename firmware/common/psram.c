// APS6404L bring-up on QMI M1: reset it in SPI, read its ID, switch it to QPI, then quad reads (0xEB) and writes (0x38)
// through the XIP window. Sequence after SparkFun's sfe_psram.c for the RP2350.
#include "psram.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"
#include "pico/stdlib.h"

#define MAX_SCK_HZ   109000000u         // APS6404L at 3.3 V, wrapped reads
#define MAX_SELECT_NS 8000u             // tCEM: CS low at most 8 us, for its refresh
#define MIN_DESELECT_NS 18u             // tCPH
#define KGD          0x5Du              // the ID's known-good-die byte

static uint32_t size;
static volatile uint32_t locked, inside;

static void direct_wait(void) { while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {} }

// One byte out on CS1 in SPI or QPI, its reply byte back
static uint8_t direct_byte(uint8_t b, bool quad) {
    qmi_hw->direct_tx = (quad ? QMI_DIRECT_TX_OE_BITS | QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB : 0) | b;
    direct_wait();
    return (uint8_t)qmi_hw->direct_rx;
}

static void direct_cmd(uint8_t b, bool quad) {
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    direct_byte(b, quad);
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
}

static uint32_t __no_inline_not_in_flash_func(bring_up)(void) {
    uint32_t sys = clock_get_hz(clk_sys), div = (sys + MAX_SCK_HZ - 1) / MAX_SCK_HZ;
    uint64_t fs = 1000000000000000ull / sys;  // femtoseconds per sys clock
    uint32_t max_select = (uint32_t)((MAX_SELECT_NS * 1000000ull / 64) / fs);
    uint32_t min_deselect = (uint32_t)((MIN_DESELECT_NS * 1000000ull + fs - 1) / fs);
    uint32_t rxdelay = div + (sys / div > 100000000u);    // as sfe_psram.c; 1 at 100 MHz SCK read garbage
    uint32_t irq = save_and_disable_interrupts();
    qmi_hw->direct_csr = 30u << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
    direct_wait();
    direct_cmd(0xF5, true);  // leave QPI, in case a warm reset left it there
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    uint8_t kgd = 0, eid = 0;
    for (int i = 0; i < 7; i++) {  // read ID: command, 3 address bytes, then KGD and EID
        uint8_t r = direct_byte(i == 0 ? 0x9F : 0xFF, false);
        if (i == 5) kgd = r;
        if (i == 6) eid = r;
    }
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    if (kgd != KGD) {
        qmi_hw->direct_csr = 0;
        restore_interrupts(irq);
        return 0;
    }
    direct_cmd(0x66, false);  // reset enable
    direct_cmd(0x99, false);  // reset
    direct_cmd(0x35, false);  // enter QPI
    qmi_hw->m[1].timing = QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
                          3u << QMI_M1_TIMING_SELECT_HOLD_LSB | 1u << QMI_M1_TIMING_COOLDOWN_LSB |
                          rxdelay << QMI_M1_TIMING_RXDELAY_LSB | max_select << QMI_M1_TIMING_MAX_SELECT_LSB |
                          min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB | div << QMI_M1_TIMING_CLKDIV_LSB;
    qmi_hw->m[1].rfmt = QMI_M1_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_PREFIX_WIDTH_LSB |
                        QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_RFMT_ADDR_WIDTH_LSB |
                        QMI_M1_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_SUFFIX_WIDTH_LSB |
                        QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_RFMT_DUMMY_WIDTH_LSB |
                        QMI_M1_RFMT_DUMMY_LEN_VALUE_24 << QMI_M1_RFMT_DUMMY_LEN_LSB |
                        QMI_M1_RFMT_DATA_WIDTH_VALUE_Q << QMI_M1_RFMT_DATA_WIDTH_LSB |
                        QMI_M1_RFMT_PREFIX_LEN_VALUE_8 << QMI_M1_RFMT_PREFIX_LEN_LSB;
    qmi_hw->m[1].rcmd = 0xEB;
    qmi_hw->m[1].wfmt = QMI_M1_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_PREFIX_WIDTH_LSB |
                        QMI_M1_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_WFMT_ADDR_WIDTH_LSB |
                        QMI_M1_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_SUFFIX_WIDTH_LSB |
                        QMI_M1_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_WFMT_DUMMY_WIDTH_LSB |
                        QMI_M1_WFMT_DATA_WIDTH_VALUE_Q << QMI_M1_WFMT_DATA_WIDTH_LSB |
                        QMI_M1_WFMT_PREFIX_LEN_VALUE_8 << QMI_M1_WFMT_PREFIX_LEN_LSB;
    qmi_hw->m[1].wcmd = 0x38;
    qmi_hw->direct_csr = 0;
    xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M1_BITS;
    restore_interrupts(irq);
    uint32_t mb = (eid >> 5) == 2 ? 8 : (eid >> 5) == 1 ? 4 : 2;  // EID's density bits
    return mb << 20;
}

uint32_t psram_init(void) {
    gpio_set_function(PSRAM_CS_PIN, GPIO_FUNC_XIP_CS1);
    size = bring_up();
    if (!size) return 0;
    volatile uint32_t *p = (volatile uint32_t *)PSRAM_BASE;  // the first and last words, read back past the cache
    p[0] = 0x5A5AA5A5u, p[size / 4 - 1] = 0xC3C33C3Cu;
    xip_cache_clean_all();
    xip_cache_invalidate_all();
    if (p[0] != 0x5A5AA5A5u || p[size / 4 - 1] != 0xC3C33C3Cu) size = 0;
    return size;
}

uint32_t psram_size(void) { return size; }

void psram_lock(void) {
    locked = 1;
    __dmb();
    while (inside) {}
}

void psram_unlock(void) {
    __dmb();
    locked = 0;
}

bool __not_in_flash_func(psram_enter)(void) {
    inside = 1;
    __dmb();
    if (locked) { inside = 0; return false; }
    return true;
}

void __not_in_flash_func(psram_leave)(void) {
    __dmb();
    inside = 0;
}
