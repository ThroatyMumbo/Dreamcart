// PIF joybus: port 0 a standard controller with an optional 32 KB Controller Pak, ports 1-3 empty, channel 4 the
// cart's EEPROM (4 or 16 Kbit) or none.
#include "pif.h"
#include "os.h"

#define NORESP 0x80u  // CHNL_ERR_NORESP in the rx length byte
#define EEP_MAX 2048u

static uint16_t pad_buttons;
static int8_t pad_x, pad_y;
static uint8_t eeprom[EEP_MAX];
static uint32_t eep_blocks = 64;  // 4 Kbit until an image says otherwise
static int no_eeprom;
static uint8_t *pak;
void (*pif_on_eeprom_write)(uint32_t block, const uint8_t data[8]);
void (*pif_on_pak_write)(uint32_t off);

void pif_attach_pak(uint8_t *ram) { pak = ram; }

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)(v >> 16)), put16(p + 2, (uint16_t)v); }

// ares Gamepad::formatControllerPak for one bank, with a fixed serial in place of ares' random one
void pif_pak_format(uint8_t *ram) {
    for (uint32_t i = 0; i < PIF_PAK_BYTES; i++) ram[i] = 0;
    static const uint8_t areas[] = { 1, 3, 4, 6 };
    for (unsigned a = 0; a < sizeof areas; a++) {
        uint8_t *id = ram + areas[a] * 0x20;
        id[1] = 0x2A;
        put32(id + 4, 0x0004C643u);
        put32(id + 8, 0x0364A7E1u);
        put16(id + 0x18, 0x0001);  // device ID
        id[0x1A] = 1;              // banks
        uint16_t sum = 0, inv = 0;
        for (int h = 0; h < 14; h++) {
            uint16_t v = (uint16_t)(id[2 * h] << 8 | id[2 * h + 1]);
            sum += v, inv += (uint16_t)~v;
        }
        put16(id + 0x1C, sum);
        put16(id + 0x1E, inv);
    }
    for (uint32_t page = 1; page <= 2; page++) {  // inode table and its copy; pages 0-4 are the system's
        uint8_t *p = ram + 0x100 * page, sum = 0;
        for (uint32_t slot = 5; slot < 128; slot++) p[slot * 2 + 1] = 0x03;  // empty
        for (int i = 2; i < 0x100; i++) sum += ram[0x100 + i];
        p[1] = sum;
    }
}

void pif_set_pad(uint16_t buttons, int8_t x, int8_t y) {
    pad_buttons = buttons;
    pad_x = x;
    pad_y = y;
}

uint8_t *pif_eeprom(void) { return eeprom; }

void pif_load_eeprom(const uint8_t *img, uint32_t len) {
    if (len != 512 && len != EEP_MAX) os_die("pif: EEPROM image is neither 512 nor 2048 bytes", len);
    for (uint32_t i = 0; i < len; i++) eeprom[i] = img[i];
    eep_blocks = len / 8;
    no_eeprom = 0;
}

uint32_t pif_eeprom_bytes(void) { return eep_blocks * 8; }

void pif_no_eeprom(void) { no_eeprom = 1; }

static int need(uint8_t got_tx, uint8_t got_rx, uint8_t tx, uint8_t rx, uint8_t cmd) {
    if (got_tx != tx || got_rx != rx) os_die("pif: joybus command with unexpected lengths", cmd << 16 | got_tx << 8 | got_rx);
    return 1;
}

// The accessory port's CRC over 32 data bytes (ares PIF::dataCRC)
static uint8_t data_crc(const uint8_t *d) {
    uint8_t crc = 0;
    for (int i = 0; i < 33; i++)
        for (int j = 7; j >= 0; j--) {
            uint8_t x = crc & 0x80 ? 0x85 : 0x00;
            crc = (uint8_t)(crc << 1 | (i < 32 && (d[i] >> j & 1)));
            crc ^= x;
        }
    return crc;
}

// The 5-bit CRC under a pak address's low bits (ares PIF::addressCRC)
static uint8_t addr_crc(uint16_t a) {
    uint8_t crc = 0;
    for (int i = 0; i < 16; i++, a <<= 1) {
        uint8_t x = crc & 0x10 ? 0x15 : 0x00;
        crc = (uint8_t)((crc << 1 | (a >> 15 & 1)) ^ x) & 0x1F;
    }
    return crc;
}

// Fills rx and returns 1, or returns 0 for no device on the channel.
static int command(int ch, const uint8_t *tx, uint8_t ntx, uint8_t *rx, uint8_t nrx) {
    uint8_t cmd = tx[0];
    if ((ch >= 1 && ch <= 3) || (ch == 4 && no_eeprom)) return 0;
    if (ch > 4) os_die("pif: joybus command past channel 4", ch);
    if (cmd == 0x00 || cmd == 0xFF) {
        need(ntx, nrx, 1, 3, cmd);
        rx[0] = ch == 4 ? 0x00 : 0x05;  // EEPROM 4 Kbit 0x0080, 16 Kbit 0x00C0; controller 0x0500
        rx[1] = ch == 4 ? (eep_blocks == 64 ? 0x80 : 0xC0) : 0x00;
        rx[2] = ch == 4 ? 0x00 : pak ? 0x01 : 0x02;  // controller: pak in, or none
        return 1;
    }
    if (ch == 0 && cmd == 0x01) {
        need(ntx, nrx, 1, 4, cmd);
        rx[0] = (uint8_t)(pad_buttons >> 8);
        rx[1] = (uint8_t)pad_buttons;
        rx[2] = (uint8_t)pad_x;
        rx[3] = (uint8_t)pad_y;
        return 1;
    }
    // With no pak or a bad address CRC the data CRC comes back inverted, so libultra rechecks the status and gets "no
    // pak" (DK64's osMotorInit probes the Rumble Pak this way). Past 0x7FFF a Controller Pak reads 0: not a Rumble Pak
    if (ch == 0 && (cmd == 0x02 || cmd == 0x03)) {
        cmd == 0x02 ? need(ntx, nrx, 3, 33, cmd) : need(ntx, nrx, 35, 1, cmd);
        uint16_t a = (uint16_t)(tx[1] << 8 | tx[2]) & ~31u;
        uint8_t bad = !pak || addr_crc(a) != (tx[2] & 0x1F) ? 0xFF : 0x00;
        if (cmd == 0x02) {
            for (int i = 0; i < 32; i++) rx[i] = !bad && a < PIF_PAK_BYTES ? pak[a + i] : 0;
            rx[32] = data_crc(rx) ^ bad;
            return 1;
        }
        if (!bad && a < PIF_PAK_BYTES) {  // 0x8000 is the bank select, and this pak has one bank
            int changed = 0;
            for (int i = 0; i < 32; i++) changed |= pak[a + i] ^ tx[3 + i], pak[a + i] = tx[3 + i];
            if (changed && pif_on_pak_write) pif_on_pak_write(a);
        }
        rx[0] = data_crc(tx + 3) ^ bad;
        return 1;
    }
    if (ch == 4 && cmd == 0x04) {
        need(ntx, nrx, 2, 8, cmd);
        for (int i = 0; i < 8; i++) rx[i] = eeprom[(tx[1] & (eep_blocks - 1)) * 8 + i];
        return 1;
    }
    if (ch == 4 && cmd == 0x05) {
        need(ntx, nrx, 10, 1, cmd);
        uint32_t b = tx[1] & (eep_blocks - 1);
        for (int i = 0; i < 8; i++) eeprom[b * 8 + i] = tx[2 + i];
        if (pif_on_eeprom_write) pif_on_eeprom_write(b, eeprom + b * 8);
        rx[0] = 0x00;
        return 1;
    }
    os_die("pif: joybus command the model doesn't cover", ch << 8 | cmd);
    return 0;
}

static int formatted;

void pif_write(uint8_t ram[64]) {
    if (!(ram[63] & 1)) return;
    formatted = 1;
    ram[63] &= ~1u;
}

void pif_read(uint8_t ram[64]) {
    if (!formatted) return;
    int ch = 0;
    for (unsigned i = 0; i < 63 && ch < 6;) {
        uint8_t b = ram[i];
        if (b == 0xFE) break;
        if (b == 0xFF || b == 0xFD) {
            i++;
            continue;
        }
        if (b == 0x00) {
            i++;
            ch++;
            continue;
        }
        uint8_t ntx = b & 0x3F, nrx = ram[i + 1] & 0x3F;
        // libultra's pak blocks are padded to 40 bytes, so the PIF sees a stray stack byte before 0xFE; as in ares, a
        // block that runs past PIF RAM ends the scan
        if (i + 2 + ntx + nrx > 63) break;
        if (!command(ch, ram + i + 2, ntx, ram + i + 2 + ntx, nrx)) ram[i + 1] |= NORESP;
        i += 2 + ntx + nrx;
        ch++;
    }
}
