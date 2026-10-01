#ifndef KERNEL_INTERNAL_H
#define KERNEL_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>

// 시스템 설정 상수
#define MAX_TASKS  8
#define IDLE_TASK_PRIORITY 0 // IDLE 태스크 우선순위는 최저
#define MAX_PRIORITY  (MAX_TASKS-1)
/* context 구조체
 * - 원래: RISC-V 표준 C 호출 규약(Calling Convention)에 따라, 자발적 양보시 
 *         Callee-saved 레지스터만 보존하면 되었기에 총 13개(s0~s11, ra) 레지스터로 설계되었음.
 * - 변경: CLINT 타이머 인터럽트 기반 Preemptive Trap과 IPCP 스케줄링 환경에서는 
 *         태스크 임의 지점에서 레지스터가 오염될 수 있고, mepc/mstatus 보존 및 16바이트 스택 정렬이 
 *         필수적이기 때문에 총 32개(128바이트, 범용 30개 + mepc/mstatus) 풀 컨텍스트 프레임으로 바꿨음.
 */
typedef struct {
    // 1. 범용 레지스터 30개 (zero 제외, sp는 TCB의 sp로 직접 관리)
    uint32_t ra;
    uint32_t gp;
    uint32_t tp;
    uint32_t t0, t1, t2;
    uint32_t s0, s1;
    uint32_t a0, a1, a2, a3, a4, a5, a6, a7;
    uint32_t s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    uint32_t t3, t4, t5, t6;

    // 2. 인터럽트/트랩 복귀 제어용 특수 레지스터 2개
    uint32_t mepc;    // 태스크 실행 복귀 PC 주소
    uint32_t mstatus; // 머신 모드 인터럽트 제어 상태 (MPIE, MPP 등)
} TaskContext_t; // 총 32개 * 4바이트 = 128바이트 (RISC-V 16바이트 스택 정렬 규약 완벽 만족)

//=========================================================================//
/* 태스크 상태 및 TCB */

typedef enum{
    TASK_STATE_UNUSED = 0,
    TASK_STATE_READY = 1,
    TASK_STATE_RUNNING = 2,
    TASK_STATE_BLOCKED = 3
}TaskState_t;

typedef struct TCB{
    uint32_t *sp; // 스택 포인터
    uint32_t priority; // 태스크 현재 우선순위 (0-7)
    uint32_t base_priority; // IPCP 에서 원래 우선순위 복원용
    TaskState_t state; // 태스크 상태
    uint32_t *stack_base; // 스택 시작 주소
    uint32_t stack_size; // 할당된 스택 크기

}TCB_t;

//=========================================================================//
/* 커널 전역 상태 장부*/

extern TCB_t g_tcbs[MAX_TASKS]; // 태스크 장부
extern TCB_t *g_current_tcb; // 현재 태스크 주소
extern uint32_t g_ready_bitmap; // 비트 0-7 : 태스트 준비 여부 플래그

//=========================================================================//
/*O(1) 비트맵 스케줄러 매크로*/
// 비트맵에서 가장 높은 우선순위를 1사이클 만에 추출
#define SCHED_GET_HIGHEST_PRIO(bitmap) (31 - __builtin_clz(bitmap)) // 가장 높은 1 찾기

// 특정 우선순위 비트 조작
#define SCHED_SET_READY(prio) (g_ready_bitmap |=(1U << (prio))) // task on
#define SCHED_CLEAR_READY(prio) (g_ready_bitmap &= ~(1U << (prio))) // task off

//=========================================================================//
/* 어셈블리 및 커널 내부 엔진 함수*/
extern void prio_context_switch(TCB_t *prev, TCB_t *next);
extern void prio_context_first_switch(TCB_t *next);

//sched.c 내부 함수
void sched_schedule(void);

#endif