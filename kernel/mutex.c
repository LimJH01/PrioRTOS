#include "prio_rtos.h"
#include "kernel_internal.h"
#include <stddef.h>

_Static_assert(MAX_MUTEXES <= MAX_TASKS,
               "mutex waiter bitmap must fit the task bitmap");

typedef struct {
    bool created;
    bool locked;
    uint32_t owner_slot;
    uint32_t waiters;
} MutexState_t;

static MutexState_t mutexes[MAX_MUTEXES];

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

MutexResult_t mutex_create(uint32_t *mutex_id_out) {
    if (mutex_id_out == NULL) {
        return MUTEX_ERR_INVALID_ARGUMENT;
    }

    uint32_t saved_mstatus = sched_irq_save();
    for (uint32_t mutex_id = 0; mutex_id < MAX_MUTEXES; ++mutex_id) {
        MutexState_t *mutex = &mutexes[mutex_id];
        if (!mutex->created) {
            mutex->locked = false;
            mutex->owner_slot = 0U;
            mutex->waiters = 0U;
            mutex->created = true;
            *mutex_id_out = mutex_id;
            sched_irq_restore(saved_mstatus);
            return MUTEX_OK;
        }
    }

    sched_irq_restore(saved_mstatus);
    return MUTEX_ERR_NO_FREE_SLOT;
}

MutexResult_t mutex_lock(uint32_t mutex_id) {
    if (mutex_id >= MAX_MUTEXES) {
        return MUTEX_ERR_INVALID_ID;
    }

    uint32_t saved_mstatus = sched_irq_save();
    MutexState_t *mutex = &mutexes[mutex_id];
    if (!mutex->created) {
        sched_irq_restore(saved_mstatus);
        return MUTEX_ERR_NOT_CREATED;
    }

    uint32_t slot;
    if (!current_task_slot(&slot)) {
        sched_irq_restore(saved_mstatus);
        return MUTEX_ERR_INVALID_CONTEXT;
    }

    if (!mutex->locked) {
        mutex->locked = true;
        mutex->owner_slot = slot;
        sched_irq_restore(saved_mstatus);
        return MUTEX_OK;
    }

    if (mutex->owner_slot == slot) {
        sched_irq_restore(saved_mstatus);
        return MUTEX_ERR_RECURSIVE;
    }

    uint32_t waiter_bit = 1U << slot;
    if ((mutex->waiters & waiter_bit) != 0U) {
        sched_irq_restore(saved_mstatus);
        return MUTEX_ERR_INVALID_WAITER;
    }

    mutex->waiters |= waiter_bit;
    if (!sched_block_current(saved_mstatus)) {
        uint32_t rollback_mstatus = sched_irq_save();
        mutex->waiters &= ~waiter_bit;
        sched_irq_restore(rollback_mstatus);
        return MUTEX_ERR_INVALID_CONTEXT;
    }

    uint32_t resumed_slot;
    uint32_t resumed_mstatus = sched_irq_save();
    bool owns_mutex = current_task_slot(&resumed_slot) &&
                      resumed_slot == mutex->owner_slot &&
                      mutex->locked;
    sched_irq_restore(resumed_mstatus);
    return owns_mutex ? MUTEX_OK : MUTEX_ERR_INVALID_WAITER;
}

MutexResult_t mutex_unlock(uint32_t mutex_id) {
    if (mutex_id >= MAX_MUTEXES) {
        return MUTEX_ERR_INVALID_ID;
    }

    uint32_t saved_mstatus = sched_irq_save();
    MutexState_t *mutex = &mutexes[mutex_id];
    if (!mutex->created) {
        sched_irq_restore(saved_mstatus);
        return MUTEX_ERR_NOT_CREATED;
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

    if (mutex->waiters == 0U) {
        mutex->locked = false;
        sched_irq_restore(saved_mstatus);
        return MUTEX_OK;
    }

    uint32_t next_owner = SCHED_GET_HIGHEST_PRIO(mutex->waiters);
    if (g_tcbs[next_owner].state != TASK_STATE_BLOCKED) {
        sched_irq_restore(saved_mstatus);
        return MUTEX_ERR_INVALID_WAITER;
    }

    mutex->owner_slot = next_owner;
    mutex->waiters &= ~(1U << next_owner);
    if (!sched_wake_task(next_owner)) {
        mutex->waiters |= 1U << next_owner;
        mutex->owner_slot = slot;
        sched_irq_restore(saved_mstatus);
        return MUTEX_ERR_INVALID_WAITER;
    }

    sched_irq_restore(saved_mstatus);
    asm volatile ("ecall" ::: "memory");
    return MUTEX_OK;
}