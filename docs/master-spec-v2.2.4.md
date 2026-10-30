# PrioRTOS 아키텍처 및 상세 설계 통합 명세서 (Final Master Baseline v2.2.4-Final)

- **Project Name:** PrioRTOS (Deterministic Hard-RTOS for RISC-V Single-Core)
- **Architecture / ISA:** RV32IMC (`-march=rv32imc_zicsr -mabi=ilp32`)
- **Target Hardware / Board:** RISC-V QEMU `virt`, Hart 0 단일 코어 플랫 모델
- **Interrupt Controller:** Standard RISC-V CLINT, 10 MHz timebase
- **Document Status:** **Frozen (Zero-Collision Pure Direct Mapping & Bottom-Half Guarded)**
- **Author / Maintainer:** 임준형

## Executive Summary

PrioRTOS는 동적 메모리와 범용 네트워크 스택을 배제한 단일 코어 하드 실시간 마이크로커널이다. 목표 도메인은 차량 전장, 항공·우주 제어기 및 초정밀 산업용 드라이브다.

핵심 원칙은 다음과 같다.

- 런타임 `malloc`/`free`를 금지하고 모든 커널 상태와 스택을 정적으로 배치한다.
- `g_ready_bitmap`은 `task_id == base_priority == bit index` 직접 매핑을 사용한다.
- LSB De Bruijn 선택기는 Guard Bit 31을 연산 시점에 합성하여 빈 Ready 큐에서도 분기 없이 IDLE을 선택한다.
- Mutex 보유 태스크는 Top-Half와 Bottom-Half 모두에서 선점되지 않는다.
- Mutex 보유 중 `sleep`, semaphore wait, yield는 금지한다.
- Mutex의 유효 priority shadowing과 LIFO 복원은 최대 4단계로 제한한다.
- Timer ISR은 직접 문맥 전환하지 않고 MSIP Bottom-Half에 위임한다.
- Pending MSIP는 Bottom-Half에서 Mutex 보유 여부를 다시 검사한다.
- CrashDump를 제외한 순수 스케줄링 정적 RAM은 2,976B이며, CrashDump 24B를 포함한 전체 정적 RAM은 3,000B다.

## Part I. Software Requirements Specification

### 1. System Constraints

#### 1.1 Platform

- RV32IMC, `-march=rv32imc_zicsr -mabi=ilp32`
- Single Hart, Hart 0 only
- Little-endian
- Single M-Mode flat model
- Atomic extension `A`는 사용하지 않으며 단일 코어 임계구역은 `mstatus.MIE`로 보호한다.

#### 1.2 Physical Memory Map

| 영역 | 시작 주소 | 크기 | 속성 | 역할 |
|---|---:|---:|---|---|
| CLINT | `0x0200_0000` | 64 KiB | RW MMIO | MSIP, MTIMECMP, MTIME |
| PLIC | `0x0C00_0000` | 64 MiB | RW MMIO | 외부 인터럽트 예약 영역 |
| UART0 | `0x1000_0000` | 256 B | RW MMIO | NS16550A, 115200-8-N-1 |
| DRAM | `0x8000_0000` | 128 MiB | RWX | 코드, 데이터, BSS, 정적 스택 |

#### 1.3 CLINT and Timebase

- `CLINT_MSIP`: `0x0200_0000`
- `CLINT_MTIMECMP_LOW/HIGH`: `0x0200_4000` / `0x0200_4004`
- `CLINT_MTIME_LOW/HIGH`: `0x0200_BFF8` / `0x0200_BFFC`
- Timebase: 10 MHz
- Tick: 1 ms, `TIMER_INTERVAL = 10000ULL`
- `mtvec`: Direct Mode, `trap_entry` 주소에 바인딩
- Cause `0x80000007`: Timer Top-Half
- Cause `0x80000003`: MSIP Bottom-Half
- 기타 trap과 동기 예외: `unhandled_trap_freeze()`로 Fail-Stop

### 2. System Invariants

| ID | 불변식 | 계약 | 검증 | 위반 결과 |
|---|---|---|---|---|
| `INV-MEM-001` | Zero Dynamic Heap | 런타임 힙 금지, `.heap = 0` | map/link 검사 | 빌드 실패 |
| `INV-ALG-002` | Bounded O(1) | De Bruijn 선택은 분기 0개, sleep 순회 `N <= 31`, Mutex 순회 `N <= 4` | 역어셈블리·루프 상한 검사 | WCET 실패 |
| `INV-SCH-003` | Direct Mapping & Non-Preemption | Ready bit `p`는 `task_id == base_priority == p`; Mutex 보유 태스크는 절대 Ready 전환하지 않음 | 모든 선점 경로 테스트 | 슬롯 충돌 불가능 |
| `INV-INT-004` | Single Trap Pipeline | Timer ISR은 직접 switch하지 않고 MSIP DPC를 요청 | call graph 검사 | 시스템 스택 오염 |
| `INV-CTX-005` | Context Safety | ISR에서 task 전용 API와 `enter/exit_critical` 금지; ISR API는 trap context를 검증 | call graph, CSR 검사 | `ERR_ILLEGAL_ISR_STATE` |
| `INV-CAP-006` | Fixed 32 Slots | `OS_MAX_TASKS == 32`, ID 31은 Guard/IDLE | `_Static_assert` | 중복 등록 패닉 |
| `INV-LFC-007` | No Runtime Task Deletion | 부팅 후 생성·삭제·종료 금지 | API 및 return guard 검사 | `ERR_TASK_RETURNED` |
| `INV-STK-008` | Dual Stack & Canary | 16B canary, `stack_base + 144`, 1,024B system stack | canary/boundary 검사 | `ERR_STACK_OVERFLOW` |
| `INV-ERR-009` | Bounded Polling | UART polling은 100,000회 제한 | AST·fault injection | 문자 drop |
| `INV-MUT-010` | Bounded IPCP | ceiling shadowing, LIFO, 최대 4중첩, 경합·위반 즉시 패닉 | fault injection | Mutex panic |
| `INV-ATM-011` | Atomic Overrun-Safe Timer | High-Low-High read, safe MTIMECMP write, overrun 시 1 tick 후 `now + interval` | rollover/overrun 테스트 | IRQ storm 차단 |
| `INV-CRI-012` | Critical Nesting | nesting 0일 때만 MIE 복원 | under/overflow 검사 | critical panic |
| `INV-SYN-013` | Primitive Isolation | Mutex 보유 중 sleep/wait/yield 금지 | fault injection | `ERR_ILLEGAL_BLOCKING` |

### 3. Scheduler and Interrupt Policy

#### 3.1 Direct Mapping

태스크 생성 시 `task_id == base_priority == bit index`를 영구 고정한다. Running 태스크는 Ready bitmap에서 제거되고 `g_current_tcb`로만 관리된다. Ready 상태 태스크는 Mutex를 보유할 수 없으므로 Ready 태스크의 `priority`, `base_priority`, `task_id`는 항상 동일하다.

#### 3.2 Branchless Selector

```c
safe_bm = task_bitmap | (1U << 31);
v        = safe_bm & (0U - safe_bm);
index    = (uint32_t)(v * 0x077CB531U) >> 27;
slot     = DeBruijn_LSB_LUT[index];
```

빈 Ready 큐에서는 slot 31을 반환한다. `g_tcbs[slot]`를 직접 인덱싱하며 별도 Ready 포인터 배열을 사용하지 않는다.

#### 3.3 Non-Preemptive Mutex Section

```c
candidate = sched_highest_priority_slot(g_ready_bitmap);
if (candidate < g_current_tcb->priority &&
    g_current_tcb->held_mutex == NULL) {
    clint_send_software_interrupt();
}
```

Mutex 보유 태스크는 Timer Top-Half에서 MSIP 요청을 받지 않으며, 이미 pending된 MSIP가 Bottom-Half로 들어온 경우에도 다음 순서로 처리한다.

1. MSIP를 클리어한다.
2. 카나리를 검사한다.
3. `held_mutex != NULL`이면 switch 없이 현재 태스크로 복귀한다.
4. 최외곽 `mutex_unlock()`에서 Ready bitmap을 재평가하고 필요한 경우 MSIP를 재요청한다.

### 4. IPCP Mutex

- Mutex는 정적으로 선언한다.
- `ceiling_prio`는 0~30이다. 31은 IDLE 전용이다.
- Mutex 경합, ceiling 위반, 이중 락, 역순 unlock은 대기 없이 즉시 패닉한다.
- `OS_MAX_MUTEX_NESTING == 4U`다.
- 연결 리스트 검사는 다음과 같이 반드시 bounded해야 한다.

```c
uint32_t nest_cnt = 0U;
mutex_t *curr = g_current_tcb->held_mutex;
while (curr != NULL && nest_cnt < OS_MAX_MUTEX_NESTING) {
    ++nest_cnt;
    curr = curr->prev_mutex;
}
if (curr != NULL || nest_cnt >= OS_MAX_MUTEX_NESTING) {
    kernel_panic("ERR_IPCP_NESTING");
}
```

최외곽 Mutex를 해제한 뒤에만 선점 여부를 검사한다.

### 5. Semaphore

- `waiters`는 순수 task ID bitmap이며 Guard Bit을 주입하지 않는다.
- `semaphore_wait()`는 Task 전용이다.
- `semaphore_signal()`은 Task 전용이며 임계구역을 사용한다.
- `semaphore_signal_from_isr()`는 ISR 전용이며 `mstatus.MIE == 0`과 trap context를 검증한다.
- Mutex 보유 태스크와 IDLE은 semaphore wait를 호출할 수 없다.

### 6. Timer Overrun

정상 경로에서는 `g_last_mtimecmp += TIMER_INTERVAL`을 사용한다. `now >= g_last_mtimecmp + TIMER_INTERVAL`이면 누락된 tick을 일괄 보정하지 않고 논리 tick 하나만 처리한 뒤 `g_last_mtimecmp = now + TIMER_INTERVAL`로 재동기화한다. 64비트 unsigned modulo 산술을 사용하며 rollover 경계 테스트를 포함한다.

### 7. Task Control

`task_create()`는 priority 범위, 함수 포인터, stack 포인터, 4바이트 정렬, 144B 이상 크기, 중복 등록을 검증한다. 16B canary와 128B 초기 frame을 구성한다.

- `task_yield()`: Mutex 보유 또는 IDLE이면 패닉
- `task_sleep_ticks()`: Mutex 보유 또는 IDLE이면 패닉; 0이면 yield
- `idle_task()`: priority 31, `wfi` 무한 루프
- 태스크 반환: `task_return_guard()`에서 패닉

응용 태스크의 정상 순서는 다음과 같다.

1. Mutex lock
2. 임계 연산
3. Mutex unlock
4. `task_sleep_ticks()`

### 8. Context and Low-Level APIs

| API | 컨텍스트 | 계약 |
|---|---|---|
| `enter_critical` / `exit_critical` | Task | nesting으로 MIE를 중첩 관리 |
| `get_tick_count` | Task | 임계구역에서 64비트 tick 복사 |
| `get_tick_count_from_isr` | ISR | trap context와 `MIE == 0` 검증 후 반환 |
| `timer_tick_step` | Timer Top-Half | 직접 context switch 금지 |
| `clint_get_mtime` | Task/ISR | High-Low-High read |
| `clint_set_mtimecmp` | Task/ISR | High=max, Low, High 순서 |
| `uart_putc` | Task/Panic | 최대 100,000회 polling 후 drop |
| `uart_puts` | Task/Panic | 최대 256바이트 |
| `semaphore_signal_from_isr` | ISR | `MIE == 0` 및 trap context 검증 |

## Part II. Low-Level Design

### 9. Directory Layout

```text
PrioRTOS/
├── Makefile
├── linker.ld
├── boot/startup.S
├── app/main.c
├── app/tasks.h
├── app/tasks.c
├── bsp/clint.h
├── bsp/clint.c
├── bsp/uart.h
├── bsp/uart.c
├── kernel/context.S
├── kernel/trap.c
├── kernel/sched.c
├── kernel/mutex.c
├── kernel/semaphore.c
├── kernel/panic.c
└── kernel/include/{prio_rtos.h,kernel_internal.h,stack_guard.h}
```

### 10. Data Structures

```c
#define OS_MAX_TASKS         32U
#define OS_MAX_MUTEX_NESTING 4U

typedef enum {
    TASK_STATE_UNUSED  = 0,
    TASK_STATE_READY   = 1,
    TASK_STATE_RUNNING = 2,
    TASK_STATE_BLOCKED = 3
} TaskState_t;

typedef struct mutex {
    uint8_t ceiling_prio;
    uint8_t saved_prio;
    uint8_t locked;
    uint8_t padding;
    struct mutex *prev_mutex;
} mutex_t;

typedef struct {
    uint32_t *sp;
    mutex_t *held_mutex;
    uint32_t stack_size_bytes;
    uint32_t reserved;
    uint8_t *stack_base;
    uint8_t priority;
    uint8_t base_priority;
    uint8_t task_id;
    uint8_t state;
} TCB_t;

typedef struct {
    uint32_t count;
    uint32_t waiters;
} Semaphore_t;

typedef struct {
    uint32_t mcause;
    uint32_t mepc;
    uint32_t mtval;
    uint32_t sp;
    uint32_t task_id;
    uint32_t reserved_pad;
} CrashDump_t;

_Static_assert(sizeof(mutex_t) == 8U, "mutex_t size mismatch");
_Static_assert(sizeof(TCB_t) == 24U, "TCB_t size mismatch");
_Static_assert(sizeof(Semaphore_t) == 8U, "Semaphore_t size mismatch");
_Static_assert(sizeof(CrashDump_t) == 24U, "CrashDump_t size mismatch");
_Static_assert(OS_MAX_TASKS == 32U, "OS_MAX_TASKS must be 32");
```

### 11. Context Frame

태스크 스택은 하향 성장한다. `trap_entry`는 정확히 128B를 확보한다. 프레임에는 `x1`, `x3`~`x31`, `mepc`, `mstatus`를 저장하며 `x2(sp)`는 프레임 자체로 관리한다. 스택 하단에는 16B canary가 있고 `mscratch = stack_base + 144`를 사용한다.

### 12. Static RAM Budget

| 항목 | 크기 |
|---|---:|
| `g_tcbs[32]` | 768B |
| `g_sleep_remaining_ticks[32]` | 128B |
| 스칼라 상태 | 32B |
| CrashDump | 24B |
| 순수 제어 영역 합계 | 952B |
| `idle_stack` | 1,024B |
| `kernel_sys_stack` | 1,024B |
| CrashDump 제외 정적 RAM | 2,976B |
| CrashDump 포함 정적 RAM | 3,000B |

### 13. Boot Sequence

1. `_start`: `sp = _stack_top`
2. `.bss` zero clear
3. UART와 Direct `mtvec` 초기화
4. MSIP/MTIE와 첫 `mtimecmp` 초기화
5. 정적 태스크 및 Mutex/Semaphore 등록
6. IDLE slot 31 등록
7. 최고 우선순위 태스크를 Running으로 전환
8. `mscratch = stack_base + 144`
9. `prio_context_first_switch()`와 `mret`으로 최초 태스크 진입

### 14. Acceptance Criteria

- `sched_highest_priority_slot`: 정적 명령어 10개 이하, 조건 분기 0개
- 실제 RV32IMC 하드웨어 `mcycle`: 최대 8 cycle 목표
- Mutex 중첩 검사: 최대 4회
- 정상 Timer 10,000 tick 누적 오차: 0 cycle
- 강제 `3 * TIMER_INTERVAL` 지연: 1 tick만 처리 후 `now + TIMER_INTERVAL` 재동기화
- Kernel `.text`: 4 KiB 이하
- CrashDump 제외 정적 RAM: 2,976B
- CrashDump 포함 정적 RAM: 3,000B
- `.heap`: 0B

### 15. Traceability IDs

| 요구사항 | 대상 |
|---|---|
| `REQ-SCH-001` | De Bruijn selector / `INV-ALG-002` |
| `REQ-SCH-002` | Direct mapping and non-preemption / `INV-SCH-003` |
| `REQ-TSK-001`~`005` | Task lifecycle / `INV-CAP-006`, `INV-LFC-007` |
| `REQ-SYN-001`~`005` | Mutex and Semaphore / `INV-MUT-010`, `INV-SYN-013` |
| `REQ-CRI-001`~`002` | Critical nesting / `INV-CRI-012` |
| `REQ-TMR-001`~`002` | Tick and overrun timer / `INV-ATM-011` |
| `REQ-DRV-001`~`002` | CLINT and bounded UART / `INV-ATM-011`, `INV-ERR-009` |

## Revision History

| 버전 | 개정 내용 |
|---|---|
| v2.2.3-a | `prio_context_switch` 폐기, `trap_entry` 단일화 |
| v2.2.3-b | ISR 임계구역 API 금지 |
| v2.2.3-c | 누적 `mtimecmp`와 1 ms timer 도입 |
| v2.2.3-d | system stack, canary, `mscratch + 144` 명시 |
| v2.2.4 | BSS와 정적 RAM 산술 정리 |
| v2.2.4-Final | Direct mapping, Mutex non-preemption, Bottom-Half pending MSIP guard, bounded Mutex nesting, timer overrun policy, ISR API separation, 최종 메모리 예산 확정 |
