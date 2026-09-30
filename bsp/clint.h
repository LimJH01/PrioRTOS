#ifndef CLINT_H
#define CLINT_H

#include <stdint.h>

//QEMU virt 보드의 타이머 하드웨어 주소 이름표
#define CLINT_BASE       0x02000000
#define CLINT_MTIMECMP   (*(volatile uint64_t *)(CLINT_BASE + 0x4000))
#define CLINT_MTIME      (*(volatile uint64_t *)(CLINT_BASE + 0xBFF8))

//1초에 해당하는 틱(Tick) 수 (10MHz 주파수)
#define TIMER_INTERVAL   10000000ULL

// clint.c에 이런 함수들이 구현되어 있을 것이다
void clint_timer_init(void);
void clint_set_next_timer(void);

#endif