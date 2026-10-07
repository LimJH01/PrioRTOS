#include <stdint.h>
#include "uart.h"
#include "clint.h"
#include "tasks.h"
#include "prio_rtos.h"

int main(void){

    uart_init();

    uart_puts("\n===================================\n");
    uart_puts("  Hello PrioRTOS Bare-metal Boot!  \n");
    uart_puts("  BSS Zero-Clear & SP Init OK.     \n");
    uart_puts("===================================\n\n");

    trap_init();
    clint_timer_init();

    // [주의] mstatus.MIE(전역 인터럽트)를 main()에서 직접 활성화 금지!
    // priortos_start() 호출 전에 인터럽트가 발생하면 g_current_tcb(NULL) 역참조로 커널 크래시 발생.
    // 전역 인터럽트는 첫 태스크 디스패치 시 prio_context_first_switch의 mret(0x1880, MPIE=1)을 통해 하드웨어가 자동 활성화함.
    // asm volatile("csrs mstatus, %0" :: "r"(1 << 3));

    uart_puts("== timer interrupt enabled ==\n");

    tasks_init();
    priortos_start();

    while (1) {
        // runtime execution
        asm volatile ("wfi"); 
    }
    return 0;
}