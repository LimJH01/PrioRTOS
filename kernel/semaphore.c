#include "prio_rtos.h"
#include "kernel_internal.h"
#include <stdint.h>

// 세마포어를 초기화하고, 즉시 획득 가능한 자원의 개수를 설정한다.
SemaphoreResult_t semaphore_init(Semaphore_t *sem, uint32_t initial_count) {
    if (sem == 0) {
        return SEMAPHORE_ERR_INVALID_ARGUMENT;
    }

    sem->count = initial_count;
    // 대기 중인 태스크가 없도록 대기자 비트맵을 비운다.
    sem->waiters = 0;
    return SEMAPHORE_OK;
}

// 자원이 있으면 하나를 획득하고, 없으면 현재 태스크를 대기 상태로 만든다.
SemaphoreResult_t semaphore_wait(Semaphore_t *sem) {
    if (sem == 0) {
        return SEMAPHORE_ERR_INVALID_ARGUMENT;
    }

    // count와 waiters를 원자적으로 확인하고 수정할 수 있도록 인터럽트를 잠시 끈다.
    uint32_t saved_mstatus = sched_irq_save();
    TCB_t *self = g_current_tcb;
    if (self == 0 || self->state != TASK_STATE_RUNNING) {
        sched_irq_restore(saved_mstatus);
        return SEMAPHORE_ERR_NO_CURRENT_TASK;
    }

    // 자원이 남아 있으면 개수를 줄여 즉시 획득한다.
    if (sem->count > 0) {
        --sem->count;
        sched_irq_restore(saved_mstatus);
        return SEMAPHORE_OK;
    }

    // 각 태스크의 우선순위에 해당하는 비트로 대기 여부를 기록한다.
    uint32_t waiter_bit = 1U << self->priority;
    if (sem->waiters & waiter_bit) {
        sched_irq_restore(saved_mstatus);
        return SEMAPHORE_ERR_INVALID_WAITER;
    }

    sem->waiters |= waiter_bit;
    // 현재 태스크를 블록하고 스케줄러로 전환한다.
    // 블록에 실패하면 대기자 비트도 원상 복구한다.
    if (!sched_block_current(saved_mstatus)) {
        sem->waiters &= ~waiter_bit;
        return SEMAPHORE_ERR_NO_CURRENT_TASK;
    }

    // 시그널을 받아 다시 실행된 뒤 대기가 성공한 것으로 반환한다.
    return SEMAPHORE_OK;
}

// 세마포어를 반환하고, 대기자가 있으면 우선순위가 가장 높은 태스크를 깨운다.
SemaphoreResult_t semaphore_signal(Semaphore_t *sem) {
    if (sem == 0) {
        return SEMAPHORE_ERR_INVALID_ARGUMENT;
    }

    // 대기자와 카운트를 일관되게 갱신할 수 있도록 인터럽트를 잠시 끈다.
    uint32_t saved_mstatus = sched_irq_save();
    if (g_current_tcb == 0 ||
        g_current_tcb->state != TASK_STATE_RUNNING) {
        sched_irq_restore(saved_mstatus);
        return SEMAPHORE_ERR_NO_CURRENT_TASK;
    }

    if (sem->waiters != 0) {
        // 비트맵에서 가장 높은 우선순위의 대기 태스크를 선택한다.
        uint32_t priority = SCHED_GET_HIGHEST_PRIO(sem->waiters);
        if (!sched_wake_task(priority)) {
            sched_irq_restore(saved_mstatus);
            return SEMAPHORE_ERR_INVALID_WAITER;
        }

        // 깨운 태스크는 대기자 집합에서 제거한다.
        sem->waiters &= ~(1U << priority);
    } else {
        // 대기자가 없으면 자원 카운트를 증가시킨다.
        if (sem->count == UINT32_MAX) {
            sched_irq_restore(saved_mstatus);
            return SEMAPHORE_ERR_COUNT_OVERFLOW;
        }
        ++sem->count;
    }

    sched_irq_restore(saved_mstatus);
    // 스케줄러에 진입해, 깨운 태스크가 현재 태스크보다 우선순위가 높으면 선점시킨다.
    asm volatile ("ecall" ::: "memory");
    return SEMAPHORE_OK;
}
