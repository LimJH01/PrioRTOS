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
