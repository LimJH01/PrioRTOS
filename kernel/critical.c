#include "prio_rtos.h"
#include "kernel_internal.h"

#include <stdint.h>

/*
 * Critical section 상태는 단일 Hart에서만 사용되는 커널 전역 상태다.
 *
 * critical_nesting은 현재 Critical section 안에 몇 번 진입해 있는지를
 * 기록한다. 커널 함수 A가 Critical section 안에서 다시 커널 함수 B를
 * 호출하고, B도 Critical section을 사용하더라도 인터럽트를 중간에
 * 다시 켜면 안 된다. 따라서 가장 바깥쪽 enter/exit 쌍만 인터럽트 상태를
 * 저장하고 복원한다.
 */
static uint32_t critical_nesting;
/* 가장 바깥쪽 enter_critical() 직전의 mstatus를 보관한다. */
static uint32_t critical_saved_mstatus;

/* RISC-V mstatus의 Machine Interrupt Enable 비트(bit 3). */
#define MSTATUS_MIE (1U << 3)

void enter_critical(void) {
    assert_task_context();

    uint32_t current_mstatus;

    /*
     * 현재 mstatus를 읽으면서 MIE를 동시에 0으로 만든다.
     * 단일 Hart에서는 이 한 명령으로 인터럽트가 들어올 수 있는 구간과
     * 현재 인터럽트 허용 상태의 확인을 하나로 묶을 수 있다.
     */
    asm volatile("csrrc %0, mstatus, %1"
                 : "=r"(current_mstatus)
                 : "r"(MSTATUS_MIE)
                 : "memory");

    /*
     * counter가 포화된 상태에서 다시 증가시키면 0으로 돌아가서
     * Critical section이 끝나지 않았는데 MIE가 복원될 수 있다.
     * 이는 커널 상태 손상이므로 즉시 fail-stop 처리한다.
     */
    if (critical_nesting == UINT32_MAX) {
        kernel_panic("ERR_CRITICAL_OVERFLOW");
    }

    /*
     * 첫 진입에서만 원래 MIE 상태를 저장한다.
     * 처음부터 인터럽트가 꺼져 있었다면 마지막 exit에서도 인터럽트를
     * 켜지 않아야 한다. 중첩 진입에서는 기존 저장값을 덮어쓰지 않는다.
     */
    if (critical_nesting == 0U) {
        critical_saved_mstatus = current_mstatus;
    }

    /*
     * 컴파일러와 메모리 시스템이 보호 대상 상태의 접근을 Critical section
     * 전후로 재배치하지 못하도록 순서를 확정한다.
     */
    asm volatile("fence rw, rw" ::: "memory");
    ++critical_nesting;
}

void exit_critical(void) {
    assert_task_context();

    /* enter 없이 exit하면 보호 깊이 장부가 깨진 것이므로 패닉 처리한다. */
    if (critical_nesting == 0U) {
        kernel_panic("ERR_CRITICAL_UNDERFLOW");
    }

    --critical_nesting;

    /*
     * 중첩 깊이가 아직 남아 있으면 바깥 Critical section이 끝나지 않았다.
     * 이때 MIE를 복원하면 내부 함수가 끝났다는 이유로 외부 함수의
     * 보호 구간이 깨지므로 인터럽트를 계속 차단한다.
     */
    asm volatile("fence rw, rw" ::: "memory");

    /*
     * 가장 바깥쪽 Critical section을 빠져나갈 때만 원래 MIE 상태를
     * 반영한다. 진입 당시 MIE가 0이었다면 현재도 0으로 유지한다.
     */
    if (critical_nesting == 0U &&
        (critical_saved_mstatus & MSTATUS_MIE) != 0U) {
        /* 보호된 메모리 갱신이 끝난 뒤 인터럽트를 허용한다. */
        asm volatile("fence rw, rw" ::: "memory");
        asm volatile("csrs mstatus, %0"
                     :: "r"(MSTATUS_MIE)
                     : "memory");
    }
}
