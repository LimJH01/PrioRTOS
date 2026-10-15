// kernel/include/prio_rtos.h
#ifndef PRIO_RTOS_H
#define PRIO_RTOS_H

#include <stdint.h>

#define MAX_MUTEXES 8U

typedef enum {
    TASK_CREATE_OK = 0,
    TASK_CREATE_ERR_INVALID_PRIO = -1,
    TASK_CREATE_ERR_PRIO_IN_USE = -2,
    TASK_CREATE_ERR_NULL_PTR = -3,
    TASK_CREATE_ERR_STACK_TOO_SMALL = -4,
    TASK_CREATE_ERR_ADDRESS_OVERFLOW = -5
} TaskCreateResult_t;

typedef struct {
    uint32_t count;
    uint32_t waiters;
} Semaphore_t;

typedef enum {
    SEMAPHORE_OK = 0,
    SEMAPHORE_ERR_INVALID_ARGUMENT = -1,
    SEMAPHORE_ERR_NO_CURRENT_TASK = -2,
    SEMAPHORE_ERR_INVALID_WAITER = -3,
    SEMAPHORE_ERR_COUNT_OVERFLOW = -4
} SemaphoreResult_t;

typedef enum {
    MUTEX_OK = 0,
    MUTEX_ERR_INVALID_ID = -1,
    MUTEX_ERR_INVALID_ARGUMENT = -2,
    MUTEX_ERR_NOT_OWNER = -3,
    MUTEX_ERR_RECURSIVE = -4,
    MUTEX_ERR_INVALID_CONTEXT = -5,
    MUTEX_ERR_INVALID_WAITER = -6,
    MUTEX_ERR_NOT_CONFIGURED = -7,
    MUTEX_ERR_INVALID_USERS = -8,
    MUTEX_ERR_CONFIGURATION_FROZEN = -9,
    MUTEX_ERR_ALREADY_CONFIGURED = -10
} MutexResult_t;

// 사용자가 호출 가능한 커널 공개 API 목록
void trap_init(void);
__attribute__((noreturn)) void kernel_panic(const char *msg);
TaskCreateResult_t task_create(uint32_t prio, void (*task_func)(void), uint8_t *stack, uint32_t stack_size_bytes);
void priortos_start(void);
void task_yield(void);
SemaphoreResult_t semaphore_init(Semaphore_t *sem, uint32_t initial_count);
SemaphoreResult_t semaphore_wait(Semaphore_t *sem);
SemaphoreResult_t semaphore_signal(Semaphore_t *sem);

// Configure a fixed mutex ID with the bitmap of task slots allowed to use it.
// Call during initialization, after creating all listed tasks and before start.
MutexResult_t mutex_configure(uint32_t mutex_id, uint32_t user_slots);
MutexResult_t mutex_lock(uint32_t mutex_id);
MutexResult_t mutex_unlock(uint32_t mutex_id);

#endif