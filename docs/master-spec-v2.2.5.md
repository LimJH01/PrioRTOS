# PrioRTOS 아키텍처 및 상세 설계 명세서 (Baseline v2.2.5-Final)

- **Project Name:** PrioRTOS (Deterministic Hard-RTOS for RISC-V Single-Core)
- **Architecture / ISA:** RV32IMC Base Integer Instruction Set (`march=rv32imc_zicsr -mabi=ilp32`)
- **Target Hardware / Board:** RISC-V QEMU virt (Hart 0 단일 코어 플랫 모델)
- **Interrupt Controller:** Standard RISC-V CLINT (Core Local Interruptor, 10 MHz Timebase)
- **Document Status:** Frozen (Baseline v2.2.5-Final — Pure Direct Mapping, Bottom-Half Guarded & Hardware Trap Flag Enforced)
- **Author / Maintainer:** 임준형

## [EXECUTIVE SUMMARY] 타깃 도메인 및 아키텍처 사양

### 1. 목표 타깃 도메인 (Target Domains)

PrioRTOS는 동적 메모리 힙과 범용 네트워크 스택을 배제하고, ISO 26262 ASIL-D 및 DO-178C DAL-A 도메인의 제어기가 요구하는 마이크로초 단위의 실행 시간과 바운디드 지터(Bounded-Jitter)를 목표로 설계된 단일 코어 실시간 마이크로커널이다.

- 자율주행 스마트 액추에이터: Steer-by-Wire, Brake-by-Wire 인버터 모터 FOC 제어 루프.
- 항공우주 / UAM Flight Control Computer (FCC): 1 kHz~4 kHz 주기 IMU 자세 제어(EKF), 모터 믹서 연산.
- 산업용 드라이브: 지터 10 µs 이하를 요구하는 서보 드라이브 및 제어 루프.

### 2. 핵심 설계 특성

핵심 원칙은 다음과 같다.

- **정적 메모리 할당 (Zero Dynamic Heap):** 런타임 힙 할당을 배제하고 컴파일 타임에 모든 메모리를 정적으로 배치한다 (INV-MEM-001).
- `g_ready_bitmap`은 `task_id == base_priority == bit index` 직접 매핑을 사용한다.
- LSB De Bruijn 선택기는 Guard Bit 31을 연산 시점에 합성하여 빈 Ready 큐에서도 분기 없이 IDLE을 선택한다.
- Mutex 보유 태스크는 Top-Half와 Bottom-Half 모두에서 선점되지 않는다.
- Mutex 보유 중 `sleep`, semaphore wait, yield는 금지한다.
- Mutex의 유효 priority shadowing과 LIFO 복원은 최대 4단계로 제한한다.
- Timer ISR은 직접 문맥 전환하지 않고 MSIP Bottom-Half에 위임한다.
- Pending MSIP는 Bottom-Half에서 Mutex 보유 여부를 다시 검사한다.
- `g_in_trap_context`는 C 트랩 핸들러 실행 중임을 표시하여 `mstatus.MIE == 0`과 태스크 임계구역을 구분한다.
- **트랩 컨텍스트 검증 플래그(`g_in_trap_context`):** `mstatus.MIE == 0` 상태 외에 트랩 진입/복귀 시 갱신되는 전역 플래그를 두어 태스크 크리티컬 섹션과 실제 ISR 실행 상태를 분리 검증한다 (INV-CTX-005).
- **메모리 풋프린트:** 커널 바이너리(`.text`) 4 KiB 이내, 순수 커널 스칼라 상태 변수 36 Bytes, 제어 구조체 및 배열 총합 920 Bytes, 시스템 정적 RAM 소모 총합 3,004 Bytes(32개 TCB 768B, 슬립 배열 128B, CrashDump 24B, 스칼라 36B, IDLE 스택 1024B, 시스템 스택 1024B 전체 포함).

### 3. v2.1 대비 변경 사항

- **간접 매핑 테이블(`g_prio_to_slot[32]`) 제거:** 우선순위 간접 역참조 배열을 제거하고, `base_priority == task_id == Bit Index`의 1:1 직접 매핑 체계를 적용한다.
- **Bottom-Half 비선점 IPCP 가드 및 LIFO Priority Shadowing:** 뮤텍스 획득 직전에 펜딩된 지연 MSIP가 `exit_critical()` 직후 처리되더라도 Bottom-Half(`trap_handler` Cause 3)에서 `held_mutex != NULL`을 확인하여 문맥 교환을 취소한다.
- **Guard Bit 주입(Sentinel Bit Masking):** `g_ready_bitmap`은 대기 중인(`TASK_STATE_READY`) 태스크만 추적하며, `safe_bm = task_bitmap | (1U << 31)`으로 IDLE 폴백을 처리한다.
- **단일 트랩 에필로그 문맥 교환:** `prio_context_switch`를 폐기하고 `trap_entry` 단일 루틴에서 문맥 저장·복원과 시스템 스택 전환을 수행한다.
- **트랩 컨텍스트 검증 플래그 통합:** `g_in_trap_context`를 BSS에 상주시키고 API 진입점에서 컨텍스트 유효성을 검증한다.

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
| `INV-CTX-005` | Context Safety & Trap Flag | ISR에서 task 전용 API와 `enter/exit_critical` 금지; ISR API는 `mstatus.MIE == 0 && g_in_trap_context == 1`을 검증하고 task API는 flag가 0인지 검증 | call graph, CSR·flag 검사 | `ERR_ILLEGAL_ISR_STATE` |
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

#### 3.4 Trap Context Flag

RISC-V M-mode에는 ARM의 실행 모드 비트처럼 현재 코드가 태스크 본문인지 ISR인지 직접 알려주는 일반적인 소프트웨어 판별 비트가 없다. 트랩 진입 직후에는 하드웨어가 `mstatus.MIE`를 0으로 만들지만, 태스크가 `enter_critical()`을 실행한 동안에도 같은 값이므로 `MIE`만으로는 두 상태를 구분할 수 없다.

따라서 단일 Hart의 단일 `trap_entry -> trap_handler` 파이프라인에 대해 다음의 소프트웨어 표지를 둔다.

- `volatile uint32_t g_in_trap_context`는 `.bss`에 배치하고 초기값은 0이다.
- `trap_handler()`의 첫 명령군에서 1로 설정한다.
- Cause 3/7의 정상 반환 직전에 0으로 복원한다. `unhandled_trap_freeze()`와 `kernel_panic()`처럼 복귀하지 않는 경로는 별도 clear를 요구하지 않는다.
- ISR 전용 API는 `mstatus.MIE == 0 && g_in_trap_context == 1`을 모두 만족해야 한다.
- 태스크 전용 API는 `g_in_trap_context == 0`을 요구한다. 이 검사는 MIE를 다시 켜는 동작이 아니며, 태스크의 임계구역 상태와 독립적이다.
- 이 플래그는 하드웨어 트랩 원인 자체를 인증하지 않는다. `mtvec`가 다른 진입점으로 호출되거나 메모리 손상이 발생하는 경우까지 방어하는 보안 경계가 아니며, 정해진 trap pipeline의 문맥 계약을 검증하는 진단·안전 장치다.

Bottom-Half의 Mutex 보유 조기 복귀 경로를 포함하여 모든 복귀 경로는 flag를 0으로 만들고, flag가 1인 동안에는 task-only API를 호출하지 않는다.

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

`assert_isr_context()`는 `((mstatus & 0x8U) == 0U) && (g_in_trap_context == 1U)`를 검사하고, `assert_task_context()`는 `g_in_trap_context == 0U`를 검사한다. `g_in_trap_context`는 인터럽트 enable 상태를 제어하지 않으며, 단지 현재 C 코드 실행이 트랩 파이프라인 안에 있음을 나타낸다.

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

/* 0: task context, 1: trap_handler() 실행 중 */
extern volatile uint32_t g_in_trap_context;

static inline void assert_isr_context(void) {
    uint32_t mstatus_value;
    __asm__ volatile("csrr %0, mstatus" : "=r"(mstatus_value));
    if ((mstatus_value & 0x8U) != 0U || g_in_trap_context != 1U) {
        kernel_panic("ERR_ILLEGAL_ISR_STATE");
    }
}

static inline void assert_task_context(void) {
    if (g_in_trap_context != 0U) {
        kernel_panic("ERR_ILLEGAL_ISR_STATE");
    }
}
```

### 11. Context Frame

태스크 스택은 하향 성장한다. `trap_entry`는 정확히 128B를 확보한다. 프레임에는 `x1`, `x3`~`x31`, `mepc`, `mstatus`를 저장하며 `x2(sp)`는 프레임 자체로 관리한다. 스택 하단에는 16B canary가 있고 `mscratch = stack_base + 144`를 사용한다.

### 12. Static RAM Budget

| 항목 | 크기 |
|---|---:|
| `g_tcbs[32]` | 768B |
| `g_sleep_remaining_ticks[32]` | 128B |
| 스칼라 상태 (`g_in_trap_context` 포함) | 36B |
| CrashDump | 24B |
| 순수 제어 영역 합계 | 956B |
| `idle_stack` | 1,024B |
| `kernel_sys_stack` | 1,024B |
| CrashDump 제외 정적 RAM | 2,980B |
| CrashDump 포함 정적 RAM | 3,004B |

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

`.bss` 초기화 이후 `g_in_trap_context`는 0이어야 한다. `trap_handler()`는 Cause 판별 전에 flag를 1로 만들고, Cause 3/7의 모든 반환 경로에서 0으로 되돌린다.

### 14. Acceptance Criteria

- `sched_highest_priority_slot`: 정적 명령어 10개 이하, 조건 분기 0개
- 실제 RV32IMC 하드웨어 `mcycle`: 최대 8 cycle 목표
- Mutex 중첩 검사: 최대 4회
- 정상 Timer 10,000 tick 누적 오차: 0 cycle
- 강제 `3 * TIMER_INTERVAL` 지연: 1 tick만 처리 후 `now + TIMER_INTERVAL` 재동기화
- Task 문맥에서 `g_in_trap_context == 0`, trap C handler 문맥에서 `g_in_trap_context == 1`
- Task critical section(`MIE == 0`, flag 0)과 ISR(`MIE == 0`, flag 1)을 구별하는 결함 주입 테스트 통과
- Kernel `.text`: 4 KiB 이하
- CrashDump 제외 정적 RAM: 2,980B
- CrashDump 포함 정적 RAM: 3,004B
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
| v2.2.5-Final | `g_in_trap_context` 도입; `MIE == 0`만으로 구분할 수 없는 task critical section과 ISR을 분리 검증; `assert_isr_context()`/`assert_task_context()` 계약, trap 진입·복귀 토글, BSS 및 RAM 예산 4B 반영 |
