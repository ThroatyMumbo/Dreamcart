// One shared PIO program, one SM per bus; callers pass real GPIO numbers and the SDK applies the base.

#include "input/onewire.h"
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "onewire.pio.h"
#include "pio_share.h"
#include "board.h"

static PIO ow_pio = PIO_JOYBUS;
static uint ow_sm[ONEWIRE_MAX_BUSES];
static uint8_t ow_pin[ONEWIRE_MAX_BUSES];
static bool ow_used[ONEWIRE_MAX_BUSES];
static absolute_time_t ow_free_at[ONEWIRE_MAX_BUSES];  // the device's settle gap ends here
static uint ow_offset = 0;

static void ow_load(void) {
    ow_offset = pio_add_program(ow_pio, &onewire_program);
    for (uint id = 0; id < ONEWIRE_MAX_BUSES; id++) {
        if (!ow_used[id]) continue;
        pio_sm_config c = onewire_program_get_default_config(ow_offset);
        onewire_program_init(ow_pio, ow_sm[id], ow_offset, ow_pin[id], &c);
    }
}

static void ow_unload(void) {
    for (uint id = 0; id < ONEWIRE_MAX_BUSES; id++)
        if (ow_used[id]) pio_sm_set_enabled(ow_pio, ow_sm[id], false);
    pio_remove_program(ow_pio, &onewire_program, ow_offset);
}

// pio1 is time-shared with the SD card's SDIO programs: every transfer takes it back first.
static const pio_tenant_t ow_tenant = { ow_load, ow_unload };

bool onewire_init(uint8_t id, uint8_t data_pin) {
    if (id >= ONEWIRE_MAX_BUSES || ow_used[id])
        return false;
    int sm = pio_claim_unused_sm(ow_pio, false);
    if (sm < 0)
        return false;
    ow_sm[id] = (uint)sm;
    ow_pin[id] = data_pin;
    ow_used[id] = true;
    pio_share_leave(ow_pio, &ow_tenant);        // reload with the new bus included
    pio_share_enter(ow_pio, &ow_tenant);
    return true;
}

// A short reply leaves the SM on `wait 0 pin` for bits that never come; restart it at the top
static void ow_resync(uint sm) {
    pio_sm_set_enabled(ow_pio, sm, false);
    pio_sm_clear_fifos(ow_pio, sm);
    pio_sm_restart(ow_pio, sm);
    pio_sm_exec(ow_pio, sm, pio_encode_jmp(ow_offset));
    pio_sm_set_enabled(ow_pio, sm, true);
}

uint8_t onewire_transfer(uint8_t id, const uint8_t *request, uint8_t request_len, uint8_t *response,
                         uint8_t response_len) {
    if (id >= ONEWIRE_MAX_BUSES)
        return 0;
    if (!ow_used[id])
        return 0;
    pio_share_enter(ow_pio, &ow_tenant);
    uint sm = ow_sm[id];
    busy_wait_until(ow_free_at[id]);
    pio_sm_clear_fifos(ow_pio, sm);

    // First FIFO word tells the PIO how many response bytes to clock back in (count - 1).
    pio_sm_put_blocking(ow_pio, sm, ((response_len - 1) & 0x1F) << 24);

    // Request bytes are MSB-justified (the PIO shifts left out of the top byte).
    for (int i = 0; i < request_len; i++)
        pio_sm_put_blocking(ow_pio, sm, (uint32_t)request[i] << 24);

    // A timed-out byte reads 0 but isn't counted, so callers can tell silence from 0x00.
    uint8_t received = 0;
    for (int i = 0; i < response_len; i++) {
        absolute_time_t timeout = make_timeout_time_us(600);
        bool timed_out = false;
        while (pio_sm_is_rx_fifo_empty(ow_pio, sm) && !timed_out)
            timed_out = time_reached(timeout);
        if (timed_out) {
            response[i] = 0; // silent -- report zero rather than block forever
            continue;
        }
        uint32_t data = pio_sm_get(ow_pio, sm);
        response[i] = (uint8_t)(data & 0xFF);
        received++;
    }
    if (received < response_len) ow_resync(sm);

    // Checked by the next transfer instead of spun here, so a caller polling slower than this never waits.
    ow_free_at[id] = make_timeout_time_us(4 * (request_len + response_len) + 450);
    return received;
}
