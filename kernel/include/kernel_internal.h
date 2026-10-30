#ifndef KERNEL_INTERNAL_H
#define KERNEL_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "prio_rtos.h"
#include "stack_guard.h"

// Master Spec 고정 용량. 태스크 슬롯과 mutex 중첩 깊이는 런타임에 늘어나지 않는다.
#define OS_MAX_TASKS         32U
#define OS_MAX_MUTEX_NESTING 4U
#define IDLE_TASK_PRIORITY   (OS_MAX_TASKS - 1U) // 슬롯 31: 항상 최저 우선순위인 IDLE
#define MAX_PRIORITY         (OS_MAX_TASKS - 1U) // 우선순위 범위: 0(최고) ~ 31(최저)

/* 기존 내부 코드와의 호환 별칭. 새로운 자료구조/API는 OS_* 상수를 사용한다. */
#define MAX_TASKS OS_MAX_TASKS

_Static_assert(OS_MAX_TASKS == 32U, "OS_MAX_TASKS must be 32");

/*
 * 태스크가 트랩에 진입할 때 저장되는 전체 실행 문맥.
 * context.S의 trap_entry가 이 구조체와 동일한 128바이트 프레임을 사용한다.
 * x2(sp)는 프레임 밖에서 TCB_t::sp로 관리하므로 여기에는 저장하지 않는다.
 *
 * - 원래: RISC-V 표준 C 호출 규약(Calling Convention)에 따라, 자발적 양보시 
 *         Callee-saved 레지스터만 보존하면 되었기에 총 13개(s0~s11, ra) 레지스터로 설계되었음.
 * - 변경: CLINT 타이머 인터럽트 기반 Preemptive Trap과 IPCP 스케줄링 환경에서는 
 *         태스크 임의 지점에서 레지스터가 오염될 수 있고, mepc/mstatus 보존 및 16바이트 스택 정렬이 
 *         필수적이기 때문에 총 32개(128바이트, 범용 30개 + mepc/mstatus) 풀 컨텍스트 프레임으로 바꿨음.
 */
typedef struct {
    // 범용 레지스터 30개: x1(ra), x3~x31. x0(zero)는 항상 0이므로 저장하지 않는다.
    uint32_t ra;
    uint32_t gp;
    uint32_t tp;
    uint32_t t0, t1, t2;
    uint32_t s0, s1;
    uint32_t a0, a1, a2, a3, a4, a5, a6, a7;
    uint32_t s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    uint32_t t3, t4, t5, t6;

    // 트랩 복귀 제어 레지스터: mret 시 복귀할 PC와 머신 상태.
    uint32_t mepc;    // 태스크 실행 복귀 PC 주소
    uint32_t mstatus; // 머신 모드 인터럽트 제어 상태 (MPIE, MPP 등)
} TaskContext_t; // 32개 * 4바이트 = 128바이트

//=========================================================================//
/* 태스크 상태 및 TCB */

typedef enum {
    TASK_STATE_UNUSED  = 0, // 아직 등록되지 않은 슬롯
    TASK_STATE_READY   = 1, // 실행 가능하며 ready bitmap에 포함
    TASK_STATE_RUNNING = 2, // 현재 CPU에서 실행 중인 유일한 태스크
    TASK_STATE_BLOCKED = 3  // semaphore/mutex 등으로 대기 중
} TaskState_t;

/*
 * 정적 mutex의 소유권/priority shadowing 상태.
 * mutex 하나는 8바이트로 고정되며, 보유 mutex 연결 리스트는 TCB_t의
 * held_mutex에서 시작해 prev_mutex 방향으로 연결된다.
 */
typedef struct mutex {
    uint8_t ceiling_prio; // 이 mutex가 요구하는 ceiling priority
    uint8_t saved_prio;   // lock 직전 소유 태스크의 유효 priority
    uint8_t locked;       // 0: 미획득, 1: 현재 태스크가 획득
    uint8_t padding;      // 32비트 정렬 및 8바이트 레이아웃을 위한 예약 바이트
    struct mutex *prev_mutex; // 중첩 mutex의 이전 항목(LIFO)
} mutex_t;

/*
 * 태스크 슬롯 하나의 커널 관리 블록(Task Control Block).
 * task_id == base_priority == 슬롯 번호라는 직접 매핑을 유지한다.
 * priority만 mutex ceiling에 의해 일시적으로 변경될 수 있다.
 */
typedef struct TCB {
    uint32_t *sp;              // 저장된 trap frame의 시작 주소
    mutex_t *held_mutex;       // 현재 보유한 mutex의 최상단; 없으면 NULL
    uint32_t stack_size_bytes; // 태스크 스택 전체 크기(guard 포함)
    uint32_t reserved;         // Master Spec 예약 영역, 항상 0으로 초기화
    uint8_t *stack_base;       // 16바이트 canary가 시작되는 스택 하단
    uint8_t priority;          // 현재 유효 priority(숫자가 작을수록 높음)
    uint8_t base_priority;     // 등록 당시 priority, 복원 기준
    uint8_t task_id;           // 고정 슬롯 ID(0~31)
    uint8_t state;             // TaskState_t 값
} TCB_t;

/*
 * 치명적 오류 발생 시 보존하는 최소 진단 정보.
 * RV32 기준 6개 워드, 총 24바이트이며 kernel_panic()에서 기록한다.
 */
typedef struct {
    uint32_t mcause;       // trap 원인
    uint32_t mepc;         // 오류가 발생한/복귀할 PC
    uint32_t mtval;        // trap 관련 추가 값
    uint32_t sp;           // 오류 시점의 스택 포인터
    uint32_t task_id;      // 오류를 일으킨 태스크 ID
    uint32_t reserved_pad; // 향후 확장을 위한 예약 영역
} CrashDump_t;

_Static_assert(sizeof(mutex_t) == 8U, "mutex_t size mismatch");
_Static_assert(sizeof(TCB_t) == 24U, "TCB_t size mismatch");
_Static_assert(sizeof(Semaphore_t) == 8U, "Semaphore_t size mismatch");
_Static_assert(sizeof(CrashDump_t) == 24U, "CrashDump_t size mismatch");
_Static_assert(offsetof(TCB_t, stack_base) == TCB_STACK_BASE_OFFSET,
               "TCB stack_base offset must match context.S");

//=========================================================================//
/* 커널 전역 상태 장부*/

extern TCB_t g_tcbs[MAX_TASKS]; // 태스크 장부
extern TCB_t *g_current_tcb; // 현재 태스크 주소
extern uint32_t g_ready_bitmap; // 비트 0-31: 태스크 슬롯 준비 여부 플래그
extern CrashDump_t g_crash_dump;

//=========================================================================//
//=========================================================================//
/* 어셈블리 및 커널 내부 엔진 함수*/
extern void prio_context_first_switch(TCB_t *next);
__attribute__((noreturn)) void stack_overflow_panic(void);

//sched.c 내부 함수
void sched_schedule(void);
void sched_request_reschedule(void);
uint32_t sched_irq_save(void);
void sched_irq_restore(uint32_t saved_mstatus);
bool sched_block_current(uint32_t saved_mstatus);
bool sched_wake_task(uint32_t slot);
uint32_t sched_highest_priority_slot(uint32_t task_bitmap);

// IPCP configuration and scheduler integration.
bool ipcp_validate_configuration(void);
void ipcp_scheduler_started(void);

#endif
