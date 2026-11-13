#include "clint.h"
#include "uart.h"
#include "prio_rtos.h"
#include "kernel_internal.h"

volatile uint32_t g_in_trap_context = 0U;
extern void trap_entry(void);

void trap_init(void){
    asm volatile ("csrw mtvec, %0" :: "r"((uintptr_t)trap_entry));
}

__attribute__((noreturn)) void unhandled_trap(void) {
    uint32_t mcause;

    asm volatile("csrci mstatus, 8");
    asm volatile("csrr %0, mcause" : "=r"(mcause));
    asm volatile("csrr %0, mepc" : "=r"(g_crash_dump.mepc));
    asm volatile("csrr %0, mtval" : "=r"(g_crash_dump.mtval));
    asm volatile("mv %0, sp" : "=r"(g_crash_dump.sp));

    g_crash_dump.mcause = mcause;
    g_crash_dump.task_id = g_current_tcb != NULL
                               ? g_current_tcb->task_id
                               : UINT32_MAX;
    g_crash_dump.reserved_pad = 0U;

    (void)uart_puts("\n[KERNEL UNHANDLED TRAP]\n");

    while (1) {
        asm volatile("wfi");
    }
}
// mepc 확인
// mret 확인
// __attribute__((interrupt("machine")))
void trap_handler(void) {
    uint32_t mcause;
    asm volatile("csrr %0, mcause" : "=r"(mcause));

    g_in_trap_context = 1U;

    if (mcause == 0x80000003U) {
        // Machine Software Interrupt (MSIP) Bottom-Half.
        clint_clear_software_interrupt();

        if (g_current_tcb == NULL) {
            kernel_panic("ERR_NO_CURRENT_TASK");
        }

        // Mutex 보유 중이 아닐 때만 스케줄러 실행 (조기 return 불필요)
        if (g_current_tcb->held_mutex == NULL) {
            sched_schedule();
        }
    } else if (mcause == 0x80000007U) {
        // Machine Timer Interrupt (MTI) Top-Half.
        clint_set_next_timer();
        timer_tick_step();
    } else {
        // 알 수 없는 트랩은 여기서만 처리
        unhandled_trap();
    }

    // 모든 정상 트랩 경로가 단 한 곳에서 플래그를 복구하고 종료
    g_in_trap_context = 0U;
}
