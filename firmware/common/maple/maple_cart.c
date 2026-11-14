#include "maple_cart.h"

static int file_err(uint8_t *payload, uint8_t *resp, uint32_t code) {
    *resp = MAPLE_RESP_FILE_ERR;
    maple_put_word(payload, code);
    return 4;
}

// Lands in the reader's RAM copy; the cart itself is written behind it.
static int save_write(const maple_cart_t *cart, const uint8_t *req, unsigned req_len,
                      uint8_t *payload, uint8_t *resp) {
    if (!maple_has_words(req, req_len, 3)) { return MAPLE_REPLY_NONE; }
    uint32_t off = maple_data_word(req, 1);
    uint32_t want = maple_data_word(req, 2);
    uint32_t n = (maple_nwords(req) - 3U) * 4U;
    const uint8_t *src = req + 16;
    if (req_len < 16U + n) { return MAPLE_REPLY_NONE; }
    bool (*write)(uint32_t, uint32_t, const uint8_t *) =
        (off & MAPLE_CART_SPACES) == MAPLE_CART_SAVE ? cart->save_write
        : (off & MAPLE_CART_SPACES) == MAPLE_CART_CACHE ? cart->cache_write
        : (off & MAPLE_CART_SPACES) == MAPLE_CART_XCACHE ? cart->xcache_write : 0;
    if (!write || !n || !cart->crc) { return file_err(payload, resp, MAPLE_CART_BAD_ARG); }
    if (cart->crc(src, n) != want) { return file_err(payload, resp, MAPLE_CART_CRC_BAD); }
    if (!write(off & ~MAPLE_CART_SPACES, n, src)) { return file_err(payload, resp, MAPLE_CART_BAD_ARG); }
    *resp = MAPLE_CMD_RESP_ACK;
    return 0;
}

static int sync_cmd(const maple_cart_t *cart, const uint8_t *req, unsigned req_len, uint8_t *payload, uint8_t *resp) {
    if (!maple_has_words(req, req_len, 4)) { return MAPLE_REPLY_NONE; }
    if (!cart->sync || !cart->sync(maple_data_word(req, 1), maple_data_word(req, 2), maple_data_word(req, 3))) {
        return file_err(payload, resp, MAPLE_CART_NOT_READY);
    }
    *resp = MAPLE_CMD_RESP_ACK;
    return 0;
}

static int media_info(const maple_cart_t *cart, uint32_t func, uint8_t *payload, uint8_t *resp) {
    uint32_t pending = 0, given_up = 0;
    if (cart->save_status) { cart->save_status(&pending, &given_up); }
    *resp = MAPLE_CMD_RESP_DATA_XFER;
    maple_put_word(payload + 0, func);
    maple_put_word(payload + 4, cart->save_size ? cart->save_size() : 0);
    maple_put_word(payload + 8, pending);
    maple_put_word(payload + 12, given_up);
    maple_put_word(payload + 16, cart->rom_size ? cart->rom_size() : cart->rom_span);
    uint32_t w[MAPLE_CART_INFO_WORDS] = {0};
    if (cart->info) { cart->info(w); }
    for (unsigned i = 0; i < MAPLE_CART_INFO_WORDS; i++) { maple_put_word(payload + 20 + 4 * i, w[i]); }
    return 20 + 4 * (int)MAPLE_CART_INFO_WORDS;
}

static int block_read(const maple_cart_t *cart, uint32_t func, const uint8_t *req, unsigned req_len,
                      uint8_t *payload, uint8_t *resp) {
    if (!maple_has_words(req, req_len, 2)) { return MAPLE_REPLY_NONE; }
    uint32_t off = maple_data_word(req, 1);
    uint32_t n = maple_has_words(req, req_len, 3) ? maple_data_word(req, 2) : MAPLE_CART_DEFAULT_LEN;
    bool want_crc = maple_has_words(req, req_len, 4) && (maple_data_word(req, 3) & MAPLE_CART_CRC);
    uint32_t max = want_crc ? MAPLE_CART_CRC_MAX_LEN : MAPLE_CART_MAX_LEN;
    if (!n || (n & 3U) || n > max || off > UINT32_MAX - n || (want_crc && !cart->crc)) {
        return file_err(payload, resp, MAPLE_CART_BAD_ARG);
    }
    uint32_t space = off & MAPLE_CART_SPACES, o = off & ~MAPLE_CART_SPACES;
    if (space == MAPLE_CART_SAVE) {
        if (!cart->save_read || !cart->save_read(o, n, payload + 8)) {
            return file_err(payload, resp, MAPLE_CART_BAD_ARG);
        }
    } else if (space == MAPLE_CART_TABLES || space == MAPLE_CART_CACHE || space == MAPLE_CART_XCACHE) {
        uint32_t (*size_fn)(void) = space == MAPLE_CART_TABLES ? cart->tables_size
                                  : space == MAPLE_CART_CACHE ? cart->cache_size : cart->xcache_size;
        bool (*read_fn)(uint32_t, uint32_t, uint8_t *) = space == MAPLE_CART_TABLES ? cart->tables_read
                                                       : space == MAPLE_CART_CACHE ? cart->cache_read : cart->xcache_read;
        uint32_t size = size_fn ? size_fn() : 0;
        if (!read_fn || n > size || o > size - n) { return file_err(payload, resp, MAPLE_CART_BAD_ARG); }
        if (!read_fn(o, n, payload + 8)) { return file_err(payload, resp, MAPLE_CART_NOT_READY); }
    } else if (space) {
        return file_err(payload, resp, MAPLE_CART_BAD_ARG);
    } else if (n > cart->rom_span || off > cart->rom_span - n) {
        return file_err(payload, resp, MAPLE_CART_BAD_ARG);
    } else if (!cart->read(off, n, payload + 8)) {
        return file_err(payload, resp, MAPLE_CART_NOT_READY);
    }
    *resp = MAPLE_CMD_RESP_DATA_XFER;
    maple_put_word(payload + 0, func);
    maple_put_word(payload + 4, off);
    if (!want_crc) { return 8 + (int)n; }
    maple_put_word(payload + 8 + n, cart->crc(payload + 8, n));
    return 12 + (int)n;
}

static int cart_command(const maple_unit_t *u, const uint8_t *req, unsigned req_len,
                        uint8_t *payload, uint8_t *resp) {
    int len;
    if (maple_common_command(u, req, payload, resp, &len)) { return len; }

    const uint8_t cmd = maple_cmd(req);
    if (cmd != MAPLE_CMD_BLOCK_READ && cmd != MAPLE_CMD_BLOCK_WRITE && cmd != MAPLE_CMD_GET_MEDIA_INFO &&
        cmd != MAPLE_CMD_BLOCK_SYNC) {
        *resp = MAPLE_RESP_UNKNOWN_CMD;
        return 0;
    }
    if (!maple_has_words(req, req_len, 1)) { return MAPLE_REPLY_NONE; }
    const uint32_t func = u->id->func;
    if (maple_data_word(req, 0) != func) { *resp = MAPLE_RESP_UNKNOWN_FUNC; return 0; }

    const maple_cart_t *cart = (const maple_cart_t *)u->state;
    if (cmd == MAPLE_CMD_BLOCK_SYNC) { return sync_cmd(cart, req, req_len, payload, resp); }  // powers a cart back on
    if (cart->ready && !cart->ready()) { return file_err(payload, resp, MAPLE_CART_NOT_READY); }
    if (cmd == MAPLE_CMD_GET_MEDIA_INFO) { return media_info(cart, func, payload, resp); }
    if (cmd == MAPLE_CMD_BLOCK_WRITE) { return save_write(cart, req, req_len, payload, resp); }
    return block_read(cart, func, req, req_len, payload, resp);
}

void maple_cart_unit(maple_unit_t *u, const maple_cart_t *cart, const maple_devid_t *id, uint8_t addr) {
    u->addr    = addr;
    u->id      = id;
    u->command = cart_command;
    u->state   = (void *)cart;
}
