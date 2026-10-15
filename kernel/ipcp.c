#include "prio_rtos.h"
#include "kernel_internal.h"
#include <stddef.h>

typedef struct {
    bool configured;
    bool locked;
    uint32_t owner_slot;
    uint32_t user_slots;
    uint32_t waiters;
    uint32_t ceiling;
} IpcpMutex_t;

static IpcpMutex_t mutexes[MAX_MUTEXES];
static bool configuration_frozen;

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

static uint32_t configured_user_mask(void) {
    if (MAX_TASKS >= 32U) {
        return UINT32_MAX & ~(1U << IDLE_TASK_PRIORITY);
    }
    return ((1U << MAX_TASKS) - 1U) & ~(1U << IDLE_TASK_PRIORITY);
}

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
        mutex->locked = true;
        mutex->owner_slot = selected_slot;
        mutex->waiters &= ~(1U << selected_slot);
        refresh_effective_priority(selected_slot);
        if (!sched_wake_task(selected_slot)) {
            kernel_panic("IPCP waiter state inconsistent");
        }
    }
}

MutexResult_t mutex_configure(uint32_t mutex_id, uint32_t user_slots) {
    if (mutex_id >= MAX_MUTEXES) {
        return MUTEX_ERR_INVALID_ID;
    }
    if (user_slots == 0U ||
        (user_slots & ~configured_user_mask()) != 0U) {
        return MUTEX_ERR_INVALID_USERS;
    }

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

    mutex->user_slots = user_slots;
    mutex->ceiling = ceiling;
    mutex->owner_slot = 0U;
    mutex->waiters = 0U;
    mutex->locked = false;
    mutex->configured = true;
    sched_irq_restore(saved_mstatus);
    return MUTEX_OK;
}

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
            return waiting ? MUTEX_OK : MUTEX_ERR_RECURSIVE;
        }

        if (can_acquire(mutex_id, slot)) {
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
        if (!sched_block_current(saved_mstatus)) {
            uint32_t rollback_mstatus = sched_irq_save();
            mutex->waiters &= ~waiter_bit;
            sched_irq_restore(rollback_mstatus);
            return MUTEX_ERR_INVALID_CONTEXT;
        }
    }
}

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
    refresh_effective_priority(slot);
    dispatch_eligible_waiters();
    sched_irq_restore(saved_mstatus);
    asm volatile ("ecall" ::: "memory");
    return MUTEX_OK;
}

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

void ipcp_scheduler_started(void) {
    configuration_frozen = true;
}
