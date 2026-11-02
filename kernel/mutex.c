#include "prio_rtos.h"
#include "kernel_internal.h"

#include <stdint.h>

#define MUTEX_WATCHDOG_SLOTS (OS_MAX_TASKS * OS_MAX_MUTEX_NESTING)

/*
 * mutex_t의 Master Spec 레이아웃은 8바이트로 고정되어 있으므로 실행시간
 * 계측용 시각과 예산은 Mutex 객체 안에 추가하지 않는다. 이 표는 소유권을
 * 관리하지 않고, 현재 held_mutex 연결 목록에 대응하는 감시 정보만 보관한다.
 */
typedef struct {
    mutex_t *mutex;
    TCB_t *owner;
    uint64_t acquired_tick;
    uint32_t budget_ticks;
    bool active;
} MutexWatchdogEntry_t;

static MutexWatchdogEntry_t mutex_watchdog[MUTEX_WATCHDOG_SLOTS];

static MutexWatchdogEntry_t *watchdog_find(const mutex_t *mutex) {
    for (uint32_t i = 0U; i < MUTEX_WATCHDOG_SLOTS; ++i) {
        if (mutex_watchdog[i].active && mutex_watchdog[i].mutex == mutex) {
            return &mutex_watchdog[i];
        }
    }
    return NULL;
}

static MutexWatchdogEntry_t *watchdog_free_slot(void) {
    for (uint32_t i = 0U; i < MUTEX_WATCHDOG_SLOTS; ++i) {
        if (!mutex_watchdog[i].active) {
            return &mutex_watchdog[i];
        }
    }
    return NULL;
}

/*
 * 현재 실행 태스크가 유효한지 확인한다. Mutex 소유자는 별도 owner_slot에
 * 복제하지 않고, 현재 TCB의 held_mutex 연결 목록으로만 관리한다.
 */
static TCB_t *current_task(void) {
    if (g_current_tcb == NULL ||
        g_current_tcb->state != TASK_STATE_RUNNING) {
        return NULL;
    }
    return g_current_tcb;
}

/* 현재 태스크가 이미 보유한 Mutex인지 확인한다. 최대 4단계만 순회한다. */
static bool owns_mutex(const TCB_t *task, const mutex_t *mutex) {
    const mutex_t *current = task->held_mutex;
    uint32_t depth = 0U;

    while (current != NULL && depth < OS_MAX_MUTEX_NESTING) {
        if (current == mutex) {
            return true;
        }
        current = current->prev_mutex;
        ++depth;
    }
    return false;
}

/* 현재 태스크가 보유한 Mutex 개수를 bounded하게 센다. */
static uint32_t mutex_depth(const TCB_t *task) {
    const mutex_t *current = task->held_mutex;
    uint32_t depth = 0U;

    while (current != NULL && depth < OS_MAX_MUTEX_NESTING) {
        current = current->prev_mutex;
        ++depth;
    }

    if (current != NULL) {
        kernel_panic("ERR_IPCP_NESTING");
    }
    return depth;
}

MutexResult_t mutex_init(mutex_t *mutex, uint8_t ceiling_prio) {
    if (mutex == NULL) {
        return MUTEX_ERR_INVALID_ARGUMENT;
    }
    if (ceiling_prio > MAX_PRIORITY - 1U) {
        return MUTEX_ERR_INVALID_CEILING;
    }

    mutex->ceiling_prio = ceiling_prio;
    mutex->saved_prio = 0U;
    mutex->locked = 0U;
    mutex->padding = 0U;
    mutex->prev_mutex = NULL;

    MutexWatchdogEntry_t *watchdog = watchdog_find(mutex);
    if (watchdog != NULL) {
        watchdog->active = false;
        watchdog->mutex = NULL;
        watchdog->owner = NULL;
        watchdog->budget_ticks = 0U;
    }
    return MUTEX_OK;
}

MutexResult_t mutex_set_execution_budget(mutex_t *mutex, uint32_t budget_ticks) {
    if (mutex == NULL || budget_ticks == 0U) {
        return MUTEX_ERR_INVALID_BUDGET;
    }

    enter_critical();

    MutexWatchdogEntry_t *watchdog = watchdog_find(mutex);
    if (watchdog == NULL) {
        watchdog = watchdog_free_slot();
    }
    if (watchdog == NULL) {
        exit_critical();
        return MUTEX_ERR_INVALID_BUDGET;
    }

    watchdog->mutex = mutex;
    watchdog->owner = NULL;
    watchdog->acquired_tick = 0U;
    watchdog->budget_ticks = budget_ticks;
    watchdog->active = true;

    exit_critical();
    return MUTEX_OK;
}

MutexResult_t mutex_lock(mutex_t *mutex) {
    if (mutex == NULL) {
        return MUTEX_ERR_INVALID_ARGUMENT;
    }

    TCB_t *task = current_task();
    if (task == NULL || task->task_id == IDLE_TASK_PRIORITY) {
        return MUTEX_ERR_INVALID_CONTEXT;
    }

    enter_critical();

    if (mutex->locked != 0U || owns_mutex(task, mutex)) {
        kernel_panic("ERR_MUTEX_CONTENTION");
    }

    const uint32_t depth = mutex_depth(task);
    if (depth >= OS_MAX_MUTEX_NESTING) {
        kernel_panic("ERR_IPCP_NESTING");
    }

    /* 숫자가 작을수록 높은 우선순위다. 더 높은 태스크가 낮은 ceiling을
     * 가진 Mutex를 획득하면 priority를 낮추는 상황이므로 명세 위반이다. */
    if (task->priority < mutex->ceiling_prio) {
        kernel_panic("ERR_MUTEX_CEILING");
    }

    MutexWatchdogEntry_t *watchdog = watchdog_find(mutex);
    if (watchdog == NULL || watchdog->budget_ticks == 0U) {
        kernel_panic("ERR_MUTEX_BUDGET_UNCONFIGURED");
    }

    mutex->saved_prio = task->priority;
    mutex->prev_mutex = task->held_mutex;
    mutex->locked = 1U;
    task->held_mutex = mutex;
    task->priority = mutex->ceiling_prio;
    watchdog->owner = task;
    watchdog->acquired_tick = get_tick_count();

    exit_critical();
    return MUTEX_OK;
}

MutexResult_t mutex_unlock(mutex_t *mutex) {
    if (mutex == NULL) {
        return MUTEX_ERR_INVALID_ARGUMENT;
    }

    TCB_t *task = current_task();
    if (task == NULL || task->task_id == IDLE_TASK_PRIORITY) {
        return MUTEX_ERR_INVALID_CONTEXT;
    }

    enter_critical();

    /* LIFO 해제를 강제한다. 보유 목록의 최상단이 아니면 즉시 중지한다. */
    if (mutex->locked == 0U || task->held_mutex != mutex) {
        kernel_panic("ERR_MUTEX_UNLOCK_ORDER");
    }

    task->held_mutex = mutex->prev_mutex;
    task->priority = mutex->saved_prio;
    mutex->prev_mutex = NULL;
    mutex->saved_prio = 0U;
    mutex->locked = 0U;

    MutexWatchdogEntry_t *watchdog = watchdog_find(mutex);
    if (watchdog != NULL) {
        watchdog->owner = NULL;
        watchdog->acquired_tick = 0U;
    }

    const bool outermost_unlock = (task->held_mutex == NULL);
    exit_critical();

    /* 최외곽 Mutex를 해제한 뒤에만 pending 선점을 재평가한다. */
    if (outermost_unlock) {
        sched_request_reschedule();
    }
    return MUTEX_OK;
}

void mutex_watchdog_tick(void) {
    TCB_t *task = g_current_tcb;
    if (task == NULL || task->state != TASK_STATE_RUNNING ||
        task->held_mutex == NULL) {
        return;
    }

    const uint64_t now = get_tick_count();
    const mutex_t *mutex = task->held_mutex;
    uint32_t depth = 0U;

    while (mutex != NULL && depth < OS_MAX_MUTEX_NESTING) {
        MutexWatchdogEntry_t *watchdog = watchdog_find(mutex);
        if (watchdog == NULL || watchdog->owner != task ||
            watchdog->budget_ticks == 0U) {
            kernel_panic("ERR_MUTEX_BUDGET_UNCONFIGURED");
        }

        if ((now - watchdog->acquired_tick) >= watchdog->budget_ticks) {
            kernel_panic("ERR_MUTEX_EXECUTION_BUDGET");
        }

        mutex = mutex->prev_mutex;
        ++depth;
    }

    if (mutex != NULL) {
        kernel_panic("ERR_IPCP_NESTING");
    }
}
