#ifndef CLINT_H
#define CLINT_H

#include <stdint.h>

//QEMU virt 보드의 타이머 하드웨어 주소 이름표
#define CLINT_BASE       0x02000000
#define CLINT_MTIMECMP_LOW (*(volatile uint32_t *)(CLINT_BASE + 0x4000))
#define CLINT_MTIMECMP_HIGH (*(volatile uint32_t *)(CLINT_BASE + 0x4004))
#define CLINT_MTIME_LOW (*(volatile uint32_t *)(CLINT_BASE + 0xBFF8))
#define CLINT_MTIME_HIGH (*(volatile uint32_t *)(CLINT_BASE + 0xBFFC))


// 10 MHz timebase에서 1초에 해당하는 tick 수
#define TIMER_INTERVAL   10000000ULL

void clint_timer_init(void);
void clint_set_next_timer(void);
uint64_t clint_get_mtime(void);
void clint_set_mtimecmp(uint64_t compare_value);

#endif