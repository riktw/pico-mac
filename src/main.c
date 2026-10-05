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
#include "pio_usb_configuration.h"
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

#if ENABLE_PIO_USB
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.tx_ch = 2;
    pio_cfg.pin_dp = 28;
    pio_cfg.pinout = PIO_USB_PINOUT_DPDM;
    // USB VBUS enable
    gpio_init(42);
    gpio_set_dir(42, GPIO_OUT);
    gpio_put(42, true);

    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);

    tuh_init(BOARD_TUH_RHPORT);
#else
    const tusb_rhport_init_t rh_init = {
        .role = TUSB_ROLE_HOST,
        .speed = TUH_OPT_HIGH_SPEED ? TUSB_SPEED_HIGH : TUSB_SPEED_FULL,
    };
    TU_ASSERT(tuh_rhport_init(BOARD_TUH_RHPORT, &rh_init));
#endif
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
        .data_pin = 30,
        .clock_pin_base = 32,
        .pio_sm = 0,
        .dma_channel = 3};

static struct audio_buffer_format producer_format = {
    .format = &audio_format,
    .sample_stride = 2};

static void audio_setup()
{

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
}

void umac_audio_cfg(int volume, int sndres)
{
    volscale = sndres ? 0 : 65536 * volume / 7;
    set_mute_state(volscale != 0);
}
#endif
