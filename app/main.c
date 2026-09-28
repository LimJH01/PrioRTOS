#include <stdint.h>

// .bss 영역에 배치되는 시스템 전용 스택 (2KB)
uint8_t g_system_stack[2048] __attribute__((aligned(16)));

// QEMU virt 보드의 16550 UART 레지스터 주소
#define UART0_BASE 0x10000000
#define UART_THR   ((volatile uint8_t *)(UART0_BASE + 0x00))
#define UART_LSR   ((volatile uint8_t *)(UART0_BASE + 0x05))
#define UART_LSR_EMPTY_MASK 0x20

void uart_putc(char c){
    while ((*UART_LSR & UART_LSR_EMPTY_MASK) == 0);
    *UART_THR = (uint8_t)c;
}

void uart_puts(const char *str){
    while (*str) {
        if (*str == '\n') {
            uart_putc('\r');
        }
        uart_putc(*str++);
    }
}

int main(void){
    uart_puts("\n===================================\n");
    uart_puts("  Hello SlotRTOS Bare-metal Boot!  \n");
    uart_puts("  BSS Zero-Clear & SP Init OK.     \n");
    uart_puts("===================================\n\n");

    while (1) {
        // 인터럽트 대기
    }
    return 0;
}