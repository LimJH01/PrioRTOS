#include "tasks.h"
#include <stdint.h>
#include "uart.h"
#include "prio_rtos.h"

#define TASK_STACK_SIZE_BYTES 1024U
#define TASK_OUTPUT_BURST_SIZE 24U
#define TASK_A_SLOT 1U
#define TASK_B_SLOT 2U
#define OUTPUT_MUTEX_ID 0U

#define STACK_ALIGNED __attribute__((aligned(16)))

static STACK_ALIGNED uint8_t stack_task_a[TASK_STACK_SIZE_BYTES];
static STACK_ALIGNED uint8_t stack_task_b[TASK_STACK_SIZE_BYTES];
static Semaphore_t sem_a_turn;
static Semaphore_t sem_b_turn;

// mock task를 위한 nop 태스크 동작
static void mock_delay(volatile uint32_t count){
    while(count--){
        asm volatile ("nop");
    }
}

void task_a(void){
    while(1){
        if (semaphore_wait(&sem_a_turn) != SEMAPHORE_OK) {
            kernel_panic("task A semaphore wait failed");
        }
        if (mutex_lock(OUTPUT_MUTEX_ID) != MUTEX_OK) {
            kernel_panic("task A mutex lock failed");
        }
        for (uint32_t i = 0; i < TASK_OUTPUT_BURST_SIZE; ++i) {
            if (uart_putc('A') != UART_OK) {
                kernel_panic("task A UART transmit timeout");
            }
            mock_delay(5000);
        }
        if (mutex_unlock(OUTPUT_MUTEX_ID) != MUTEX_OK) {
            kernel_panic("task A mutex unlock failed");
        }
        if (semaphore_signal(&sem_b_turn) != SEMAPHORE_OK) {
            kernel_panic("task A semaphore signal failed");
        }
    }
}

void task_b(void){
    while(1){
        if (semaphore_wait(&sem_b_turn) != SEMAPHORE_OK) {
            kernel_panic("task B semaphore wait failed");
        }
        if (mutex_lock(OUTPUT_MUTEX_ID) != MUTEX_OK) {
            kernel_panic("task B mutex lock failed");
        }
        for (uint32_t i = 0; i < TASK_OUTPUT_BURST_SIZE; ++i) {
            if (uart_putc('B') != UART_OK) {
                kernel_panic("task B UART transmit timeout");
            }
            mock_delay(5000);
        }
        if (mutex_unlock(OUTPUT_MUTEX_ID) != MUTEX_OK) {
            kernel_panic("task B mutex unlock failed");
        }
        if (semaphore_signal(&sem_a_turn) != SEMAPHORE_OK) {
            kernel_panic("task B semaphore signal failed");
        }
    }
}

TaskCreateResult_t tasks_init(void){
    if (semaphore_init(&sem_a_turn, 1U) != SEMAPHORE_OK ||
        semaphore_init(&sem_b_turn, 0U) != SEMAPHORE_OK) {
        kernel_panic("semaphore initialization failed");
    }

    // 인수- 우선순위, 함수 주소, 스택주소, 스택 크기를 받음
    TaskCreateResult_t result = task_create(TASK_A_SLOT, task_a, stack_task_a, TASK_STACK_SIZE_BYTES);
    if (result != TASK_CREATE_OK){
        return result;
    }

    result = task_create(TASK_B_SLOT, task_b, stack_task_b, TASK_STACK_SIZE_BYTES);
    if (result != TASK_CREATE_OK) {
        return result;
    }

    uint32_t output_users = (1U << TASK_A_SLOT) | (1U << TASK_B_SLOT);
    if (mutex_configure(OUTPUT_MUTEX_ID, output_users) != MUTEX_OK) {
        kernel_panic("output mutex configuration failed");
    }
    return TASK_CREATE_OK;
}
