#include "clint.h"
#include "uart.h"

void trap_handler(){
    
    // macuse, trap이 어떤 사유로 발생했는지 들어감
    uint32_t mcause;
    asm volatile("csrr %0, mcause" : "=r"(mcause));

    // 0x80000007, CLINT 인터럽트
    if (mcause == 0x80000007){
        clint_set_next_timer();
        uart_putc('.');
    }
}