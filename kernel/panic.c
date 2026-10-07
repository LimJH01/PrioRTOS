#include "prio_rtos.h"
#include "uart.h"

void kernel_panic(const char *msg) {
    // 1. 모든 인터럽트 비활성화 (추가 오작동 차단)
    asm volatile("csrci mstatus, 8"); // mstatus.MIE = 0

    // 2. UART로 치명적 에러 메시지 출력
    if (msg) {
        uart_puts("\n[KERNEL PANIC] ");
        uart_puts(msg);
    }

    // 3. CPU 정지 (무한 저전력 대기)
    while (1) {
        asm volatile("wfi");
    }
}