/* pico-umac
 *
 * Main loop to initialise umac, and run main event loop (piping
 * keyboard/mouse events in).
 *
 * Copyright 2024 Matt Evans
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hw.h"
#include "video.h"
#include "kbd.h"

#include "bsp/rp2040/boards/adafruit_fruit_jam/board.h"
#include "tusb.h"

#include "umac.h"
#include "clocking.h"

#include "support.h"

#if ENABLE_AUDIO
#include "pico/audio_i2s.h"
#include "hardware/i2c.h"
uint8_t *audio_base;
static void audio_setup();
static bool audio_poll();
static void set_mute_state(bool new_state);
static absolute_time_t automute_time;
#endif

////////////////////////////////////////////////////////////////////////////////
// Imports and data

extern void hid_app_task(void);
extern int cursor_x;
extern int cursor_y;
extern int cursor_button;

// Mac binary data:  disc and ROM images
static const uint8_t umac_disc[] = {
#include "umac-disc.h"
};
static const uint8_t umac_rom[] = {
#include "umac-rom.h"
};

#if USE_PSRAM
#define umac_ram ((uint8_t *)0x11000000)
#define umac_ram_uncached ((uint8_t *)0x15000000)
#else
static uint8_t umac_ram[RAM_SIZE];
#define umac_ram_uncached (umac_ram)
#endif

#if MIRROR_FRAMEBUFFER
#if DISP_WIDTH < 640
static uint32_t umac_framebuffer_mirror[640 * 480 / 32];
#else
static uint32_t umac_framebuffer_mirror[DISP_WIDTH * DISP_HEIGHT / 32];
#endif
#else
#if DISP_WIDTH < 640
#error "Mirror required for DISP_WIDTH below 640 (e.g., 512x342)"
#endif
#endif

////////////////////////////////////////////////////////////////////////////////

static void io_init()
{
    gpio_init(GPIO_LED_PIN);
    gpio_set_dir(GPIO_LED_PIN, GPIO_OUT);
}

static void poll_led_etc()
{
    static absolute_time_t last = 0;
    absolute_time_t now = get_absolute_time();

    if (absolute_time_diff_us(last, now) > 500 * 1000)
    {
        last = now;
    }
}

static int umac_cursor_x = 0;
static int umac_cursor_y = 0;
static int umac_cursor_button = 0;

#define umac_get_audio_offset() (RAM_SIZE - 768)
#if MIRROR_FRAMEBUFFER
static void __no_inline_not_in_flash_func(copy_framebuffer)()
{
    uint32_t *src = (uint32_t *)(umac_ram + umac_get_fb_offset());
#if DISP_WIDTH == 512 && DISP_HEIGHT == 342
    const int DISP_XOFFSET = ((640 - 512) / 32 / 2);
    const int DISP_YOFFSET = ((480 - 342) / 2);
    const int LONGS_PER_INPUT_ROW = (512 / 32);
    const int LONGS_PER_OUTPUT_ROW = (640 / 32);
    for (int i = 0; i < DISP_HEIGHT; i++)
    {
        uint32_t *dest = umac_framebuffer_mirror + (DISP_YOFFSET * LONGS_PER_OUTPUT_ROW + DISP_XOFFSET) + LONGS_PER_OUTPUT_ROW * i;
        for (int j = 0; j < LONGS_PER_INPUT_ROW; j++)
        {
            *dest++ = *src++ ^ 0xffffffff;
        }
    }
#else
    uint32_t *dest = umac_framebuffer_mirror;
    for (int i = 0; i < DISP_WIDTH * DISP_HEIGHT / 32; i++)
    {
        *dest++ = *src++ ^ 0xffffffff;
    }
#endif
}
#endif

static void poll_umac()
{
    static absolute_time_t last_1hz = 0;
    static absolute_time_t last_vsync = 0;
    absolute_time_t now = get_absolute_time();

    umac_loop();

    int64_t p_1hz = absolute_time_diff_us(last_1hz, now);
    int64_t p_vsync = absolute_time_diff_us(last_vsync, now);
    bool pending_vsync = p_vsync > 16667;
#if ENABLE_AUDIO
    if (automute_time < now)
    {
        automute_time = at_the_end_of_time;
        set_mute_state(false);
    }
#endif
#if ENABLE_AUDIO
    pending_vsync |= audio_poll();
#endif
    if (pending_vsync)
    {
#if MIRROR_FRAMEBUFFER
        copy_framebuffer();
#endif
        /* FIXME: Trigger this off actual vsync */
        umac_vsync_event();
        last_vsync = now;
    }
    if (p_1hz >= 1000000)
    {
        umac_1hz_event();
        last_1hz = now;
    }

    int update = 0;
    int dx = 0;
    int dy = 0;
    int b = umac_cursor_button;
    if (cursor_x != umac_cursor_x)
    {
        dx = cursor_x - umac_cursor_x;
        umac_cursor_x = cursor_x;
        update = 1;
    }
    if (cursor_y != umac_cursor_y)
    {
        dy = cursor_y - umac_cursor_y;
        umac_cursor_y = cursor_y;
        update = 1;
    }
    if (cursor_button != umac_cursor_button)
    {
        b = cursor_button;
        umac_cursor_button = cursor_button;
        update = 1;
    }
    if (update)
    {
        umac_mouse(dx, -dy, b);
    }

    if (!kbd_queue_empty())
    {
        uint16_t k = kbd_queue_pop();
        umac_kbd_event(k & 0xff, !!(k & 0x8000));
    }
}

static void core1_main()
{
    disc_descr_t discs[DISC_NUM_DRIVES] = {0};

    printf("Core 1 started\n");
    disc_setup(discs);

    umac_init(umac_ram, (void *)umac_rom, discs);
    /* Video runs on core 1, i.e. IRQs/DMA are unaffected by
     * core 0's USB activity.
     */
#if MIRROR_FRAMEBUFFER
    uint32_t *fb = (uint32_t *)(umac_framebuffer_mirror);
#else
    uint32_t *fb = (uint32_t *)(umac_ram + umac_get_fb_offset());
#endif

#if DISP_WIDTH < 640
    if (video_init(fb, 640, 480, 60))
    {
        printf("video init derped!\n");
    }
    else
    {
        printf("video on hdmi running!\n");
    }
#elif DISP_WIDTH >= 1024
    video_init(fb, DISP_WIDTH, DISP_HEIGHT, 50);
#else
    video_init(fb, DISP_WIDTH, DISP_HEIGHT, 60);
#endif

#if ENABLE_AUDIO
    audio_base = (uint8_t *)umac_ram + umac_get_audio_offset();
#endif
    printf("Enjoyable Mac times now begin:\n\n");

    while (true)
    {
        poll_umac();
    }
}

int main()
{
    int _psram_size = setup_psram();
#if OVERCLOCK
    overclock(CLK_SYS_264MHZ);
#else
    overclock(CLK_SYS_176MHZ);
#endif
    stdio_init_all();

    printf("psram size %u\n", _psram_size);

#ifndef RAM_TEST
#define RAM_TEST (0)
#endif

#if RAM_TEST

    if (psram_test(10, _psram_size))
    {
        printf("ram test passed\n");
    }
    else
    {
        panic("ram test failed!\n");
    }
#endif

    io_init();

#if ENABLE_AUDIO
    audio_setup();
#endif

    multicore_launch_core1(core1_main);

    printf("Starting, init usb\n");

    const tusb_rhport_init_t rh_init = {
        .role = TUSB_ROLE_HOST,
        .speed = TUH_OPT_HIGH_SPEED ? TUSB_SPEED_HIGH : TUSB_SPEED_FULL,
    };
    TU_ASSERT(tuh_rhport_init(BOARD_TUH_RHPORT, &rh_init));

    /* This happens on core 0: */
    while (true)
    {
        tuh_task();
        hid_app_task();
        poll_led_etc();
    }

    return 0;
}

#if ENABLE_AUDIO

#define I2C_ADDR 0x18

void writeRegister(uint8_t reg, uint8_t value)
{
    char buf[2];
    buf[0] = reg;
    buf[1] = value;
    int res = i2c_write_timeout_us(i2c0, I2C_ADDR, buf, sizeof(buf), /* nostop */ false, 1000);
    if (res != 2)
    {
        printf("res=%d\n", res);
        panic("i2c_write_timeout failed: res=%d\n", res);
    }
}

uint8_t readRegister(uint8_t reg)
{
    char buf[1];
    buf[0] = reg;
    int res = i2c_write_timeout_us(i2c0, I2C_ADDR, buf, sizeof(buf), /* nostop */ true, 1000);
    if (res != 1)
    {
        panic("i2c_write_timeout failed: res=%d\n", res);
    }
    res = i2c_read_timeout_us(i2c0, I2C_ADDR, buf, sizeof(buf), /* nostop */ false, 1000);
    if (res != 1)
    {
        panic("i2c_read_timeout failed: res=%d\n", res);
    }
    uint8_t value = buf[0];
    return value;
}

void modifyRegister(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t current = readRegister(reg);
    uint8_t new_value = (current & ~mask) | (value & mask);
    writeRegister(reg, new_value);
}

void setPage(uint8_t page)
{
    writeRegister(0x00, page);
}

void Wire_begin()
{
    i2c_init(i2c0, 100000);
    gpio_set_function(20, GPIO_FUNC_I2C);
    gpio_set_function(21, GPIO_FUNC_I2C);
}

static void setup_i2s_dac()
{
    gpio_init(22);
    gpio_set_dir(22, true);
    gpio_put(22, true); // allow i2s to come out of reset

    Wire_begin();
    sleep_ms(1000);

    printf("initialize codec\n");

    // Reset codec
    writeRegister(0x01, 0x01);
    sleep_ms(10);

    // Interface Control
    modifyRegister(0x1B, 0xC0, 0x00);
    modifyRegister(0x1B, 0x30, 0x00);

    // Clock MUX and PLL settings
    modifyRegister(0x04, 0x03, 0x03);
    modifyRegister(0x04, 0x0C, 0x04);

    writeRegister(0x06, 0x20); // PLL J
    writeRegister(0x08, 0x00); // PLL D LSB
    writeRegister(0x07, 0x00); // PLL D MSB

    modifyRegister(0x05, 0x0F, 0x02); // PLL P/R
    modifyRegister(0x05, 0x70, 0x10);

    // DAC/ADC Config
    modifyRegister(0x0B, 0x7F, 0x08); // NDAC
    modifyRegister(0x0B, 0x80, 0x80);

    modifyRegister(0x0C, 0x7F, 0x02); // MDAC
    modifyRegister(0x0C, 0x80, 0x80);

    modifyRegister(0x12, 0x7F, 0x08); // NADC
    modifyRegister(0x12, 0x80, 0x80);

    modifyRegister(0x13, 0x7F, 0x02); // MADC
    modifyRegister(0x13, 0x80, 0x80);

    // PLL Power Up
    modifyRegister(0x05, 0x80, 0x80);

    // Headset and GPIO Config
    setPage(1);
    modifyRegister(0x2e, 0xFF, 0x0b);
    setPage(0);
    modifyRegister(0x43, 0x80, 0x80); // Headset Detect
    modifyRegister(0x30, 0x80, 0x80); // INT1 Control
    modifyRegister(0x33, 0x3C, 0x14); // GPIO1

    // DAC Setup
    modifyRegister(0x3F, 0xC0, 0xC0);

    // DAC Routing
    setPage(1);
    modifyRegister(0x23, 0xC0, 0x40);
    modifyRegister(0x23, 0x0C, 0x04);

    // DAC Volume Control
    setPage(0);
    modifyRegister(0x40, 0x0C, 0x00);
    writeRegister(0x41, 0x0); // Left DAC Vol, 0dB
    writeRegister(0x42, 0x0); // Right DAC Vol, 0dB

    // Headphone and Speaker Setup
    setPage(1);
    modifyRegister(0x1F, 0xC0, 0xC0); // HP Driver Powered

    modifyRegister(0x28, 0x04, 0x04); // HP Left not muted
    modifyRegister(0x29, 0x04, 0x04); // HP Right not muted

    writeRegister(0x24, 50); // Left Analog HP, -26 dB
    writeRegister(0x25, 50); // Right Analog HP, -26 dB

    modifyRegister(0x28, 0x78, 0x00); // HP Left Gain, 0 db
    modifyRegister(0x29, 0x78, 0x00); // HP Right Gain, 0 db

    // Speaker Amp
    modifyRegister(0x20, 0x80, 0x80); // Amp enabled (0x80) disable with (0x00)
    modifyRegister(0x2A, 0x04, 0x04); // Not muted (0x04) mute with (0x00)
    modifyRegister(0x2A, 0x18, 0x08); // 0 dB gain
    writeRegister(0x26, 40);          // amp gain, -20.1 dB

    // Return to page 0
    setPage(0);

    printf("Audio I2C Initialization complete!\n");
}
static int volscale;

#define SAMPLES_PER_BUFFER (370)
int16_t audio[SAMPLES_PER_BUFFER];

void umac_audio_trap()
{
    set_mute_state(volscale != 0);
    if (volscale)
    {
        automute_time = make_timeout_time_ms(500);
    }
    int32_t offset = 128;
    uint16_t *audiodata = (uint16_t *)audio_base;
    int scale = volscale;
    if (!scale)
    {
        memset(audio, 0, sizeof(audio));
        return;
    }
    int16_t *stream = audio;
    for (int i = 0; i < SAMPLES_PER_BUFFER; i++)
    {
        int32_t a = (*audiodata++ & 0xff) - offset;
        a = (a * scale) >> 8;
        *stream++ = a;
    }
}

struct audio_buffer_pool *producer_pool;

static audio_format_t audio_format = {
    .format = AUDIO_BUFFER_FORMAT_PCM_S16,
    .sample_freq = 22256, // 60.15Hz*370, rounded up
    .channel_count = 1,
};

const struct audio_i2s_config config =
    {
        .data_pin = PICO_AUDIO_I2S_DATA_PIN,
        .clock_pin_base = PICO_AUDIO_I2S_CLOCK_PIN_BASE,
        .pio_sm = 0,
        .dma_channel = 3};

static struct audio_buffer_format producer_format = {
    .format = &audio_format,
    .sample_stride = 2};

static void audio_setup()
{
    setup_i2s_dac();
    const struct audio_format *output_format = audio_i2s_setup(&audio_format, &config);
    assert(output_format);
    if (!output_format)
    {
        panic("PicoAudio: Unable to open audio device.\n");
    }
    producer_pool = audio_new_producer_pool(&producer_format, 3, SAMPLES_PER_BUFFER);
    assert(producer_pool);
    bool ok = audio_i2s_connect(producer_pool);
    assert(ok);
    audio_i2s_set_enabled(true);
}

static bool audio_poll()
{
    audio_buffer_t *buffer = take_audio_buffer(producer_pool, false);
    if (!buffer)
        return false;
    memcpy(buffer->buffer->bytes, audio, sizeof(audio));
    buffer->sample_count = SAMPLES_PER_BUFFER;
    give_audio_buffer(producer_pool, buffer);
    return true;
}

static bool mute_state = false;
static void set_mute_state(bool new_state)
{
    if (mute_state == new_state)
        return;
    mute_state = new_state;

    setPage(1);
    if (mute_state)
    {
        modifyRegister(0x28, 0x04, 0x04); // HP Left not muted
        modifyRegister(0x29, 0x04, 0x04); // HP Right not muted
        modifyRegister(0x2A, 0x04, 0x04); // Speaker not muted
    }
    else
    {
        modifyRegister(0x28, 0x04, 0x0); // HP Left muted
        modifyRegister(0x29, 0x04, 0x0); // HP Right muted
        modifyRegister(0x2A, 0x04, 0x0); // Speaker muted
    }
}

void umac_audio_cfg(int volume, int sndres)
{
    volscale = sndres ? 0 : 65536 * volume / 7;
    set_mute_state(volscale != 0);
}
#endif
