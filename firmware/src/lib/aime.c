/*
 * AIME Reader
 * WHowe <github.com/whowechina>
 * 
 * Use NFC Module to read AIME
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"

#include "nfc.h"
#include "aime.h"

static bool debug = false;
#define DEBUG(...) if (nfc_runtime.debug) printf(__VA_ARGS__)

#define AIME_EXPIRE_US (1200 * 1000000ULL)
#define AIME_FAST_EXPIRE_US (3 * 1000000ULL)
#define FELICA_SYSCODE_AIC 0x88B4
#define FELICA_SYSCODE_SUICA 0x0003
#define FELICA_SYSCODE_LITE_S 0xFE00

static void fill_felica_session_nonce(uint8_t *data)
{
    uint32_t x = time_us_32();
    for (uint8_t i = 0; i < 8; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        data[i] = x & 0xff;
    }
    memset(data + 8, 0, 8);
}

enum {
    CMD_GET_FW_VERSION = 0x30,
    CMD_GET_HW_VERSION = 0x32,

    // Card read
    CMD_START_POLLING = 0x40,
    CMD_STOP_POLLING = 0x41,
    CMD_CARD_DETECT = 0x42,
    CMD_CARD_SELECT = 0x43,
    CMD_CARD_HALT = 0x44,

    // MIFARE
    CMD_MIFARE_KEY_SET_A = 0x50,
    CMD_MIFARE_AUTHORIZE_A = 0x51,
    CMD_MIFARE_READ = 0x52,
    CMD_MIFARE_WRITE = 0x53,
    CMD_MIFARE_KEY_SET_B = 0x54,
    CMD_MIFARE_AUTHORIZE_B = 0x55,

    // Boot,update
    CMD_TO_UPDATER_MODE = 0x60,
    CMD_SEND_HEX_DATA = 0x61,
    CMD_TO_NORMAL_MODE = 0x62,
    CMD_SEND_BINDATA_INIT = 0x63,
    CMD_SEND_BINDATA_EXEC = 0x64,

    // FeliCa
    CMD_FELICA_PUSH = 0x70,
    CMD_FELICA_OP = 0x71,

    // LED board
    CMD_EXT_BOARD_LED_RGB = 0x81,
    CMD_EXT_BOARD_INFO = 0xf0,
    CMD_EXT_TO_NORMAL_MODE = 0xf5,
};

enum {
    STATUS_OK = 0,
    STATUS_CARD_ERROR = 1,
    STATUS_INVALID_COMMAND = 3,
};

enum {
    FELICA_POLLING = 0x00,
    FELICA_READ_WITHOUT_ENCRYPTION = 0x06,
    FELICA_WRITE_WITHOUT_ENCRYPTION = 0x08,
    FELICA_REQUEST_SYSTEM_CODE = 0x0c,
    FELICA_ACTIVE2 = 0xa4,
};

typedef struct {
    const uint8_t *data;
    uint8_t len;
} binary_payload_t;

static const uint8_t fw_version_0[] = "\x92";
static const uint8_t fw_version_1[] = "\x94";
static const uint8_t hw_version_0[] = "837-15286EXP";
static const uint8_t hw_version_1[] = "837-15396";
static const uint8_t led_info_0[] = "000-00000\xFF\x11\x40";
static const uint8_t led_info_1[] = "000-00000\xFF\x11\x40";

static const binary_payload_t fw_version[] = {
    { fw_version_0, sizeof(fw_version_0) - 1 },
    { fw_version_1, sizeof(fw_version_1) - 1 },
};
static const binary_payload_t hw_version[] = {
    { hw_version_0, sizeof(hw_version_0) - 1 },
    { hw_version_1, sizeof(hw_version_1) - 1 },
};
static const binary_payload_t led_info[] = {
    { led_info_0, sizeof(led_info_0) - 1 },
    { led_info_1, sizeof(led_info_1) - 1 },
};
static int ver_mode = 1;
static bool expecting_dtr_off = false;
static uint64_t expected_dtr_off_time = 0;
static nfc_card_t last_felica_card;
static bool has_last_felica_card = false;
static uint8_t virtual_aic_session[16];
static bool virtual_aic_session_valid = false;

static struct {
    bool enabled;
    bool active; // currently active
    uint8_t idm[8];
    const uint8_t pmm[8];
    const uint8_t syscode[2];
} virtual_aic = { false, false,
                  "", "\x00\xf1\x00\x00\x00\x01\x43\x00", "\x88\xb4" };

static uint16_t felica_syscode_value(const uint8_t syscode[2])
{
    return ((uint16_t)syscode[0] << 8) | syscode[1];
}

static bool felica_card_is_aic(const nfc_card_t *card)
{
    return felica_syscode_value(card->syscode) == FELICA_SYSCODE_AIC;
}

static bool felica_card_is_suica_compatible(const nfc_card_t *card)
{
    uint16_t syscode = felica_syscode_value(card->syscode);
    return (syscode == FELICA_SYSCODE_SUICA) || (syscode == FELICA_SYSCODE_LITE_S);
}

static void refresh_virtual_aic_session(const uint8_t seed[16])
{
    uint32_t x = time_us_32();
    for (uint8_t i = 0; i < 16; i++) {
        x ^= (uint32_t)seed[i] << ((i & 3) * 8);
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
    }

    for (uint8_t i = 0; i < 8; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        virtual_aic_session[i] = x & 0xff;
    }
    memset(virtual_aic_session + 8, 0, 8);
    virtual_aic_session_valid = true;
}

static void putc_trap(uint8_t byte)
{
}

static aime_putc_func aime_putc = putc_trap;

void aime_sub_mode(int sub_mode)
{
    ver_mode = (sub_mode == 0) ? 0 : 1;
}

const char *aime_get_mode_string()
{
    return (const char *)hw_version[ver_mode].data;
}

void aime_init(aime_putc_func putc_func)
{
    aime_putc = putc_func;
}

void aime_virtual_aic(bool enable)
{
    virtual_aic.enabled = enable;
}

static uint8_t mifare_keys[2][6]; // 'KeyA' and 'KeyB'

static union __attribute__((packed)) {
    struct {
        uint8_t len;
        uint8_t addr;
        uint8_t seq;
        uint8_t cmd;
        uint8_t status;
        uint8_t payload_len;
        uint8_t payload[];
    };
    uint8_t raw[256];
} response;

static union __attribute__((packed)) {
    struct {
        uint8_t len;
        uint8_t addr;
        uint8_t seq;
        uint8_t cmd;
        uint8_t payload_len;
        union {
            struct {
                uint8_t uid[4];
                uint8_t block_id;
            } mifare;
            struct {
                uint8_t idm[8];
                uint8_t len;
                uint8_t code;
                uint8_t data[0];
            } felica;
            uint8_t payload[250];
        };
    };
    uint8_t raw[256];
} request;

static struct {
    bool active;
    uint8_t len;
    uint8_t check_sum;
    bool escaping;
    uint64_t time;
} req_ctx;

static void build_response(int payload_len)
{
    response.len = payload_len + 6;
    response.addr = request.addr;
    response.seq = request.seq;
    response.cmd = request.cmd;
    response.status = STATUS_OK;
    response.payload_len = payload_len;
}

static void send_response()
{
    uint8_t checksum = 0;
    for (int i = 0; i < response.len; i++) {
        checksum += response.raw[i];
    }
    response.raw[response.len] = checksum;

    aime_putc(0xe0); // sync

    for (int i = 0; i < response.len + 1; i++) {
        uint8_t c = response.raw[i];
        if (c == 0xe0 || c == 0xd0) {
            aime_putc(0xd0); // escape
            c--;
        }
        aime_putc(c);
    }

    DEBUG("\n\033[33m%6ld<< %02x:", time_us_32() / 1000, response.cmd);
    for (int i = 0; i < response.payload_len; i++) {
        DEBUG(" %02x", response.payload[i]);
    }
    DEBUG("\033[0m");
}

static void send_simple_response(uint8_t status)
{
    build_response(0);
    response.status = status;
    send_response();
}

static void cmd_to_normal_mode()
{
    send_simple_response(STATUS_INVALID_COMMAND);
}

static void cmd_binary_payload(const binary_payload_t version[])
{
    build_response(version[ver_mode].len);
    memcpy(response.payload, version[ver_mode].data, version[ver_mode].len);
    send_response();
}

static void cmd_key_set(uint8_t key[6])
{
    memcpy(key, request.payload, 6);
    send_simple_response(STATUS_OK);
}

static void cmd_set_polling(bool enabled)
{
    nfc_rf_field(enabled);
    send_simple_response(STATUS_OK);
}

typedef struct __attribute__((packed)) {
    uint8_t count;
    uint8_t type;
    uint8_t id_len;
    union {
        struct {
            uint8_t idm[8];
            uint8_t pmm[8];
        };
        uint8_t uid[6];
    };
} card_info_t;

static void handle_mifare_card(const uint8_t *uid, int len)
{
    card_info_t *card = (card_info_t *) response.payload;

    build_response(len > 4 ? 10 : 7);

    card->count = 1;
    card->type = 0x10;
    card->id_len = len;
    memcpy(card->uid, uid, len);

    printf("\nMIFARE Card:");
    for (int i = 0; i < len; i++) {
        printf(" %02x", uid[i]);
    }
}

static void handle_felica_card(const uint8_t idm[8], const uint8_t pmm[8])
{
    build_response(19);
    card_info_t *card = (card_info_t *) response.payload;

    card->count = 1;
    card->type = 0x20;
    card->id_len = 16;
    memcpy(card->idm, idm, 8);
    memcpy(card->pmm, pmm, 8);
}

static void fake_felica_card()
{
    build_response(19);
    card_info_t *card = (card_info_t *) response.payload;

    card->count = 1;
    card->type = 0x20;
    card->id_len = 16;
    memcpy(card->idm, virtual_aic.idm, 8);
    memcpy(card->pmm, virtual_aic.pmm, 8);
}

static void activate_virtual_aic(const uint8_t idm[8])
{
    virtual_aic.active = true;
    virtual_aic_session_valid = false;
    memcpy(virtual_aic.idm, idm, 8);
}

static void handle_no_card()
{
    build_response(1);
    card_info_t *card = (card_info_t *) response.payload;

    card->count = 0;
    response.status = STATUS_OK;
}

static void cmd_detect_card()
{
    nfc_card_t card = nfc_detect_card();
    if (debug) {
        display_card(&card);
    }

    switch (card.card_type) {
        case NFC_CARD_MIFARE:
            if (virtual_aic.enabled) {
                printf("\nVirtual FeliCa from MIFARE.");
                memcpy(virtual_aic.idm, "\x01\x01", 2);
                if (card.len == 4) {
                    memcpy(virtual_aic.idm + 2, card.uid, 4);
                    memcpy(virtual_aic.idm + 6, card.uid, 2);
                } else if (card.len == 7) {
                    memcpy(virtual_aic.idm + 2, card.uid, 6);
                }
                virtual_aic.active = true;
                virtual_aic_session_valid = false;
                fake_felica_card();
            } else {
                handle_mifare_card(card.uid, card.len);
            }
            break;
        case NFC_CARD_FELICA:
            if (virtual_aic.enabled && !felica_card_is_aic(&card)) {
                printf(felica_card_is_suica_compatible(&card)
                       ? "\nVirtual AIC from Suica/FeliCa phone."
                       : "\nVirtual AIC from FeliCa.");
                activate_virtual_aic(card.uid);
                fake_felica_card();
            } else {
                virtual_aic.active = false;
                handle_felica_card(card.uid, card.pmm);
            }
            break;
        case NFC_CARD_VICINITY:
            if (virtual_aic.enabled) {
                printf("\nVirtual FeliCa from 15693.");
                memcpy(virtual_aic.idm, card.uid, 8);
                virtual_aic.idm[0] = 0x01;
                virtual_aic.active = true;
                virtual_aic_session_valid = false;
                fake_felica_card();
            }
            break;
        default:
            virtual_aic.active = false;
            handle_no_card();
            break;
    }

    send_response();
}

static void cmd_card_select()
{
    send_simple_response(STATUS_OK);
}

static void cmd_mifare_auth(int type)
{
    const uint8_t *key = mifare_keys[type];
    nfc_mifare_auth(request.mifare.uid, request.mifare.block_id,
                    type, key);
    send_simple_response(STATUS_OK);
}

static void cmd_mifare_read()
{
    build_response(16);
    memset(response.payload, 0, 16);
    nfc_mifare_read(request.mifare.block_id, response.payload);
    send_response();
}

static void cmd_mifare_halt()
{
    send_simple_response(STATUS_OK);
}

typedef struct __attribute__((packed)) {
    uint8_t idm[8];
    uint8_t service_count;
    uint8_t service_code[2];
    uint8_t block_count;
    uint8_t block_list[][2];
} felica_read_request_t;

typedef struct __attribute__((packed)) {
    uint8_t idm[8];
    uint8_t service_count;
    uint8_t service_code[2];
    uint8_t block_count;
    uint8_t block_list[1][2];
    uint8_t block_data[16];
} felica_write_request_t;

static bool fill_aime_felica_block(uint16_t service, uint16_t block, const uint8_t idm[8], uint8_t block_data[16])
{
    if (service != 0x000b) {
        return false;
    }

    memset(block_data, 0, 16);

    switch (block) {
        case 0x8082:
            memcpy(block_data, idm, 8);
            block_data[9] = 0x78;
            return true;

        case 0x8086:
            block_data[1] = 0x01;
            return true;

        case 0x8090:
            return true;

        default:
            return false;
    }
}

static bool fill_virtual_aic_felica_block(uint16_t service, uint16_t block,
                                          const uint8_t idm[8],
                                          uint8_t block_data[16])
{
    if (fill_aime_felica_block(service, block, idm, block_data)) {
        return true;
    }

    if ((service == 0x000b) && (block == 0x8091)) {
        if (!virtual_aic_session_valid) {
            uint8_t seed[16] = { 0 };
            refresh_virtual_aic_session(seed);
        }
        memcpy(block_data, virtual_aic_session, 16);
        return true;
    }

    return false;
}

static bool poll_felica_card(nfc_card_t *card)
{
    if (virtual_aic.enabled && virtual_aic.active) {
        memset(card, 0, sizeof(*card));
        card->card_type = NFC_CARD_FELICA;
        card->len = 8;
        memcpy(card->uid, virtual_aic.idm, 8);
        memcpy(card->pmm, virtual_aic.pmm, 8);
        memcpy(card->syscode, virtual_aic.syscode, 2);
        return true;
    }

    *card = nfc_detect_card_ex(false, true, false);
    return card->card_type == NFC_CARD_FELICA;
}

static void cmd_felica_push()
{
    send_simple_response(STATUS_INVALID_COMMAND);
    expecting_dtr_off = true;
    expected_dtr_off_time = time_us_64() + 50000ULL;
}

static void cmd_felica_op()
{
    uint8_t code = request.felica.code;
    nfc_card_t card;
    if (!poll_felica_card(&card)) {
        if ((code != FELICA_READ_WITHOUT_ENCRYPTION) &&
            (code != FELICA_WRITE_WITHOUT_ENCRYPTION) &&
            !has_last_felica_card) {
            send_simple_response(STATUS_CARD_ERROR);
            return;
        }

        memset(&card, 0, sizeof(card));
        card.card_type = NFC_CARD_FELICA;
        card.len = 8;
        memcpy(card.uid, request.felica.idm, 8);
        if (has_last_felica_card) {
            memcpy(card.pmm, last_felica_card.pmm, 8);
            memcpy(card.syscode, last_felica_card.syscode, 2);
        }
    } else {
        last_felica_card = card;
        has_last_felica_card = true;
    }

    memset(response.payload, 0, 128);

    switch (code) {
        case FELICA_POLLING:
            build_response(0x14);
            response.payload[1] = code + 1;
            memcpy(response.payload + 2, card.uid, 8);
            memcpy(response.payload + 10, card.pmm, 8);
            memcpy(response.payload + 18, card.syscode, 2);
            break;

        case FELICA_REQUEST_SYSTEM_CODE:
            build_response(0x0d);
            response.payload[1] = code + 1;
            memcpy(response.payload + 2, card.uid, 8);
            response.payload[10] = 1;
            memcpy(response.payload + 11, card.syscode, 2);
            break;

        case FELICA_ACTIVE2:
            build_response(0x0b);
            response.payload[1] = code + 1;
            memcpy(response.payload + 2, card.uid, 8);
            response.payload[10] = 0;
            break;

        case FELICA_READ_WITHOUT_ENCRYPTION: {
            felica_read_request_t *read = (felica_read_request_t *)request.felica.data;
            uint8_t block_count = read->block_count;
            if (block_count > 8) {
                block_count = 8;
            }

            build_response(0x0d + block_count * 16);
            response.payload[1] = code + 1;
            memcpy(response.payload + 2, card.uid, 8);
            response.payload[10] = 0;
            response.payload[11] = 0;
            response.payload[12] = block_count;

            uint16_t service = ((uint16_t)read->service_code[1] << 8) | read->service_code[0];
            uint16_t blocks[8];
            for (uint8_t i = 0; i < block_count; i++) {
                blocks[i] = ((uint16_t)read->block_list[i][0] << 8) | read->block_list[i][1];
            }

            if (!virtual_aic.active && (block_count > 1)) {
                if (nfc_felica_read_blocks(service, block_count, blocks,
                                           (uint8_t (*)[16])(response.payload + 13))) {
                    break;
                }
            }

            for (uint8_t i = 0; i < block_count; i++) {
                uint16_t block = blocks[i];
                uint8_t *block_data = response.payload + 13 + i * 16;
                if (virtual_aic.active &&
                    fill_virtual_aic_felica_block(service, block, card.uid, block_data)) {
                    continue;
                }

                if (fill_aime_felica_block(service, block, card.uid, block_data)) {
                    continue;
                }

                if (virtual_aic.active || !nfc_felica_read(service, block, block_data)) {
                    if ((service == 0x000b) && (block == 0x8091)) {
                        fill_felica_session_nonce(block_data);
                    } else {
                        memset(block_data, 0, 16);
                    }
                }
            }
            break;
        }

        case FELICA_WRITE_WITHOUT_ENCRYPTION: {
            felica_write_request_t *write = (felica_write_request_t *)request.felica.data;
            uint16_t service = ((uint16_t)write->service_code[1] << 8) | write->service_code[0];
            uint16_t block = ((uint16_t)write->block_list[0][0] << 8) | write->block_list[0][1];
            if (write->block_count > 0) {
                if (virtual_aic.active && (service == 0x0009) && (block == 0x8080)) {
                    refresh_virtual_aic_session(write->block_data);
                } else if (!virtual_aic.active) {
                    nfc_felica_write(service, block, write->block_data);
                }
            }

            build_response(0x0c);
            response.payload[1] = code + 1;
            memcpy(response.payload + 2, card.uid, 8);
            response.payload[10] = 0x00;
            response.payload[11] = 0x00;
            break;
        }

        default:
            build_response(0);
            response.status = STATUS_INVALID_COMMAND;
            break;
    }

    if (response.payload_len > 0) {
        response.payload[0] = response.payload_len;
    }
    send_response();
}

static uint32_t led_color;

static void cmd_led_rgb()
{
    uint8_t r = request.payload[0];
    uint8_t g = request.payload[1];
    uint8_t b = request.payload[2];
    led_color = r << 16 | g << 8 | b;

    build_response(0);
    send_response();

    expecting_dtr_off = true;
    expected_dtr_off_time = time_us_64() + 400000ULL;
}

static void handle_frame()
{
    DEBUG("\n\033[32mAime %d:%02x >>", request.payload_len, request.cmd);
    for (int i = 0; i < request.payload_len; i++) {
        DEBUG(" %02x", request.payload[i]);
    }
    DEBUG("\033[0m");

    switch (request.cmd) {
        case CMD_TO_NORMAL_MODE:
            DEBUG("\nAIME: cmd_to_normal");
            cmd_to_normal_mode();
            break;
        case CMD_GET_FW_VERSION:
            DEBUG("\nAIME: fw_version");
            cmd_binary_payload(fw_version);
            break;
        case CMD_GET_HW_VERSION:
            DEBUG("\nAIME: hw_version");
            cmd_binary_payload(hw_version);
            break;
        case CMD_MIFARE_KEY_SET_A:
            DEBUG("\nAIME: key A");
            cmd_key_set(mifare_keys[0]);
            break;
        case CMD_MIFARE_KEY_SET_B:
            DEBUG("\nAIME: key B");
            cmd_key_set(mifare_keys[1]);
            break;

        case CMD_START_POLLING:
            cmd_set_polling(true);
            break;
        case CMD_STOP_POLLING:
            cmd_set_polling(false);
            break;
        case CMD_CARD_DETECT:
            cmd_detect_card();
            break;

        case CMD_FELICA_PUSH:
            DEBUG("\nAIME: felica push");
            cmd_felica_push();
            break;
        case CMD_FELICA_OP:
            DEBUG("\nAIME: felica op");
            cmd_felica_op();
            break;

        case CMD_CARD_SELECT:
            DEBUG("\nAIME: card select");
            cmd_card_select();
            break;
        
        case CMD_MIFARE_AUTHORIZE_A:
            DEBUG("\nAIME: auth A");
            cmd_mifare_auth(0);
            break;

        case CMD_MIFARE_AUTHORIZE_B:
            DEBUG("\nAIME: auth B");
            cmd_mifare_auth(1);
            break;
        
        case CMD_MIFARE_READ:
            DEBUG("\nAIME: mifare read");
            cmd_mifare_read();
            break;

        case CMD_CARD_HALT:
            DEBUG("\nAIME: mifare halt");
            cmd_mifare_halt();
            break;

        case CMD_EXT_BOARD_INFO:
            DEBUG("\nAIME: led info");
            cmd_binary_payload(led_info);
            break;
        case CMD_EXT_BOARD_LED_RGB:
            DEBUG("\nAIME: led rgb");
            cmd_led_rgb();
            break;

        case CMD_SEND_HEX_DATA:
        case CMD_EXT_TO_NORMAL_MODE:
            DEBUG("\nAIME: hex data or ex to normal: %d", request.cmd);
            send_simple_response(STATUS_OK);
            break;

        default:
            DEBUG("\nUnknown command: %02x [", request.cmd);
            for (int i = 0; i < request.len; i++) {
                DEBUG(" %02x", request.raw[i]);
            }
            DEBUG("]");
            send_simple_response(STATUS_OK);
            break;
    }
}

static uint64_t expire_time;

bool aime_feed(int c)
{
    if (c == 0xe0) {
        req_ctx.active = true;
        req_ctx.len = 0;
        req_ctx.check_sum = 0;
        req_ctx.escaping = false;
        req_ctx.time = time_us_64();
        return true;
    }

    if (!req_ctx.active) {
        return false;
    }

    if (c == 0xd0) {
        req_ctx.escaping = true;
        return true;
    }

    if (req_ctx.escaping) {
        c++;
        req_ctx.escaping = false;
    }

    if (req_ctx.len != 0 && req_ctx.len == request.len) {
        if (req_ctx.check_sum == c) {
            handle_frame();
            req_ctx.active = false;
            expire_time = time_us_64() + AIME_EXPIRE_US;
        }
        return true;
    }

    request.raw[req_ctx.len] = c;
    req_ctx.len++;
    req_ctx.check_sum += c;

    return true;
}

bool aime_is_active()
{
    return time_us_64() < expire_time;
}

void aime_dtr_off()
{
    if ((expecting_dtr_off) &&
        (abs(time_us_64() - expected_dtr_off_time) < 70000ULL)) {
        expecting_dtr_off = false;
        return;
    }

    if (!aime_is_active()) {
        return;
    }

    DEBUG("\nAIME: DTR_OFF delta: %lld", time_us_64() - expected_dtr_off_time);
    expire_time = time_us_64() + AIME_FAST_EXPIRE_US;
}

uint32_t aime_led_color()
{
    return led_color;
}
