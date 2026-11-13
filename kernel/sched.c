#include "prio_rtos.h"
#include "kernel_internal.h"
#include "clint.h"
#define STACK_ALIGNED __attribute__((aligned(16)))

_Static_assert(sizeof(TaskContext_t) == STACK_TRAP_FRAME_SIZE,
               "TaskContext_t must match the assembly trap frame size");
#define IDLE_STACK_SIZE_BYTES 1024U
static STACK_ALIGNED uint8_t idle_stack[IDLE_STACK_SIZE_BYTES];

TCB_t g_tcbs[MAX_TASKS];
TCB_t *g_current_tcb = 0;
uint32_t g_ready_bitmap = 0;

/*
 * 최하위 set bit(LSB)가 가장 높은 우선순위라는 직접 매핑 규칙을 이용해
 * 준비/대기 bitmap에서 슬롯을 O(1)으로 선택한다.
 *
 * 빈 bitmap도 별도 분기 없이 처리하기 위해 IDLE 슬롯인 bit 31을
 * Guard Bit으로 합성한다. 따라서 task_bitmap이 0이면 selector는
 * 자동으로 slot 31을 반환한다.
 *
 * 0x077CB531은 32비트 De Bruijn multiplier이며, LUT는 고립된 한 비트의
 * 인덱스를 0..31 슬롯 번호로 변환한다. 이 함수의 반복문과 bitmap 순회는
 * 없으므로 선택 비용은 입력 bitmap과 무관하게 일정하다.
 */
static const uint8_t debruijn_lsb_index[32] = {
    0U,  1U,  28U, 2U,  29U, 14U, 24U, 3U,
    30U, 22U, 20U, 15U, 25U, 17U, 4U,  8U,
    31U, 27U, 13U, 23U, 21U, 19U, 16U, 7U,
    26U, 12U, 18U, 6U,  11U, 5U,  10U, 9U
};

uint32_t sched_highest_priority_slot(uint32_t task_bitmap) {
    const uint32_t safe_bitmap = task_bitmap | (1U << IDLE_TASK_PRIORITY);
    const uint32_t isolated_lsb = safe_bitmap & (0U - safe_bitmap);
    uint32_t lut_index =
        (isolated_lsb * 0x077CB531U) >> 27;
    return debruijn_lsb_index[lut_index];
}

/*
 * 현재 ready bitmap에서 다음 태스크를 선택하고 그 태스크의 READY bit를
 * 제거한다. bitmap이 비어 있어도 Guard Bit 덕분에 IDLE TCB(slot 31)이
 * 선택된다.
 *
 * 이 함수는 TCB 상태를 RUNNING으로 바꾸지 않는다. 호출자가 현재 태스크의
 * 상태를 먼저 정리한 뒤, 반환된 TCB를 RUNNING으로 전환해야 한다.
 */
TCB_t *sched_select_next_task(void) {
    const uint32_t next_slot = sched_highest_priority_slot(g_ready_bitmap);
    g_ready_bitmap &= ~(1U << next_slot);
    return &g_tcbs[next_slot];
}

// 태스크가 while(1)을 탈출했을 때 kernel_panic 발생
__attribute__((noreturn)) static void task_exit_trap(void) {
    kernel_panic("task returned");
}

__attribute__((noreturn)) void stack_overflow_panic(void) {
    kernel_panic("task stack overflow");
}

TaskCreateResult_t task_create(uint32_t prio, void (*task_func)(void), uint8_t *stack, uint32_t stack_size_bytes) {
    // 1. 유효성 검증
    if (prio >= MAX_TASKS){
        return TASK_CREATE_ERR_INVALID_PRIO;
    }
    if (g_tcbs[prio].state != TASK_STATE_UNUSED){
        kernel_panic("duplicate priority");
    }
    if (task_func == 0 || stack == 0){
        return TASK_CREATE_ERR_NULL_PTR;
    }
    if (((uintptr_t)stack & 0x3U) != 0U) {
        return TASK_CREATE_ERR_STACK_MISALIGNED;
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
    g_tcbs[prio].held_mutex = NULL;
    g_tcbs[prio].reserved = 0U;
    g_tcbs[prio].priority = (uint8_t)prio;
    g_tcbs[prio].base_priority = (uint8_t)prio;
    g_tcbs[prio].task_id = (uint8_t)prio;
    g_tcbs[prio].state = TASK_STATE_READY;
    g_tcbs[prio].stack_base = stack;
    g_tcbs[prio].stack_size_bytes = stack_size_bytes;
    // 새 슬롯이 이전 태스크의 sleep 잔여값을 물려받지 않도록 초기화한다.
    g_sleep_remaining_ticks[prio] = 0U;

    // 8. 스케줄러 레디 비트맵 활성화
    g_ready_bitmap |= (1U << prio);

    return TASK_CREATE_OK;
}

void idle_task(void){
    while (1){
        asm volatile ("wfi");
    }
}

void priortos_start(void) {
    // 1. 최하위 우선순위 31번에 IDLE 태스크 등록 (비트맵 31번 비트 영구 세트)
    // 즉 시스템 최후의 방어선 31번 IDLE 태스크를 만드는것 (user가 만드는것이 아님)
    TaskCreateResult_t result = 
        task_create(IDLE_TASK_PRIORITY, idle_task, idle_stack, sizeof(idle_stack));

    if (result != TASK_CREATE_OK) {
        kernel_panic("failed to create idle task");
    }

    // 2. Guard Bit selector로 첫 번째 실행 태스크를 선택한다.
    TCB_t *first_task = sched_select_next_task();

    // 3. 직접 매핑으로 TCB를 확정하고 RUNNING으로 전이한다.
    g_current_tcb = first_task;
    g_current_tcb->state = TASK_STATE_RUNNING;

    // 5. 최초 태스크 문맥 복원 및 전역 인터럽트 활성화 (mret)
    prio_context_first_switch(g_current_tcb);
}


void sched_schedule(){
    TCB_t *current = g_current_tcb;

    /*
     * Mutex 보유 구간에서는 Timer/MSIP가 이미 pending이어도 문맥을
     * 바꾸지 않는다. 최외곽 mutex_unlock() 뒤에 다시 평가한다.
     */
    if (current != NULL &&
        current->state == TASK_STATE_RUNNING &&
        current->held_mutex != NULL) {
        return;
    }

    const uint32_t next_slot = sched_highest_priority_slot(g_ready_bitmap);
    TCB_t *next = &g_tcbs[next_slot];

    /*
     * 현재 태스크가 계속 실행 가능한 경우에는 더 높은 우선순위가
     * 있을 때만 선점한다. 동률은 Round-Robin 없이 현재 태스크를 유지한다.
     */
    if (current != NULL && current->state == TASK_STATE_RUNNING &&
        next->priority >= current->priority) {
        return;
    }

    /*
     * 현재 태스크가 선점되는 경우 다시 READY 집합에 넣는다.
     * BLOCKED 태스크는 이미 block 경로에서 bit를 제거했으므로 복구하지
     * 않는다. 직접 매핑 규칙상 task_id가 bitmap bit 번호다.
     */
    if (current != NULL && current->state == TASK_STATE_RUNNING) {
        current->state = TASK_STATE_READY;
        g_ready_bitmap |= (1U << current->task_id);
    }

    /* 선택된 태스크의 READY bit 제거와 포인터 변경을 한 곳에서 수행한다. */
    next = sched_select_next_task();
    g_current_tcb = next;
    g_current_tcb->state = TASK_STATE_RUNNING;
}

uint32_t sched_irq_save(void) {
    /* Compatibility wrapper for existing atomic scheduler/IPC paths. */
    enter_critical();
    return 0U;
}

void sched_irq_restore(uint32_t saved_mstatus) {
    (void)saved_mstatus;
    exit_critical();
}

void sched_request_reschedule(void) {
    clint_send_software_interrupt();
}

bool sched_block_current(uint32_t saved_mstatus) {
    TCB_t *self = g_current_tcb;
    if (self == NULL || self->state != TASK_STATE_RUNNING) {
        sched_irq_restore(saved_mstatus);
        return false;
    }

    if (self->held_mutex != NULL || self->task_id == IDLE_TASK_PRIORITY) {
        kernel_panic("ERR_ILLEGAL_BLOCKING");
    }

    self->state = TASK_STATE_BLOCKED;
    uint32_t slot = (uint32_t)(self - g_tcbs);
    g_ready_bitmap &= ~(1U << slot);

    sched_request_reschedule();
    sched_irq_restore(saved_mstatus);
    return true;
}

bool sched_wake_task(uint32_t slot) {
    if (slot >= MAX_TASKS || g_tcbs[slot].state != TASK_STATE_BLOCKED) {
        return false;
    }

    g_tcbs[slot].state = TASK_STATE_READY;
    g_ready_bitmap |= (1U << slot);
    return true;
}

void task_yield(void) {
    assert_task_context();

    if (g_current_tcb == NULL ||
        g_current_tcb->task_id == IDLE_TASK_PRIORITY ||
        g_current_tcb->held_mutex != NULL) {
        kernel_panic("ERR_ILLEGAL_YIELD");
    }
    sched_request_reschedule();
}

/*
 * 현재 태스크를 ticks 동안 sleep시킨다.
 *
 * ticks가 0이면 별도 BLOCKED 상태를 만들지 않고 yield한다. 양수이면
 * sleep 카운터를 먼저 기록한 뒤 현재 태스크를 BLOCKED로 만들고 READY
 * bitmap에서 제거한다. 이후 Timer ISR이 카운터를 만료시키면 해당 태스크를
 * 다시 READY로 복귀시킨다.
 */
void task_sleep_ticks(uint32_t ticks) {
    assert_task_context();

    if (ticks == 0U) {
        task_yield();
        return;
    }

    uint32_t saved_mstatus = sched_irq_save();
    TCB_t *self = g_current_tcb;
    if (self == NULL || self->state != TASK_STATE_RUNNING) {
        sched_irq_restore(saved_mstatus);
        kernel_panic("ERR_NO_CURRENT_TASK");
    }

    if (self->held_mutex != NULL || self->task_id == IDLE_TASK_PRIORITY) {
        kernel_panic("ERR_ILLEGAL_BLOCKING");
    }

    // task_id와 TCB 배열 인덱스가 동일하므로 포인터 차이로 슬롯을 구한다.
    const uint32_t slot = (uint32_t)(self - g_tcbs);
    g_sleep_remaining_ticks[slot] = ticks;

    // 이 함수가 READY bit 제거와 MSIP 요청을 함께 처리한다.
    if (!sched_block_current(saved_mstatus)) {
        g_sleep_remaining_ticks[slot] = 0U;
    }
}
