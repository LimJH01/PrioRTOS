#include "prio_rtos.h"
#include "kernel_internal.h"

/* 1ms 논리 tick. RV32에서도 읽기/증가를 critical section으로 보호한다. */
static volatile uint64_t system_tick;

void timer_tick_step(void) {
    ++system_tick;
    mutex_watchdog_tick();
}

uint64_t get_tick_count(void) {
    uint64_t tick;

    enter_critical();
    tick = system_tick;
    exit_critical();
    return tick;
}

/*
 * Timer ISR 전용 tick 조회.
 * ISR 진입 시 MIE가 이미 0이므로 critical section을 다시 열지 않는다.
 */
uint64_t get_tick_count_from_isr(void) {
    return system_tick;
}
