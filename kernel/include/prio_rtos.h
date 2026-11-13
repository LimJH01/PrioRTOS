// kernel/include/prio_rtos.h
#ifndef PRIO_RTOS_H
#define PRIO_RTOS_H

#include <stdint.h>

/*
 * 정적 Mutex 상태. 응용 코드는 이 타입으로 Mutex를 전역 또는 static으로
 * 선언하고, mutex_init()에서 ceiling만 설정한다.
 */
typedef struct mutex {
    uint8_t ceiling_prio;
    uint8_t saved_prio;
    uint8_t locked;
    uint8_t padding;
    struct mutex *prev_mutex;
} mutex_t;

typedef enum {
    TASK_CREATE_OK = 0,
    TASK_CREATE_ERR_INVALID_PRIO = -1,
    TASK_CREATE_ERR_PRIO_IN_USE = -2,
    TASK_CREATE_ERR_NULL_PTR = -3,
    TASK_CREATE_ERR_STACK_TOO_SMALL = -4,
    TASK_CREATE_ERR_ADDRESS_OVERFLOW = -5,
    TASK_CREATE_ERR_STACK_MISALIGNED = -6
} TaskCreateResult_t;

/*
 * 정적 semaphore 상태.
 * count는 사용 가능한 자원 수, waiters는 대기 중인 태스크 ID bitmap이다.
 * 두 필드 모두 32비트이므로 Master Spec에서 총 8바이트로 고정된다.
 */
typedef struct {
    uint32_t count;   // 현재 사용 가능한 semaphore 자원 수
    uint32_t waiters; // 대기 태스크 bitmap: bit n == task_id n
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
    MUTEX_ERR_INVALID_ARGUMENT = -1,
    MUTEX_ERR_INVALID_CONTEXT = -2,
    MUTEX_ERR_INVALID_CEILING = -3,
    MUTEX_ERR_INVALID_BUDGET = -4
} MutexResult_t;

// 사용자가 호출 가능한 커널 공개 API 목록
void trap_init(void);
__attribute__((noreturn)) void kernel_panic(const char *msg);
void enter_critical(void);
void exit_critical(void);
TaskCreateResult_t task_create(uint32_t prio, void (*task_func)(void), uint8_t *stack, uint32_t stack_size_bytes);
void priortos_start(void);
void task_yield(void);
void task_sleep_ticks(uint32_t ticks);
void timer_tick_step(void);
uint64_t get_tick_count(void);
uint64_t get_tick_count_from_isr(void);
SemaphoreResult_t semaphore_init(Semaphore_t *sem, uint32_t initial_count);
SemaphoreResult_t semaphore_wait(Semaphore_t *sem);
SemaphoreResult_t semaphore_signal(Semaphore_t *sem);

MutexResult_t mutex_init(mutex_t *mutex, uint8_t ceiling_prio);
MutexResult_t mutex_set_execution_budget(mutex_t *mutex, uint32_t budget_ticks);
MutexResult_t mutex_lock(mutex_t *mutex);
MutexResult_t mutex_unlock(mutex_t *mutex);

#endif
