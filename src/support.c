#include "support.h"

#include <stdio.h>
#include <string.h>

#include "hardware/structs/qmi.h"
#include "hardware/structs/xip.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

#include "f_util.h"
#include "ff.h"
#include "rtc.h"
#include "hw_config.h"

#include "hw.h"

int __no_inline_not_in_flash_func(setup_psram)()
{
    int _psram_size = 0;
    gpio_set_function(PIN_PSRAM_CS, GPIO_FUNC_XIP_CS1);
    uint32_t save = save_and_disable_interrupts();
    // Try and read the PSRAM ID via direct_csr.
    qmi_hw->direct_csr = 30 << QMI_DIRECT_CSR_CLKDIV_LSB |
                         QMI_DIRECT_CSR_EN_BITS;
    // Need to poll for the cooldown on the last XIP transfer to expire
    // (via direct-mode BUSY flag) before it is safe to perform the first
    // direct-mode operation
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0)
    {
    }

    // Exit out of QMI in case we've inited already
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    // Transmit as quad.
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
                        QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB |
                        0xf5;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0)
    {
    }
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS);

    // Read the id
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    uint8_t kgd = 0;
    uint8_t eid = 0;
    for (size_t i = 0; i < 7; i++)
    {
        if (i == 0)
        {
            qmi_hw->direct_tx = 0x9f;
        }
        else
        {
            qmi_hw->direct_tx = 0xff;
        }
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0)
        {
        }
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0)
        {
        }
        if (i == 5)
        {
            kgd = qmi_hw->direct_rx;
        }
        else if (i == 6)
        {
            eid = qmi_hw->direct_rx;
        }
        else
        {
            (void)qmi_hw->direct_rx;
        }
    }
    // Disable direct csr.
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    if (kgd != 0x5D)
    {
        restore_interrupts(save);
        return 0;
    }

    // Enable quad mode.
    qmi_hw->direct_csr = 30 << QMI_DIRECT_CSR_CLKDIV_LSB |
                         QMI_DIRECT_CSR_EN_BITS;
    // Need to poll for the cooldown on the last XIP transfer to expire
    // (via direct-mode BUSY flag) before it is safe to perform the first
    // direct-mode operation
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0)
    {
    }

    // RESETEN, RESET and quad enable
    for (uint8_t i = 0; i < 4; i++)
    {
        qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        switch (i)
        {
        case 0:
            // RESETEN
            qmi_hw->direct_tx = 0x66;
            break;
        case 1:
            // RESET
            qmi_hw->direct_tx = 0x99;
            break;
        case 2:
            // Quad enable
            qmi_hw->direct_tx = 0x35;
            break;
        case 3:
            // Toggle wrap boundary mode
            qmi_hw->direct_tx = 0xc0;
            break;
        }
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0)
        {
        }
        qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS);
        for (size_t j = 0; j < 20; j++)
        {
            asm("nop");
        }
        (void)qmi_hw->direct_rx;
    }
    // Disable direct csr.
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    qmi_hw->m[1].timing =
        QMI_M0_TIMING_PAGEBREAK_VALUE_1024 << QMI_M0_TIMING_PAGEBREAK_LSB | // Break between pages.
        3 << QMI_M0_TIMING_SELECT_HOLD_LSB |                                // Delay releasing CS for 3 extra system cycles.
        1 << QMI_M0_TIMING_COOLDOWN_LSB |
        2 << QMI_M0_TIMING_RXDELAY_LSB |
        15 << QMI_M0_TIMING_MAX_SELECT_LSB |  // In units of 64 system clock cycles. PSRAM says 8us max. 8 / 0.00752 / 64 = 16.62
        8 << QMI_M0_TIMING_MIN_DESELECT_LSB | // In units of system clock cycles. PSRAM says 50ns. 50 / 7.52 = 6.64
        2 << QMI_M0_TIMING_CLKDIV_LSB;
    qmi_hw->m[1].rfmt = (QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB |
                         QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_RFMT_ADDR_WIDTH_LSB |
                         QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB |
                         QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_RFMT_DUMMY_WIDTH_LSB |
                         QMI_M0_RFMT_DUMMY_LEN_VALUE_24 << QMI_M0_RFMT_DUMMY_LEN_LSB |
                         QMI_M0_RFMT_DATA_WIDTH_VALUE_Q << QMI_M0_RFMT_DATA_WIDTH_LSB |
                         QMI_M0_RFMT_PREFIX_LEN_VALUE_8 << QMI_M0_RFMT_PREFIX_LEN_LSB |
                         QMI_M0_RFMT_SUFFIX_LEN_VALUE_NONE << QMI_M0_RFMT_SUFFIX_LEN_LSB);
    qmi_hw->m[1].rcmd = 0xeb << QMI_M0_RCMD_PREFIX_LSB |
                        0 << QMI_M0_RCMD_SUFFIX_LSB;
    qmi_hw->m[1].wfmt = (QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB |
                         QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_WFMT_ADDR_WIDTH_LSB |
                         QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB |
                         QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_WFMT_DUMMY_WIDTH_LSB |
                         QMI_M0_WFMT_DUMMY_LEN_VALUE_NONE << QMI_M0_WFMT_DUMMY_LEN_LSB |
                         QMI_M0_WFMT_DATA_WIDTH_VALUE_Q << QMI_M0_WFMT_DATA_WIDTH_LSB |
                         QMI_M0_WFMT_PREFIX_LEN_VALUE_8 << QMI_M0_WFMT_PREFIX_LEN_LSB |
                         QMI_M0_WFMT_SUFFIX_LEN_VALUE_NONE << QMI_M0_WFMT_SUFFIX_LEN_LSB);
    qmi_hw->m[1].wcmd = 0x38 << QMI_M0_WCMD_PREFIX_LSB |
                        0 << QMI_M0_WCMD_SUFFIX_LSB;

    restore_interrupts(save);

    _psram_size = 1024 * 1024; // 1 MiB
    uint8_t size_id = eid >> 5;
    if (eid == 0x26 || size_id == 2)
    {
        _psram_size *= 8;
    }
    else if (size_id == 0)
    {
        _psram_size *= 2;
    }
    else if (size_id == 1)
    {
        _psram_size *= 4;
    }

    // Mark that we can write to PSRAM.
    xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M1_BITS;

    // Test write to the PSRAM.
    volatile uint32_t *psram_nocache = (volatile uint32_t *)0x15000000;
    psram_nocache[0] = 0x12345678;
    volatile uint32_t readback = psram_nocache[0];
    if (readback != 0x12345678)
    {
        _psram_size = 0;
        return _psram_size;
    }

    // Clear the PSRAM
    for (int i = 0; i < _psram_size / 4; ++i)
    {
        psram_nocache[0] = 0x00000000;
    }
    return _psram_size;
}

int psram_test(int passes, int psramSize)
{
    volatile uint32_t *psram_buffer = (volatile uint32_t *)0x15000000;
    for (int pass = 0; pass < passes; pass++)
    {
        uint8_t acc = 1;
        for (int i = 0; i < psramSize; i++)
        {
            psram_buffer[i] = acc;
            acc = acc * 13 + 21;
        }

        acc = 1;
        for (int i = 0; i < psramSize; i++)
        {
            uint8_t ri = psram_buffer[i];
            if (ri != acc)
            {
                return false;
            }
            acc = acc * 13 + 21;
        }
    }
    return true;
}

static FIL discfp0, discfp1;

int disc_do_read(void *ctx, uint8_t *data, unsigned int offset, unsigned int len)
{
    gpio_put(GPIO_LED_PIN, 1);
    if (&discfp1 == ctx)
    {
        printf("dsk2\n");
    }
    FIL *fp = (FIL *)ctx;
    f_lseek(fp, offset);
    unsigned int did_read = 0;
    FRESULT fr = f_read(fp, data, len, &did_read);
    gpio_put(GPIO_LED_PIN, 0);
    if (fr != FR_OK || len != did_read)
    {
        printf("disc: f_read returned %d, read %u (of %u)\n", fr, did_read, len);
        return -1;
    }
    return 0;
}

int disc_do_write(void *ctx, uint8_t *data, unsigned int offset, unsigned int len)
{
    gpio_put(GPIO_LED_PIN, 1);
    FIL *fp = (FIL *)ctx;
    f_lseek(fp, offset);
    unsigned int did_write = 0;
    FRESULT fr = f_write(fp, data, len, &did_write);
    gpio_put(GPIO_LED_PIN, 0);
    if (fr != FR_OK || len != did_write)
    {
        printf("disc: f_write returned %d, read %u (of %u)\n", fr, did_write, len);
        return -1;
    }
    return 0;
}

void disc_setup(disc_descr_t discs[DISC_NUM_DRIVES])
{
    char *disc0_name;
    const char *disc0_ro_name = "umac0ro.img";
    const char *disc0_pattern = "umac0*.img";

    char *disc1_name;
    const char *disc1_ro_name = "umac2ro.img";
    const char *disc1_pattern = "umac2*.img";

    /* Mount SD filesystem */
    printf("Starting SPI/FatFS:\n");
    set_spi_dma_irq_channel(true, false);
    sd_card_t *pSD = sd_get_by_num(0);
    FRESULT fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
    printf("  mount: %d\n", fr);
    if (fr != FR_OK)
    {
        printf("  error mounting disc: %s (%d)\n", FRESULT_str(fr), fr);
        panic("No SD!\n");
    }

    /* Look for a disc image */
    DIR di = {0};
    FILINFO fi = {0};
    fr = f_findfirst(&di, &fi, "/", disc0_pattern);
    if (fr != FR_OK)
    {
        printf("  Can't find images %s: %s (%d)\n", disc0_pattern, FRESULT_str(fr), fr);
        panic("No SD!\n");
    }
    disc0_name = fi.fname;
    f_closedir(&di);

    int read_only = !strcmp(disc0_name, disc0_ro_name);
    printf("  Opening %s (R%c)\n", disc0_name, read_only ? 'O' : 'W');

    /* Open image, set up disc info: */
    fr = f_open(&discfp0, disc0_name, FA_OPEN_EXISTING | FA_READ | FA_WRITE);
    if (fr != FR_OK && fr != FR_EXIST)
    {
        printf("  *** Can't open %s: %s (%d)!\n", disc0_name, FRESULT_str(fr), fr);
        panic("No SD!\n");
    }
    else
    {
        printf("  Opened, size %d (0x%x)\n", (unsigned)f_size(&discfp0), (unsigned)f_size(&discfp0));
        if (read_only)
            printf("  (disc is read-only)\n");
        discs[0].base = 0; // Means use R/W ops
        discs[0].read_only = read_only;
        discs[0].size = f_size(&discfp0);
        discs[0].op_ctx = &discfp0;
        discs[0].op_read = disc_do_read;
        discs[0].op_write = disc_do_write;
    }

    // disc two ugly hacked in uwu
    fr = f_findfirst(&di, &fi, "/", disc1_pattern);
    if (fr != FR_OK)
    {
        printf("  Can't find images %s: %s (%d)\n", disc1_pattern, FRESULT_str(fr), fr);
        return;
    }
    disc1_name = fi.fname;
    f_closedir(&di);

    read_only = !strcmp(disc1_name, disc1_ro_name);
    printf("  Opening %s (R%c)\n", disc1_name, read_only ? 'O' : 'W');

    /* Open image, set up disc info: */
    fr = f_open(&discfp1, disc1_name, FA_OPEN_EXISTING | FA_READ | FA_WRITE);
    if (fr != FR_OK && fr != FR_EXIST)
    {
        printf("  *** Can't open %s: %s (%d)!\n", disc1_name, FRESULT_str(fr), fr);
        return;
    }
    else
    {
        printf("  Opened, size %d (0x%x)\n", (unsigned)f_size(&discfp1), (unsigned)f_size(&discfp1));
        if (read_only)
            printf("  (disc is read-only)\n");
        discs[1].base = 0; // Means use R/W ops
        discs[1].read_only = read_only;
        discs[1].size = f_size(&discfp1);
        discs[1].op_ctx = &discfp1;
        discs[1].op_read = disc_do_read;
        discs[1].op_write = disc_do_write;
    }

    /* FIXME: Other files can be stored on SD too, such as logging
     * and NVRAM storage.
     *
     * We could also implement a menu here to select an image,
     * writing text to the framebuffer and checking kbd_queue_*()
     * for user input.
     */
    return;
}