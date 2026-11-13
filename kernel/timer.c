#include "prio_rtos.h"
#include "kernel_internal.h"

/* 1ms 논리 tick. RV32에서도 읽기/증가를 critical section으로 보호한다. */
static volatile uint64_t system_tick;
/*
 * 태스크 슬롯별 남은 sleep tick이다.
 * .bss에 배치되어 부팅 시 0으로 초기화되며, task_sleep_ticks()가 값을
 * 기록하고 timer tick에서 감소시킨다.
 */
uint32_t g_sleep_remaining_ticks[MAX_TASKS];

/*
 * sleep 중인 태스크의 카운터를 한 tick씩 줄이고, 만료된 태스크를 READY로
 * 되돌린다. 최대 MAX_TASKS개 슬롯만 검사하므로 timer ISR에서 실행 시간이
 * 제한된다. 실제 문맥 전환은 timer_tick_step()이 요청한 MSIP 경로에서
 * 수행한다.
 */
static void wake_expired_tasks(void) {
    for (uint32_t slot = 0U; slot < MAX_TASKS; ++slot) {
        if (g_sleep_remaining_ticks[slot] == 0U) {
            continue;
        }

        --g_sleep_remaining_ticks[slot];
        if (g_sleep_remaining_ticks[slot] == 0U &&
            g_tcbs[slot].state == TASK_STATE_BLOCKED) {
            g_tcbs[slot].state = TASK_STATE_READY;
            g_ready_bitmap |= (1U << slot);
        }
    }
}

/*
 * Timer ISR의 논리 tick 처리 순서다.
 * 1) 시스템 tick 증가
 * 2) 현재 Mutex 실행시간 예산 검사
 * 3) sleep 만료 태스크 복귀
 * 4) 매 tick마다 MSIP로 scheduler 재평가 요청
 *
 * Timer top-half에서는 직접 context switch하지 않는다. sleep 만료가
 * 없어도 매 tick scheduler를 호출해야 외부 이벤트로 READY가 된 더 높은
 * 우선순위 태스크를 놓치지 않는다. 동률 유지와 Mutex non-preemption은
 * sched_schedule()이 최종적으로 판단한다.
 */
void timer_tick_step(void) {
    assert_isr_context();

    ++system_tick;
    mutex_watchdog_tick();
    wake_expired_tasks();
    sched_request_reschedule();
}

uint64_t get_tick_count(void) {
    assert_task_context();

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
    assert_isr_context();

    /* 호출자는 Timer ISR 문맥에 있어야 하며, 진입 시 MIE가 이미 0이다. */
    return system_tick;
}
