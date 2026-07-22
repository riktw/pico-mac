#ifndef SUPPORT_H_
#define SUPPORT_H_

#include <inttypes.h>
#include <stdint.h>
#include "umac.h"

int setup_psram();
int psram_test(int passes, int psramSize);

int disc_do_read(void *ctx, uint8_t *data, unsigned int offset, unsigned int len);
int disc_do_write(void *ctx, uint8_t *data, unsigned int offset, unsigned int len);
int disc1_do_read(void *ctx, uint8_t *data, unsigned int offset, unsigned int len);
int disc1_do_write(void *ctx, uint8_t *data, unsigned int offset, unsigned int len);
void disc_setup(disc_descr_t discs[DISC_NUM_DRIVES]);
void disc2_setup(disc_descr_t discs[DISC_NUM_DRIVES]);

#endif // SUPPORT_H_