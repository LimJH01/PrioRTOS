#ifndef CLINT_H
#define CLINT_H

#include <stdint.h>

//QEMU virt 보드의 타이머 하드웨어 주소 이름표
#define CLINT_BASE       0x02000000
#define CLINT_MTIMECMP_LOW (*(volatile uint32_t *)(CLINT_BASE + 0x4000))
#define CLINT_MTIMECMP_HIGH (*(volatile uint32_t *)(CLINT_BASE + 0x4004))
#define CLINT_MTIME_HIGH (*(volatile uint32_t *)(CLINT_BASE + 0xBFF8))
#define CLINT_MTIME_LOW (*(volatile uint32_t *)(CLINT_BASE + 0xBFFC))


//1초에 해당하는 틱(Tick) 수 (10MHz 주파수)
#define TIMER_INTERVAL   10000000ULL

// clint.c에 이런 함수들이 구현되어 있을 것이다
void clint_timer_init(void);
void clint_set_next_timer(void);

// [안전한 64비트 읽기] High-Low-High 3번 읽기
uint64_t clint_get_mtime(void) {
    uint32_t high, low, high_again;
    do {
        high       = CLINT_MTIME_HIGH;
        low        = CLINT_MTIME_LOW;
        high_again = CLINT_MTIME_HIGH;
    } while (high != high_again); // 읽는 동안 자릿수 올림이 일어났으면 다시 읽음

    return (((uint64_t)high) << 32) | low;
}

// [안전한 64비트 쓰기] 상위를 무한대로 채워 유령 인터럽트 억제 후 쓰기
void clint_set_mtimecmp(uint64_t next_time) {
    // 1. 상위를 0xFFFFFFFF로 채워 알람을 먼 미래로 미룸 (인터럽트 발사 방지)
    CLINT_MTIMECMP_HIGH = 0xFFFFFFFF;
    
    // 2. 하위 32비트를 안전하게 기록
    CLINT_MTIMECMP_LOW  = (uint32_t)(next_time & 0xFFFFFFFF);
    
    // 3. 진짜 상위 32비트를 기록
    CLINT_MTIMECMP_HIGH = (uint32_t)(next_time >> 32);
}
#endif