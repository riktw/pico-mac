#pragma once

#include <stdint.h>

enum clk_sys_speed {
    CLK_SYS_264MHZ = 2,
    CLK_SYS_176MHZ = 3,
    CLK_SYS_132MHZ = 4,
};

extern void overclock(enum clk_sys_speed clk_sys_div);
void hstx_clock_hz(uint32_t bit_clk_hz);
