// kernel/include/prio_rtos.h
#ifndef PRIO_RTOS_H
#define PRIO_RTOS_H

#include <stdint.h>

// 사용자가 호출 가능한 커널 공개 API 목록
int task_create(uint32_t prio, void (*task_func)(void), uint32_t *stack, uint32_t stack_size);
void priortos_start(void);
void task_yield(void);

// IPCP 뮤텍스 API (나중에 사용)
int  mutex_lock(uint32_t mutex_id);
int  mutex_unlock(uint32_t mutex_id);

#endif