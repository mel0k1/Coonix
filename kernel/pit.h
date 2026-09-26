// PIT timer (irq0) — for tick and future scheduler
#pragma once
#include <stdint.h>

void pit_init(uint32_t hz);
uint64_t pit_ticks(void);
