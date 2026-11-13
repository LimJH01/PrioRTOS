#include "clint.h"
#include "uart.h"
#include "prio_rtos.h"
#include "kernel_internal.h"

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

void trap_handler(){
    
    // macuse, trap이 어떤 사유로 발생했는지 들어감
    uint32_t mcause;
    asm volatile("csrr %0, mcause" : "=r"(mcause));

    if (mcause == 0x80000003U) {
        // Machine Software Interrupt (MSIP), bottom half (여기서 직접 스케줄링)
        clint_clear_software_interrupt();
        sched_schedule();
    } else if (mcause == 0x80000007U) {
        // Machine Timer Interrupt (MTI), top half (여기서 타이머를 갱신, 틱 처리, 직접 스케줄링 X)
        clint_set_next_timer();
        timer_tick_step();
    } else {
        unhandled_trap();
    }
}
