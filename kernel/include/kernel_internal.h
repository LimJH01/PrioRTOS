#ifndef KERNEL_INTERNAL_H
#define KERNEL_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>

// 시스템 설정 상수
#define MAX_TASKS  8
#define IDEL_TASK_PRIORITY 0 // IDLE 태스크 우선순위는 최저
#define MAX_PRIORITY  (MAX_TASKS-1)
/*context 구조체
RISC-V에 따라 context switching 시 저장해야할 레지스터들
총 13개의 레지스터
*/
typedef struct { 
    uint32_t s0;    // x8: Frame Pointer / Saved Register 0
    uint32_t s1;    // x9: Saved Register 1
    uint32_t s2;    // x18
    uint32_t s3;    // x19
    uint32_t s4;    // x20
    uint32_t s5;    // x21
    uint32_t s6;    // x22
    uint32_t s7;    // x23
    uint32_t s8;    // x24
    uint32_t s9;    // x25
    uint32_t s10;   // x26
    uint32_t s11;   // x27
    uint32_t ra;    // x1 : Return Address 
    uint32_t pad[3]; // 16 바이트 스택 정렬 맞춤용

}TaskContext_t;

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
extern TCB_t *g_current_tcb; // 다음 차례의 태스크 주소
extern uint32_t g_ready_bitmap; // 비트 0-7 : 태스트 준비 여부 플래그

//=========================================================================//
/*O(1) 비트맵 스케줄러 매크로*/
// 비트맵에서 가장 높은 우선순위를 1사이클 만에 추출
#define SCHED_GET_HIGHEST_PRIO(bitmap) (31 - __builtin_clz(bitmap)) // 가종 높은 1 찾기

// 특정 우선순위 비트 조작
#define SCHED_SET_READY(prio) (g_ready_bitmap |=(1U << (prio))) // task on
#define SCHED_CLEAR_READY(prio) (g_ready_bitmap &= ~(1U << (prio))) // task off

//=========================================================================//
/* 어셈블리 및 커널 내부 엔진 함수*/
extern void slot_context_switch(TCB_t *prev, TCB_t *next);
extern void slot_context_first_switch(TCB_t *next);

//sched.c 내부 함수
void sched_schedule(void);

#endif