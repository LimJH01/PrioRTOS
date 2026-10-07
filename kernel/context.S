# ==============================================================================
# [아키텍처 변경 이력]
# - 기존 (베어메탈 단계):
#     단일 실행 흐름(main) 전용. C 호출 규약상 오염되는 Caller-saved 16개만 보존(64B).
#     인터럽트 처리 후 항상 동일한 실행 지점으로 복귀하므로 sp 전환 불필요.
#
# - 변경 (선점형 RTOS 멀티태스킹 단계):
#     1 비동기 선점(Preemption) 지원: 태스크 임의 지점에서 중단되므로
#        Callee-saved(s0~s11) 및 CSR(mepc, mstatus)을 포함한 32개 풀 컨텍스트(128B) 보존 필수.
#     2 동적 태스크 전환: 인터럽트 진입 시 TCB->sp에 현재 스택을 백업하고,
#        스케줄러(sched_schedule) 판단에 따라 다음 TCB->sp로 sp를 교체하여 mret 복귀.
# ==============================================================================

    #include "stack_guard.h"

    .section .text
    .globl trap_entry
    .globl prio_context_first_switch
    .align 4

# ------------------------------------------------------------------------------
# prio_context_first_switch: 최초 태스크 디스패처 (부팅 완료 시 1회 호출)
# - 역할: 부팅 임시 컨텍스트(main)를 영구 폐기하므로 레지스터 백업(sw) 과정 생략.
# - 동작: task_create()가 TCB 스택에 사전 조각해 둔 가짜 프레임(128B)을
#         CPU 레지스터 및 mepc/mstatus에 복원(lw)한 뒤 mret으로 첫 태스크 진입.
# - 인자: a0 = 첫 번째로 실행할 태스크의 TCB 주소 (&g_tcbs[top_prio])
# ------------------------------------------------------------------------------
prio_context_first_switch:
    # mscratch stores the lowest permitted incoming sp before a trap frame.
    lw   t0, TCB_STACK_BASE_OFFSET(a0)
    addi t0, t0, 3
    andi t0, t0, -4
    addi t0, t0, STACK_GUARD_SIZE + STACK_TRAP_FRAME_SIZE
    csrw mscratch, t0

    # 1. TCB 구조체의 첫 번째 멤버인 sp를 읽어와 CPU의 sp로 교체
    lw   sp, 0(a0)

    # 2. task_create()가 심어둔 CSR 초기값 복원
    lw   t0, 124(sp)       # mstatus 초기값 (0x1880: MPIE=1, MPP=Machine Mode)
    csrw mstatus, t0
    lw   t1, 120(sp)       # mepc 초기값 (태스크 함수 시작 주소)
    csrw mepc, t1

    # 3. 가짜 스택에 사전 배치된 범용 레지스터 30개 복원
    lw   ra,  0(sp)
    lw   gp,  4(sp)
    lw   tp,  8(sp)
    lw   t0, 12(sp)
    lw   t1, 16(sp)
    lw   t2, 20(sp)
    lw   s0, 24(sp)
    lw   s1, 28(sp)
    lw   a0, 32(sp)
    lw   a1, 36(sp)
    lw   a2, 40(sp)
    lw   a3, 44(sp)
    lw   a4, 48(sp)
    lw   a5, 52(sp)
    lw   a6, 56(sp)
    lw   a7, 60(sp)
    lw   s2, 64(sp)
    lw   s3, 68(sp)
    lw   s4, 72(sp)
    lw   s5, 76(sp)
    lw   s6, 80(sp)
    lw   s7, 84(sp)
    lw   s8, 88(sp)
    lw   s9, 92(sp)
    lw   s10, 96(sp)
    lw   s11, 100(sp)
    lw   t3, 104(sp)
    lw   t4, 108(sp)
    lw   t5, 112(sp)
    lw   t6, 116(sp)

    # 4. 가짜 스택 프레임(128B) 해제
    addi sp, sp, 128

    # 5. mepc로 하드웨어 점프 (최초 태스크 함수 진입)
    mret


# ------------------------------------------------------------------------------
# trap_entry: 선점형 인터럽트/트랩 공용 핸들러
# - 역할: 실행 중인 태스크의 문맥을 보존하고, 필요 시 다음 우선순위 태스크로 제어권 이양.
# - 단계:
#     1 현재 태스크 스택에 풀 컨텍스트(128B) 백업 (sw)
#     2 g_current_tcb->sp 갱신 (현재 스택 포인터 동기화)
#     3 trap_handler() 호출 (틱 갱신 및 sched_schedule에 의한 g_current_tcb 변경 가능)
#     4 갱신된 g_current_tcb->sp로 CPU sp 교체
#     5 선택된 태스크의 128B 컨텍스트 복원(lw) 후 mret
# ------------------------------------------------------------------------------
trap_entry:
    # Check frame headroom without modifying a task register.
    csrrw t0, mscratch, t0
    bltu sp, t0, .Lstack_overflow
    csrrw t0, mscratch, t0

    # --------------------------------------------------------------------------
    # 1 현재 태스크 스택에 128바이트 풀 프레임 확보 및 레지스터 백업
    # --------------------------------------------------------------------------
    addi sp, sp, -128

    sw   ra,  0(sp)
    sw   gp,  4(sp)
    sw   tp,  8(sp)
    sw   t0, 12(sp)
    sw   t1, 16(sp)
    sw   t2, 20(sp)
    sw   s0, 24(sp)
    sw   s1, 28(sp)
    sw   a0, 32(sp)
    sw   a1, 36(sp)
    sw   a2, 40(sp)
    sw   a3, 44(sp)
    sw   a4, 48(sp)
    sw   a5, 52(sp)
    sw   a6, 56(sp)
    sw   a7, 60(sp)
    sw   s2, 64(sp)
    sw   s3, 68(sp)
    sw   s4, 72(sp)
    sw   s5, 76(sp)
    sw   s6, 80(sp)
    sw   s7, 84(sp)
    sw   s8, 88(sp)
    sw   s9, 92(sp)
    sw   s10, 96(sp)
    sw   s11, 100(sp)
    sw   t3, 104(sp)
    sw   t4, 108(sp)
    sw   t5, 112(sp)
    sw   t6, 116(sp)

    # 인터럽트 발생 당시의 하드웨어 CSR 상태 백업
    csrr t0, mstatus
    sw   t0, 124(sp)
    csrr t1, mepc
    sw   t1, 120(sp)

    # Check the task's bottom-of-stack canary after the frame is safely saved.
    la   t0, g_current_tcb
    lw   t0, 0(t0)
    lw   t0, TCB_STACK_BASE_OFFSET(t0)
    addi t0, t0, 3
    andi t0, t0, -4

    lw   t1, 0(t0)
    li   t2, STACK_GUARD_WORD_0
    bne  t1, t2, .Lstack_overflow
    lw   t1, 4(t0)
    li   t2, STACK_GUARD_WORD_1
    bne  t1, t2, .Lstack_overflow
    lw   t1, 8(t0)
    li   t2, STACK_GUARD_WORD_2
    bne  t1, t2, .Lstack_overflow
    lw   t1, 12(t0)
    li   t2, STACK_GUARD_WORD_3
    bne  t1, t2, .Lstack_overflow

    # --------------------------------------------------------------------------
    # 2 g_current_tcb->sp에 현재 스택 포인터 동기화
    # --------------------------------------------------------------------------
    la   t0, g_current_tcb
    lw   t1, 0(t0)          # t1 = g_current_tcb
    sw   sp, 0(t1)          # g_current_tcb->sp = sp

    # --------------------------------------------------------------------------
    # 3 C 언어 트랩 핸들러 호출 (틱 처리 및 스케줄러 실행)
    #    - trap_handler() 내부에서 g_current_tcb가 교체될 수 있음
    # --------------------------------------------------------------------------
    # Run kernel C code on the dedicated system stack, not the task stack.
    la   sp, _stack_top
    andi sp, sp, -16
    call trap_handler

    # --------------------------------------------------------------------------
    # 4 (스케줄링 반영) 최종 g_current_tcb가 가리키는 태스크의 스택으로 전환
    # --------------------------------------------------------------------------
    la   t0, g_current_tcb
    lw   t1, 0(t0)          # t1 = 스케줄러가 결정한 태스크의 TCB

    # Update the trap-frame limit for the task about to resume.
    lw   t0, TCB_STACK_BASE_OFFSET(t1)
    addi t0, t0, 3
    andi t0, t0, -4
    addi t0, t0, STACK_GUARD_SIZE + STACK_TRAP_FRAME_SIZE
    csrw mscratch, t0

    lw   sp, 0(t1)          # CPU의 sp를 해당 태스크의 스택 주소로 교체

    # --------------------------------------------------------------------------
    # 5 태스크의 CSR 및 범용 레지스터 복원 후 mret으로 복귀
    # --------------------------------------------------------------------------
    lw   t0, 124(sp)
    csrw mstatus, t0
    lw   t1, 120(sp)
    csrw mepc, t1

    lw   ra,  0(sp)
    lw   gp,  4(sp)
    lw   tp,  8(sp)
    lw   t0, 12(sp)
    lw   t1, 16(sp)
    lw   t2, 20(sp)
    lw   s0, 24(sp)
    lw   s1, 28(sp)
    lw   a0, 32(sp)
    lw   a1, 36(sp)
    lw   a2, 40(sp)
    lw   a3, 44(sp)
    lw   a4, 48(sp)
    lw   a5, 52(sp)
    lw   a6, 56(sp)
    lw   a7, 60(sp)
    lw   s2, 64(sp)
    lw   s3, 68(sp)
    lw   s4, 72(sp)
    lw   s5, 76(sp)
    lw   s6, 80(sp)
    lw   s7, 84(sp)
    lw   s8, 88(sp)
    lw   s9, 92(sp)
    lw   s10, 96(sp)
    lw   s11, 100(sp)
    lw   t3, 104(sp)
    lw   t4, 108(sp)
    lw   t5, 112(sp)
    lw   t6, 116(sp)

    addi sp, sp, 128

    mret

.Lstack_overflow:
    # Do not call C on a task stack that may already be exhausted.
    la   sp, _stack_top
    call stack_overflow_panic
1:
    wfi
    j    1b