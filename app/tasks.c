#include "tasks.h"
#include <stdint.h>
#include "uart.h"
#include "prio_rtos.h"

#define TASK_STACK_WORDS 256

#define STACK_ALIGNED __attribute__((aligned(16)))

static STACK_ALIGNED uint32_t stack_task_a[TASK_STACK_WORDS];
static STACK_ALIGNED uint32_t stack_task_b[TASK_STACK_WORDS];

// mock task를 위한 nop 태스크 동작
static void mock_delay(volatile uint32_t count){
    while(count--){
        asm volatile ("nop");
    }
}

void task_a(void){
    while(1){
        uart_putc('A');
        mock_delay(5000);
    }
}

void task_b(void){
    while(1){
        uart_putc('B');
        mock_delay(10000);
    }
}

void tasks_init(){
    // 인수로, 우선순위, 함수 주소, 스택주소, 스택 크기를 받음
    task_create(1, task_a, stack_task_a, TASK_STACK_WORDS);
    task_create(2, task_b, stack_task_b, TASK_STACK_WORDS);
}
