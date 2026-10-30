#include "prio_rtos.h"
#include "kernel_internal.h"
#include "uart.h"

CrashDump_t g_crash_dump;

__attribute__((noreturn)) void kernel_panic(const char *msg) {
    // 1. 모든 인터럽트 비활성화 (추가 오작동 차단)
    // 즉, 가능할 수 있는 오류 상황이 더 이상 일어나지 않게 
    // 인터럽트를 영구차단한다.
    asm volatile("csrci mstatus, 8"); // mstatus.MIE = 0

    asm volatile("csrr %0, mcause" : "=r"(g_crash_dump.mcause));
    asm volatile("csrr %0, mepc" : "=r"(g_crash_dump.mepc));
    asm volatile("csrr %0, mtval" : "=r"(g_crash_dump.mtval));
    asm volatile("mv %0, sp" : "=r"(g_crash_dump.sp));
    g_crash_dump.task_id = g_current_tcb != 0 ? g_current_tcb->task_id : UINT32_MAX;
    g_crash_dump.reserved_pad = 0U;

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
