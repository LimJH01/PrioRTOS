#include <stdint.h>
#include "uart.h"
#include "clint.h"

extern void trap_handler(void);
int main(void){

    uart_init();

    uart_puts("\n===================================\n");
    uart_puts("  Hello PrioRTOS Bare-metal Boot!  \n");
    uart_puts("  BSS Zero-Clear & SP Init OK.     \n");
    uart_puts("===================================\n\n");

    // trap_hander()의 주소를 인터럽트 발생시 처리할 주고 mtvec에 넣는다
    asm volatile ("csrw mtvec, %0" :: "r"((uintptr_t)trap_handler));
    clint_set_next_timer();
    // mie -> machine instruction enable, bit 7을 1로 나머지는 0
    // 7번 bit인 이유 -> timer interrupt 허용이기 때문
    asm volatile("csrs mie, %0" :: "r"(1 << 7));
    // machine status, 전체 인터럽트 허용/차단 여부 결정
    // bit 3번은 mie 
    // (mstatus.MIE = Bit 3) , 이중 차단 구조
    asm volatile("csrs mstatus, %0" :: "r"(1 << 3));

    uart_puts("== timer interrupt enabled ==\n");

    while (1) {
        // runtime execution
        asm volatile ("wfi");
    }
    return 0;
}