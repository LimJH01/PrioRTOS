#include "clint.h"
#include "uart.h"
#include "kernel_internal.h"

extern void trap_entry(void);

void trap_init(void){
    asm volatile ("csrw mtvec, %0" :: "r"((uintptr_t)trap_entry));
}

// mepc 확인
// mret 확인
// __attribute__((interrupt("machine")))

void trap_handler(){
    
    // macuse, trap이 어떤 사유로 발생했는지 들어감
    uint32_t mcause;
    asm volatile("csrr %0, mcause" : "=r"(mcause));

    // 0x80000007, CLINT 인터럽트 사유임
    if (mcause == 0x80000007){
        clint_set_next_timer();
        uart_putc('.');

        sched_schedule();
    }else if (mcause == 11U){
        TaskContext_t *context = (TaskContext_t *)g_current_tcb->sp;
        context->mepc += 4U;
        sched_schedule();
    }else{
        while(1){
            asm volatile ("wfi");
        }
    }
}
