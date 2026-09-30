#include <stdint.h>
#include "uart.h"

int main(void){

    uart_init();

    uart_puts("\n===================================\n");
    uart_puts("  Hello PrioRTOS Bare-metal Boot!  \n");
    uart_puts("  BSS Zero-Clear & SP Init OK.     \n");
    uart_puts("===================================\n\n");

    while (1) {
        // 인터럽트 대기
    }
    return 0;
}