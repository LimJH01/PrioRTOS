#include "prio_rtos.h"
#include "kernel_internal.h"
#include <stddef.h>

/*
 * IPCP mutex의 런타임 상태.
 *
 * user_slots와 waiters의 각 비트는 우선순위가 아니라 g_tcbs[]의 슬롯을
 * 나타낸다. 따라서 태스크가 ceiling으로 승격되어도 mutex 소유자/대기자
 * 식별은 바뀌지 않는다. mutex 테이블은 고정 크기 정적 저장소를 사용한다.
 */
typedef struct {
    bool configured;      /* 초기화 시 사용자와 ceiling이 등록되었는가 */
    bool locked;          /* 현재 소유자가 있는가 */
    uint32_t owner_slot;  /* locked일 때 소유 태스크의 고정 슬롯 */
    uint32_t user_slots;  /* 이 mutex를 사용할 수 있도록 등록된 슬롯 */
    uint32_t waiters;     /* 이 mutex를 기다리며 BLOCKED인 슬롯 */
    uint32_t ceiling;     /* 사용자들의 base_priority 최댓값 */
} IpcpMutex_t;

static IpcpMutex_t mutexes[MAX_MUTEXES];
/* priortos_start() 이후 설정 변경을 막아 런타임 구성을 고정한다. */
static bool configuration_frozen;

/*
 * 현재 실행 중인 TCB의 슬롯을 찾아낸다.
 * mutex owner/waiter bitmap은 슬롯 번호를 저장하므로, 유효 우선순위가
 * 바뀌는 TCB.priority로 슬롯을 추정하지 않고 TCB 배열의 주소로 식별한다.
 */
static bool current_task_slot(uint32_t *slot_out) {
    if (g_current_tcb == NULL) {
        return false;
    }

    for (uint32_t slot = 0; slot < MAX_TASKS; ++slot) {
        if (g_current_tcb == &g_tcbs[slot] &&
            g_current_tcb->state == TASK_STATE_RUNNING) {
            *slot_out = slot;
            return true;
        }
    }
    return false;
}

/*
 * mutex 사용자로 허용 가능한 슬롯 비트마스크를 만든다.
 * 슬롯 0은 idle 전용이므로 제외한다. MAX_TASKS가 uint32_t의 비트 수와
 * 같을 때 (1U << 32)가 정의되지 않으므로 그 경계는 별도 처리한다.
 */
static uint32_t configured_user_mask(void) {
    if (MAX_TASKS >= 32U) {
        return UINT32_MAX & ~(1U << IDLE_TASK_PRIORITY);
    }
    return ((1U << MAX_TASKS) - 1U) & ~(1U << IDLE_TASK_PRIORITY);
}

/*
 * 요청 태스크가 소유하지 않은 잠긴 자원들의 system ceiling을 계산한다.
 * 요청 태스크가 이미 소유한 자원은 중첩 lock 판정에서 제외한다.
 * UINT32_MAX는 잠긴 다른 자원이 없어 ceiling 제한이 없다는 내부 표식이다.
 */
static uint32_t system_ceiling(uint32_t requesting_slot) {
    uint32_t ceiling = 0U;
    bool has_ceiling = false;

    for (uint32_t id = 0; id < MAX_MUTEXES; ++id) {
        const IpcpMutex_t *mutex = &mutexes[id];
        if (mutex->configured && mutex->locked &&
            mutex->owner_slot != requesting_slot &&
            (!has_ceiling || mutex->ceiling > ceiling)) {
            ceiling = mutex->ceiling;
            has_ceiling = true;
        }
    }

    return has_ceiling ? ceiling : UINT32_MAX;
}

/*
 * 태스크가 현재 보유한 모든 mutex의 ceiling을 다시 반영한다.
 * base priority를 항상 하한으로 두므로 ceiling이 낮은 mutex를 해제해도
 * 더 높은 ceiling의 다른 mutex를 보유 중이면 priority가 그 수준을 유지한다.
 */
static void refresh_effective_priority(uint32_t slot) {
    uint32_t priority = g_tcbs[slot].base_priority;
    for (uint32_t id = 0; id < MAX_MUTEXES; ++id) {
        const IpcpMutex_t *mutex = &mutexes[id];
        if (mutex->configured && mutex->locked &&
            mutex->owner_slot == slot &&
            mutex->ceiling > priority) {
            priority = mutex->ceiling;
        }
    }
    g_tcbs[slot].priority = priority;
}

/*
 * IPCP admission 조건을 검사한다.
 * 새 자원은 비어 있고 호출 태스크가 등록된 사용자여야 하며,
 * 호출 태스크의 유효 우선순위는 다른 태스크의 system ceiling보다 커야 한다.
 * 같은 우선순위는 허용하지 않는 strict-greater 비교다.
 */
static bool can_acquire(uint32_t mutex_id, uint32_t slot) {
    const IpcpMutex_t *mutex = &mutexes[mutex_id];
    if (!mutex->configured || mutex->locked ||
        (mutex->user_slots & (1U << slot)) == 0U) {
        return false;
    }

    uint32_t ceiling = system_ceiling(slot);
    return ceiling == UINT32_MAX ||
           g_tcbs[slot].priority > ceiling;
}

/*
 * unlock 뒤 모든 mutex의 대기자 중 지금 획득 가능한 태스크를 재평가한다.
 * 매회 최고 유효 우선순위, 동률이면 슬롯 번호가 작은 태스크를 선택한다.
 * 선택된 대기자의 소유권을 READY 전환보다 먼저 확정한다. waiter 수가
 * MAX_TASKS 이하이므로 반복은 정적 상한(MAX_TASKS)을 넘지 않는다.
 */
static void dispatch_eligible_waiters(void) {
    for (;;) {
        uint32_t selected_mutex = MAX_MUTEXES;
        uint32_t selected_slot = MAX_TASKS;

        for (uint32_t id = 0; id < MAX_MUTEXES; ++id) {
            IpcpMutex_t *mutex = &mutexes[id];
            if (!mutex->configured || mutex->locked) {
                continue;
            }

            for (uint32_t slot = 1; slot < MAX_TASKS; ++slot) {
                uint32_t waiter_bit = 1U << slot;
                if ((mutex->waiters & waiter_bit) == 0U ||
                    g_tcbs[slot].state != TASK_STATE_BLOCKED ||
                    !can_acquire(id, slot)) {
                    continue;
                }

                /* 유효 priority 우선, 동률이면 결정적인 슬롯 순서로 선택. */
                if (selected_slot == MAX_TASKS ||
                    g_tcbs[slot].priority > g_tcbs[selected_slot].priority ||
                    (g_tcbs[slot].priority == g_tcbs[selected_slot].priority &&
                     slot < selected_slot)) {
                    selected_mutex = id;
                    selected_slot = slot;
                }
            }
        }

        if (selected_mutex == MAX_MUTEXES) {
            return;
        }

        IpcpMutex_t *mutex = &mutexes[selected_mutex];
        /* waiter가 실행 가능해지기 전에 소유권을 먼저 handoff한다. */
        mutex->locked = true;
        mutex->owner_slot = selected_slot;
        mutex->waiters &= ~(1U << selected_slot);
        refresh_effective_priority(selected_slot);
        if (!sched_wake_task(selected_slot)) {
            /* waiter bitmap과 TCB 상태가 어긋난 것은 복구 가능한 API 오류가 아니다. */
            kernel_panic("IPCP waiter state inconsistent");
        }
    }
}

/*
 * 초기화 구간에서 고정 mutex ID와 허용 사용자 슬롯을 등록한다.
 * ceiling은 사용 태스크들의 기본 우선순위 최댓값으로 계산하며,
 * 모든 검사 성공 뒤 마지막에 configured를 세워 부분 설정을 노출하지 않는다.
 */
MutexResult_t mutex_configure(uint32_t mutex_id, uint32_t user_slots) {
    if (mutex_id >= MAX_MUTEXES) {
        return MUTEX_ERR_INVALID_ID;
    }
    if (user_slots == 0U ||
        (user_slots & ~configured_user_mask()) != 0U) {
        return MUTEX_ERR_INVALID_USERS;
    }

    /* 설정 동결/중복 검사와 테이블 갱신이 서로 끼어들지 않게 보호한다. */
    uint32_t saved_mstatus = sched_irq_save();
    if (configuration_frozen) {
        sched_irq_restore(saved_mstatus);
        return MUTEX_ERR_CONFIGURATION_FROZEN;
    }

    IpcpMutex_t *mutex = &mutexes[mutex_id];
    if (mutex->configured) {
        sched_irq_restore(saved_mstatus);
        return MUTEX_ERR_ALREADY_CONFIGURED;
    }

    uint32_t ceiling = 0U;
    for (uint32_t slot = 1; slot < MAX_TASKS; ++slot) {
        if ((user_slots & (1U << slot)) == 0U) {
            continue;
        }
        if (g_tcbs[slot].state == TASK_STATE_UNUSED) {
            sched_irq_restore(saved_mstatus);
            return MUTEX_ERR_INVALID_USERS;
        }
        if (g_tcbs[slot].base_priority > ceiling) {
            ceiling = g_tcbs[slot].base_priority;
        }
    }

    /* 검증이 모두 끝난 뒤 mutex의 설정을 한 번에 완성한다. */
    mutex->user_slots = user_slots;
    mutex->ceiling = ceiling;
    mutex->owner_slot = 0U;
    mutex->waiters = 0U;
    mutex->locked = false;
    mutex->configured = true;
    sched_irq_restore(saved_mstatus);
    return MUTEX_OK;
}

/*
 * IPCP mutex 획득.
 * 자원을 즉시 얻지 못하면 현재 태스크를 mutex별 waiter bitmap에 기록하고
 * BLOCKED로 만든다. unlock이 소유권을 직접 넘긴 뒤 재개되면 반복문이
 * 자기 소유권을 확인하고 성공으로 반환한다.
 */
MutexResult_t mutex_lock(uint32_t mutex_id) {
    if (mutex_id >= MAX_MUTEXES) {
        return MUTEX_ERR_INVALID_ID;
    }

    bool waiting = false;
    for (;;) {
        uint32_t saved_mstatus = sched_irq_save();
        IpcpMutex_t *mutex = &mutexes[mutex_id];
        if (!mutex->configured) {
            sched_irq_restore(saved_mstatus);
            return MUTEX_ERR_NOT_CONFIGURED;
        }

        uint32_t slot;
        if (!current_task_slot(&slot)) {
            sched_irq_restore(saved_mstatus);
            return MUTEX_ERR_INVALID_CONTEXT;
        }
        if ((mutex->user_slots & (1U << slot)) == 0U) {
            sched_irq_restore(saved_mstatus);
            return MUTEX_ERR_INVALID_USERS;
        }

        if (mutex->locked && mutex->owner_slot == slot) {
            sched_irq_restore(saved_mstatus);
            /* 첫 호출에서 이미 소유 중이면 재귀 lock, block 후 복귀한 경우는 handoff 성공. */
            return waiting ? MUTEX_OK : MUTEX_ERR_RECURSIVE;
        }

        if (can_acquire(mutex_id, slot)) {
            /* 소유권과 effective priority를 같은 인터럽트 보호 구간에서 갱신한다. */
            mutex->locked = true;
            mutex->owner_slot = slot;
            refresh_effective_priority(slot);
            sched_irq_restore(saved_mstatus);
            return MUTEX_OK;
        }

        uint32_t waiter_bit = 1U << slot;
        if ((mutex->waiters & waiter_bit) != 0U) {
            sched_irq_restore(saved_mstatus);
            return MUTEX_ERR_INVALID_WAITER;
        }

        mutex->waiters |= waiter_bit;
        waiting = true;
        /*
         * sched_block_current()는 saved_mstatus 상태를 받아 상태/ready bit를
         * 갱신한 뒤 ecall로 문맥 전환한다. 실패하면 이 함수가 waiter bit를
         * 되돌려 대기자 테이블에 고아 항목이 남지 않게 한다.
         */
        if (!sched_block_current(saved_mstatus)) {
            uint32_t rollback_mstatus = sched_irq_save();
            mutex->waiters &= ~waiter_bit;
            sched_irq_restore(rollback_mstatus);
            return MUTEX_ERR_INVALID_CONTEXT;
        }
    }
}

/*
 * mutex 소유자만 해제할 수 있다.
 * 해제, 기존 소유자의 priority 복구, 적격 대기자 handoff를 하나의
 * 인터럽트 보호 구간에서 처리한 다음 ecall로 선점 여부를 평가한다.
 */
MutexResult_t mutex_unlock(uint32_t mutex_id) {
    if (mutex_id >= MAX_MUTEXES) {
        return MUTEX_ERR_INVALID_ID;
    }

    uint32_t saved_mstatus = sched_irq_save();
    IpcpMutex_t *mutex = &mutexes[mutex_id];
    if (!mutex->configured) {
        sched_irq_restore(saved_mstatus);
        return MUTEX_ERR_NOT_CONFIGURED;
    }

    uint32_t slot;
    if (!current_task_slot(&slot)) {
        sched_irq_restore(saved_mstatus);
        return MUTEX_ERR_INVALID_CONTEXT;
    }

    if (!mutex->locked || mutex->owner_slot != slot) {
        sched_irq_restore(saved_mstatus);
        return MUTEX_ERR_NOT_OWNER;
    }

    mutex->locked = false;
    mutex->owner_slot = 0U;
    /* 이 mutex ceiling을 더 이상 적용하지 않고 나머지 보유 자원을 반영한다. */
    refresh_effective_priority(slot);
    /* 다른 mutex ceiling 때문에 block된 태스크도 함께 재평가한다. */
    dispatch_eligible_waiters();
    sched_irq_restore(saved_mstatus);
    asm volatile ("ecall" ::: "memory");
    return MUTEX_OK;
}

/*
 * 첫 태스크를 실행하기 전에 등록된 설정을 다시 검증한다.
 * configure 시점 이후 task table에 변화가 있었거나 bitmap이 손상된 경우
 * 스케줄러가 시작되기 전에 false를 반환한다.
 */
bool ipcp_validate_configuration(void) {
    uint32_t saved_mstatus = sched_irq_save();
    for (uint32_t id = 0; id < MAX_MUTEXES; ++id) {
        const IpcpMutex_t *mutex = &mutexes[id];
        if (!mutex->configured) {
            continue;
        }
        if (mutex->user_slots == 0U ||
            (mutex->user_slots & ~configured_user_mask()) != 0U) {
            sched_irq_restore(saved_mstatus);
            return false;
        }
        for (uint32_t slot = 1; slot < MAX_TASKS; ++slot) {
            if ((mutex->user_slots & (1U << slot)) != 0U &&
                g_tcbs[slot].state == TASK_STATE_UNUSED) {
                sched_irq_restore(saved_mstatus);
                return false;
            }
        }
    }
    sched_irq_restore(saved_mstatus);
    return true;
}

/* 스케줄러가 첫 태스크를 선택한 뒤 호출하여 IPCP 구성을 불변으로 만든다. */
void ipcp_scheduler_started(void) {
    configuration_frozen = true;
}
