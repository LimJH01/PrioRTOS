#include "prio_rtos.h"
#include "kernel_internal.h"
#include <stddef.h>

_Static_assert(sizeof(TaskContext_t) == STACK_TRAP_FRAME_SIZE,
               "TaskContext_t must match the assembly trap frame size");
_Static_assert(offsetof(TCB_t, stack_base) == TCB_STACK_BASE_OFFSET,
               "TCB stack_base offset must match context.S");

TCB_t g_tcbs[MAX_TASKS];
TCB_t *g_current_tcb = 0;
uint32_t g_ready_bitmap = 0;

// O(1)에 top_prio를 찾기 위한 어셈블리 매크로
#define SCHED_GET_HIGHEST_PRIO(bitmap) (31 - __builtin_clz(bitmap))

// 태스크가 while(1)을 탈출해 실수로 리턴했을 때 잡기 위한 안전 덫
static void task_exit_trap(void) {
    while (1) {
        asm volatile ("wfi");
    }
}

__attribute__((noreturn)) void stack_overflow_panic(void) {
    kernel_panic("task stack overflow");
}

TaskCreateResult_t task_create(uint32_t prio, void (*task_func)(void), uint8_t *stack, uint32_t stack_size_bytes) {
    // 1. 유효성 검증
    if (prio >= MAX_TASKS){
        return TASK_CREATE_ERR_INVALID_PRIO;
    }
    if (g_ready_bitmap & (1U << prio)){
        return TASK_CREATE_ERR_PRIO_IN_USE;
    }
    if (task_func == 0 || stack == 0){
        return TASK_CREATE_ERR_NULL_PTR;
    }
    if (stack_size_bytes < STACK_GUARD_SIZE + sizeof(TaskContext_t)){
        return TASK_CREATE_ERR_STACK_TOO_SMALL;
    }

    // 2. 스택의 최상단(Top) 주소 계산
    // 배열 시작 주소에 바이트 단위 크기를 더해 스택의 끝 주소를 계산
    uintptr_t stack_start = (uintptr_t)stack;
    if (stack_size_bytes > UINTPTR_MAX - stack_start){
        return TASK_CREATE_ERR_ADDRESS_OVERFLOW;
    }
    uintptr_t stack_top = stack_start + stack_size_bytes;

    // 3. 16바이트 정렬 보정 (하위 4비트를 0으로 밀어서 버림)
    stack_top &= ~((uintptr_t)0xF);

    // 4. 정렬된 guard 영역과 가짜 스택 프레임(128바이트) 공간 확보
    // 높은 주소에서 128바이트만큼 밑으로 내려와 프레임의 시작 위치를 잡음
    uintptr_t guard_start = (stack_start + 3U) & ~((uintptr_t)0x3U);
    if (stack_top < guard_start ||
        stack_top - guard_start < STACK_GUARD_SIZE + sizeof(TaskContext_t)){
        return TASK_CREATE_ERR_STACK_TOO_SMALL;
    }
    TaskContext_t *ctx = (TaskContext_t *)(stack_top - sizeof(TaskContext_t));

    uint32_t *guard = (uint32_t *)guard_start;
    guard[0] = STACK_GUARD_WORD;
    guard[1] = STACK_GUARD_WORD;
    guard[2] = STACK_GUARD_WORD;
    guard[3] = STACK_GUARD_WORD;

    // 5. 프레임 전체를 0으로 초기화 (x1~x31 일반 레지스터 초기값)
    uint32_t *raw = (uint32_t *)ctx;
    for (uint32_t i = 0; i < sizeof(TaskContext_t) / sizeof(uint32_t); i++) {
        raw[i] = 0;
    }

    // 6. 핵심 제어 레지스터 주입
    // mepc: 태스크가 최초로 실행될 함수 시작 주소 주입
    // rtos_start나 스케줄러가 문맥 복원 후 'mret'(인터럽트 복귀 명령)을 실행하면
    // CPU가 mepc에 적힌 이 주소를 PC(Program Counter)로 읽어들여 함수를 실행함
    ctx->mepc = (uint32_t)task_func;

    // (2) ra: 만약 태스크 함수가 실수로 종료(리턴)했을 때 갈 주소
    ctx->ra = (uint32_t)task_exit_trap;

    // (3) mstatus: MPIE(bit 7) = 1, MPP(bits 12:11) = 3 (Machine Mode)
    // 0x1880 -> mret 복귀 시 Machine 모드를 유지하고 글로벌 인터럽트를 활성화(MIE=1)함
    ctx->mstatus = 0x1880;

    // 7. 완성된 스택 포인터를 TCB에 기록
    g_tcbs[prio].sp = (uint32_t *)ctx;
    g_tcbs[prio].priority = prio;
    g_tcbs[prio].base_priority = prio;
    g_tcbs[prio].state = TASK_STATE_READY;
    g_tcbs[prio].stack_base = stack;
    g_tcbs[prio].stack_size_bytes = stack_size_bytes;

    // 8. 스케줄러 레디 비트맵 활성화
    g_ready_bitmap |= (1U << prio);

    return TASK_CREATE_OK;
}

void priortos_start(void){
    if (g_ready_bitmap == 0){
        kernel_panic("scheduler started without ready tasks");
    }

    uint32_t top_prio = SCHED_GET_HIGHEST_PRIO(g_ready_bitmap);
    // []가 있으면 주소가 아닌 내용물, 따라서 &를 붙여야 한다.
    g_current_tcb = &g_tcbs[top_prio];
    g_current_tcb->state = TASK_STATE_RUNNING;

    prio_context_first_switch(g_current_tcb);
}


void sched_schedule(){
    if (g_ready_bitmap == 0){
        kernel_panic("scheduler has no ready tasks");
    }

    uint32_t top_prio = SCHED_GET_HIGHEST_PRIO(g_ready_bitmap);

    // 주소를 비교, (주소가 다르면 무조건 우선순위가 더 높음)
    if(g_current_tcb != &g_tcbs[top_prio]){
        if (g_current_tcb->state == TASK_STATE_RUNNING){
            g_current_tcb->state = TASK_STATE_READY;
        }
        g_current_tcb = &g_tcbs[top_prio];
        g_current_tcb->state = TASK_STATE_RUNNING;
    }
}

uint32_t sched_irq_save(void) {
    uint32_t saved_mstatus;
    uint32_t mie_mask = 1U << 3;
    asm volatile ("csrrc %0, mstatus, %1"
                  : "=r"(saved_mstatus)
                  : "r"(mie_mask)
                  : "memory");
    return saved_mstatus;
}

void sched_irq_restore(uint32_t saved_mstatus) {
    if (saved_mstatus & (1U << 3)) {
        asm volatile ("csrs mstatus, %0"
                      :: "r"(1U << 3)
                      : "memory");
    }
}

bool sched_block_current(uint32_t saved_mstatus) {
    TCB_t *self = g_current_tcb;
    if (self == NULL || self->state != TASK_STATE_RUNNING) {
        sched_irq_restore(saved_mstatus);
        return false;
    }

    self->state = TASK_STATE_BLOCKED;
    g_ready_bitmap &= ~(1U << self->priority);

    sched_irq_restore(saved_mstatus);
    asm volatile ("ecall" ::: "memory");
    return true;
}

bool sched_wake_task(uint32_t priority) {
    if (priority >= MAX_TASKS || g_tcbs[priority].state != TASK_STATE_BLOCKED) {
        return false;
    }

    g_tcbs[priority].state = TASK_STATE_READY;
    g_ready_bitmap |= (1U << priority);
    return true;
}

void task_yield(void) {
    asm volatile ("ecall" ::: "memory");
}