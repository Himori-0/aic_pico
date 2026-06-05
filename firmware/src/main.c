/*
 * Controller Main
 * WHowe <github.com/whowechina>
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "pico/flash.h"
#include "bsp/board.h"
#include "pico/multicore.h"
#include "pico/bootrom.h"

#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/clocks.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"

#include "board_defs.h"

#include "tusb.h"
#include "usb_descriptors.h"

#include <nfc.h>
#include <mode.h>
#include <aime.h>
#include <bana.h>

#include "save.h"
#include "config.h"
#include "cli.h"
#include "commands.h"
#include "light.h"
#ifndef AIC_BASE_ONLY
#include "cardio.h"
#include "keypad.h"
#include "lis3dh.h"
#include "gui.h"
#endif

#define DEBUG(...) if (aic_runtime.debug) printf(__VA_ARGS__)

#ifndef AIC_BASE_ONLY
struct __attribute__((packed)) {
    uint8_t modifier;
    uint8_t keymap[15];
} hid_nkro;

static const char keymap[12] = KEYPAD_NKRO_MAP;

static inline void set_nkro_bit(uint8_t code)
{
    int byte = code / 8;
    if (byte < sizeof(hid_nkro.keymap)) {
        int bit = code % 8;
        hid_nkro.keymap[byte] |= (1 << (bit));
    }
}

void report_hid_key()
{
    if (!tud_hid_ready()) {
        return;
    }

    uint16_t keys = aic_runtime.touch ? gui_keypad_read() : keypad_read();

    memset(&hid_nkro, 0, sizeof(hid_nkro));

    if (cardio_autopin_rolling()) {
        int auto_pin_key = cardio_get_pin_key();
        if (auto_pin_key > 0) {
            set_nkro_bit(auto_pin_key);
        }
    } else {
        for (int i = 0; i < keypad_key_num(); i++) {
            if (keys & (1 << i)) {
                set_nkro_bit(keymap[i]);
            }
        }
    }

    tud_hid_n_report(1, 0, &hid_nkro, sizeof(hid_nkro));
}

void report_usb_hid()
{
    cardio_report_cardio();
    report_hid_key();
}

static uint64_t last_hid_time = 0;
#endif

static bool hid_light_is_active()
{
#ifdef AIC_BASE_ONLY
    return false;
#else
    if (last_hid_time == 0) {
        return false;
    }
    return (time_us_64() - last_hid_time) < 2000000;
#endif
}

static bool reader_is_active()
{
    return aime_is_active() || bana_is_active();
}

static void light_mode_update()
{
    static bool was_cardio = true;
    bool cardio = !reader_is_active() && !hid_light_is_active();
    static uint8_t last_level;
    bool level_changed = (last_level != aic_cfg->light.level_idle);

    if (cardio && (!was_cardio || level_changed)) {
        light_rainbow(1, 1000, aic_cfg->light.level_idle);
        last_level = aic_cfg->light.level_idle;
    }

    was_cardio = cardio;
}

static void core1_init()
{
    flash_safe_execute_core_init();

#ifndef AIC_BASE_ONLY
    if (aic_runtime.touch) {
        gui_init();
        gui_level(aic_cfg->lcd.backlight);
    }
#endif
}

static void core1_loop()
{
    uint64_t next_frame = 0;

    core1_init();

    while (1) {
#ifndef AIC_BASE_ONLY
        if (aic_runtime.touch) {
            gui_loop();
        }
#endif
        light_update();

        light_mode_update();

        cli_fps_count(1);
        sleep_until(next_frame);
        next_frame = time_us_64() + 999; // no faster than 1000Hz
    }
}

void card_name_update_cb(nfc_card_name card_name)
{
#ifndef AIC_BASE_ONLY
    gui_report_card_name(card_name);
#else
    (void)card_name;
#endif
}

const int reader_intf = 1;
static struct {
    uint8_t buf[64];
    int pos;
} reader;

static void cdc_reader_putc(uint8_t byte)
{
    tud_cdc_n_write(reader_intf, &byte, 1);
    tud_cdc_n_write_flush(reader_intf);
}

static void reader_poll_data()
{
    if (tud_cdc_n_available(reader_intf)) {
        int count = tud_cdc_n_read(reader_intf, reader.buf + reader.pos,
                                   sizeof(reader.buf) - reader.pos);
        if (count > 0) {
            uint32_t now = time_us_32();
            DEBUG("\n\033[32m%6ld>>", now / 1000);
            for (int i = 0; i < count; i++) {
                DEBUG(" %02X", reader.buf[reader.pos + i]);
            }
            DEBUG("\033[0m");
            reader.pos += count;
        }
    }
}

static void reader_detect_mode()
{
    if (aic_cfg->reader.mode == MODE_AUTO) {
        static bool was_active = true; // so first time mode will be cleared
        bool is_active = aime_is_active() || bana_is_active();
        if (was_active && !is_active) {
            aic_runtime.mode = MODE_NONE;
        }
        was_active = is_active;
    } else {
        aic_runtime.mode = aic_cfg->reader.mode;
    }

    if (aic_runtime.mode == MODE_NONE) {
        cdc_line_coding_t coding;
        tud_cdc_n_get_line_coding(reader_intf, &coding);
        aic_runtime.mode = mode_detect(reader.buf, reader.pos, coding.bit_rate);
        if ((reader.pos > 10) && (aic_runtime.mode == MODE_NONE)) {
            reader.pos = 0; // drop the buffer
        }
    }

}

static void reader_light()
{
    static uint32_t old_color = 0;
    if (aime_is_active()) {
        uint32_t color = aime_led_color();
        if (old_color != color) {
            light_fade(color, 100);
            old_color = color;
        }
    } else if (bana_is_active()) {
        light_fade_s(bana_get_led_pattern());
    }
}

static int aime_sub_mode_for_current_baud(reader_mode_t mode)
{
    cdc_line_coding_t coding;
    tud_cdc_n_get_line_coding(reader_intf, &coding);

    if (coding.bit_rate == 38400) {
        return 0;
    }
    if (coding.bit_rate == 115200) {
        return 1;
    }

    return mode == MODE_AIME0 ? 0 : 1;
}

static void reader_run()
{
    reader_poll_data();
    reader_detect_mode();

    if (reader.pos > 0) {
        uint8_t buf[64];
        memcpy(buf, reader.buf, reader.pos);
        int count = reader.pos;
        switch (aic_runtime.mode) {
            case MODE_AIME0:
            case MODE_AIME1:
                reader.pos = 0;
                aime_sub_mode(aime_sub_mode_for_current_baud(aic_runtime.mode));
                for (int i = 0; i < count; i++) {
                    aime_feed(buf[i]);
                }
                break;
            case MODE_BANA:
                reader.pos = 0;
                for (int i = 0; i < count; i++) {
                    bana_feed(buf[i]);
                }
                break;
            default:
                break;
        }
    }

    reader_light();
}

void wait_loop()
{
#ifndef AIC_BASE_ONLY
    keypad_update();
    report_hid_key();
#endif

    tud_task();
    cli_run();
    reader_poll_data();

    cli_fps_count(0);
}

static void core0_loop()
{
    while(1) {
        tud_task();

        cli_run();
        reader_run();

        if (reader_is_active()) {
#ifndef AIC_BASE_ONLY
            cardio_clear();
#endif
        }
        else {
#ifndef AIC_BASE_ONLY
            cardio_run(hid_light_is_active());
#endif
        }

#ifndef AIC_BASE_ONLY
        keypad_update();
        report_usb_hid();

        lis3dh_update();
#endif

        save_loop();
        cli_fps_count(0);
        sleep_ms(1);
    }
}

static void spi_overclock()
{
    uint32_t freq = clock_get_hz(clk_sys);
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, freq, freq);
}

static void identify_touch()
{
#ifdef AIC_BASE_ONLY
    aic_runtime.touch = false;
#else
    gpio_init(AIC_TOUCH_EN);
    gpio_set_function(AIC_TOUCH_EN, GPIO_FUNC_SIO);
    gpio_set_dir(AIC_TOUCH_EN, GPIO_IN);
    gpio_pull_up(AIC_TOUCH_EN);
    sleep_us(0);
    aic_runtime.touch = !gpio_get(AIC_TOUCH_EN);
#endif
}

static void find_nfc_module()
{
    static struct {
        i2c_inst_t *port;
        uint8_t scl;
        uint8_t sda;
    } i2c[] = I2C_PORT_LIST;

    for (int retry = 0; retry < 3; retry++) {
        for (int i = 0; i < count_of(i2c); i++) {
            if (nfc_init_i2c(i2c[i].port, i2c[i].scl, i2c[i].sda, I2C_FREQ)) {
                return;
            }
        }
        sleep_ms(200);
    }

    if (nfc_init_spi(SPI_PORT, SPI_MISO, SPI_SCK, SPI_MOSI,
                     SPI_RST, SPI_NSS, SPI_BUSY)) {
        return;
    }
}

static void pico_led_off()
{
    gpio_init(25);
    gpio_set_dir(25, GPIO_IN);
}

void init()
{
    tusb_init();
    stdio_init_all();

    config_init();
    save_init(0xca340a1c);

    identify_touch();

    light_init();
    light_set_rgb_order(aic_cfg->light.rgb_order);
    light_rainbow(1, 0, aic_cfg->light.level_idle);

    if (!aic_runtime.touch) {
#ifndef AIC_BASE_ONLY
        keypad_init();
#endif
    }

    pico_led_off();

    spi_overclock();

    find_nfc_module();

    nfc_set_wait_loop(wait_loop);
#ifndef AIC_BASE_ONLY
    nfc_set_card_name_listener(card_name_update_cb);
#endif

    aime_init(cdc_reader_putc);
    aime_virtual_aic(aic_cfg->reader.virtual_aic);
    bana_init(cdc_reader_putc);

#ifndef AIC_BASE_ONLY
    lis3dh_init(i2c0, 0x4);
#endif

    cli_init(
#ifdef AIC_BASE_ONLY
        "aic_pico_base>",
        "\n  << AIC Pico Base >>\n"
#else
        "aic_pico>",
        "\n     << AIC Pico >>\n"
#endif
        " https://github.com/whowechina\n\n");
    
    commands_init();
}

/* if certain key pressed when booting, enter update mode */
static void boot_update_check()
{
#ifdef AIC_BASE_ONLY
    return;
#else
    const uint8_t pins[] = { 10, 11 }; // keypad 00 and *
    bool all_pressed = true;
    for (int i = 0; i < sizeof(pins); i++) {
        uint8_t gpio = pins[i];
        gpio_init(gpio);
        gpio_set_function(gpio, GPIO_FUNC_SIO);
        gpio_set_dir(gpio, GPIO_IN);
        gpio_pull_up(gpio);
        sleep_ms(1);
        if (gpio_get(gpio)) {
            all_pressed = false;
            break;
        }
    }

    if (all_pressed) {
        sleep_ms(100);
        reset_usb_boot(0, 2);
        return;
    }
#endif
}

static void sys_init()
{
    sleep_ms(30);
    set_sys_clock_khz(160000, true);
    board_init();
}

int main(void)
{
    sys_init();
    boot_update_check();
    init();
    multicore_launch_core1(core1_loop);
    core0_loop();
    return 0;
}

#ifndef AIC_BASE_ONLY
// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    printf("Get from USB %d-%d\n", report_id, report_type);
    return 0;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize)
{
    if ((report_id == REPORT_ID_LIGHTS) &&
        (report_type == HID_REPORT_TYPE_OUTPUT)) {
        if (bufsize >= 3) {
            last_hid_time = time_us_64();
            light_fade(buffer[0] << 16 | buffer[1] << 8 | buffer[2], 0);
        }
    }
}
#endif

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    if (itf != reader_intf) {
        return;
    }

    DEBUG("\nReader Line State: %d %d", dtr, rts);

    if (!dtr) {
        aime_dtr_off();
        bana_dtr_off();
    }
}
