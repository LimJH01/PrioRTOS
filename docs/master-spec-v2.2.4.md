# PrioRTOS 아키텍처 및 상세 설계 통합 명세서 (Final Master Baseline v2.2.4)

- **Project Name:** PrioRTOS (Deterministic Hard-RTOS for RISC-V Single-Core)
- **Architecture / ISA:** RV32IMC Base Integer Instruction Set (`march=rv32imc_zicsr -mabi=ilp32`)
- **Target Hardware / Board:** Primary: RISC-V QEMU `virt` / Secondary: ESP32-C3 (BSP Abstraction & IRAM Pinning)
- **Interrupt Model:** Standard RISC-V CLINT (Core Local Interruptor)
- **Document Status:** Frozen (Baseline v2.2.4 - Pure Direct-Mapped LSB De Bruijn with Upper Guard Bit Insertion & Unified Assembly Trap Pipeline Architecture)
- **Author / Maintainer:** 임준형

## [EXECUTIVE SUMMARY] 타깃 도메인, 핵심 셀링 포인트 및 아키텍처 차별점

### 1. 목표 타깃 도메인

PrioRTOS는 범용 OS가 아닌 ISO 26262 ASIL-D 및 DO-178C DAL-A 도메인의 로컬 안전 제어기를 위한 고신뢰성 초경량 실시간 마이크로커널이다.

- **자율주행 및 x-by-Wire:** Steer-by-Wire, Brake-by-Wire, 인버터 모터 FOC, 라이다/레이다 제어
- **UAM 및 항공우주 FCC:** 1kHz~4kHz IMU Attitude EKF, 모터 믹싱, 페일세이프
- **스마트 팩토리:** 마이크로초 동기화 서보 드라이브 및 EtherCAT 슬레이브
- **방위산업:** 고속 롤/피치 핀 제어기 및 Fail-Stop 안전 통제

### 2. 핵심 셀링 포인트

- **Zero Dynamic Heap:** 런타임 동적 할당을 금지하여 단편화와 OOM을 차단한다 (`INV-MEM-001`).
- **증명 가능한 WCET:** Guard Bit LSB De Bruijn 스케줄러는 분기 없이 6~8 사이클, `O(1)`로 동작한다.
- **IPCP Priority Shadowing:** 락 획득 시 비트맵을 조작하지 않고 TCB 유효 우선순위를 천장값으로 승격한다.
- **데드락·우선순위 역전 억제:** Baker SRP 기반으로 최악 블로킹을 최장 임계 구역 1개로 제한한다.
- **단일 어셈블리 트랩 파이프라인:** 태스크 스택과 `kernel_sys_stack`(1024B)을 분리한다.
- **메모리 풋프린트:** `.text` 4 KiB 이내, 스칼라 32B, 제어 구조체 928B, 정적 RAM 2,976B를 목표로 한다.

### 3. v2.1 대비 v2.2.4 핵심 혁신

- `g_prio_to_slot[32]` 및 동적 뮤텍스 풀(`kernel/ipcp.c`)을 폐기하고 `Priority == Slot == Bit Index` 직접 매핑을 적용한다.
- 뮤텍스는 TCB 우선순위 LIFO 섀도잉을 사용하고 `locked` 플래그로 이중 락을 방어한다.
- 스케줄러 평가 시 `safe_bm = task_bitmap | (1U << 31)`을 합성하여 빈 Ready 큐에서 IDLE을 선택한다.
- C 함수 `prio_context_switch`를 폐기하고 `trap_entry` 어셈블리 에필로그로 문맥 교환을 일원화한다.

## PART I. 소프트웨어 요구사항 명세서 (SRS)

### 1. 시스템 환경 및 하드웨어 제약

#### 1.1. QEMU `virt` 기준

- RV32IMC single Hart, Hart 0 고정, Little-Endian, 단일 M-Mode flat model
- ISA: `march=rv32imc_zicsr -mabi=ilp32`

| 영역 | 시작 주소 | 크기 | 속성 | 역할 |
| --- | ---: | ---: | --- | --- |
| CLINT | `0x0200_0000` | 64 KB | RW MMIO | 소프트웨어·타이머 인터럽트 |
| PLIC | `0x0C00_0000` | 64 MB | RW MMIO | 외부 인터럽트 |
| UART0 | `0x1000_0000` | 256 B | RW MMIO | NS16550A 디버그 UART |
| DRAM | `0x8000_0000` | 128 MB | RWX | 전체 코드·데이터·스택 |

#### 1.2. CLINT 및 인터럽트

- `MSIP`: `0x0200_0000`
- `MTIMECMP_LOW/HIGH`: `0x0200_4000` / `0x0200_4004`
- `MTIME_LOW/HIGH`: `0x0200_BFF8` / `0x0200_BFFC`
- 타임베이스 10 MHz, 1 ms = 10,000 cycles

```c
#define TIMER_INTERVAL 10000ULL
```

`mtvec`는 Direct Mode로 `trap_entry`에 바인딩한다. Cause `0x80000007`은 `timer_tick_step`, Cause `0x80000003`은 MSIP DPC로 디스패치하며, 나머지는 `unhandled_trap_freeze()`로 Fail-Stop 동결한다.

### 2. 시스템 아키텍처 불변 원칙

| ID | 카테고리 | 불변식 | 핵심 제약 | 검증 |
| --- | --- | --- | --- | --- |
| `INV-MEM-001` | 메모리 | Zero Dynamic Heap | 런타임 힙 금지, 정적 배치 | `.heap=0`, map 검사 |
| `INV-ALG-002` | 복잡도 | Bounded O(1) | Branchless De Bruijn, 슬립 `N <= 32` | 역어셈블리 검사 |
| `INV-INT-003` | 인터럽트 | Single-Pipeline Trap & DPC | Timer ISR 직접 교환 금지 | Call-Graph 검사 |
| `INV-TIM-004` | 타이밍 | Context-Aware Critical Section | ISR 임계 구역 API 금지 | CSR 검사 |
| `INV-CAP-005` | 용량 | Fixed 32-Slot Bounding | `OS_MAX_TASKS == 32` | `_Static_assert` |
| `INV-LFC-006` | 수명주기 | No Runtime Task Deletion | 생성·종료·삭제 금지 | return guard |
| `INV-STK-007` | 스택 | Dual Stack & Hardware Boundary | 카나리, 144B `mscratch`, 시스템 스택 1024B | 경계 검사 |
| `INV-ERR-008` | 오류 | Bounded HW Polling | 고정 timeout | AST 분석·결함 주입 |
| `INV-MUT-009` | 동기화 | Deterministic IPCP Shadowing | 비트맵 없는 천장 승격, 이중 락·역순 해제 패닉 | 단위 테스트 |
| `INV-ATM-010` | 드라이버 | CLINT Atomic & Drift-Free | 누적 `mtimecmp`, 원자적 접근 | 명령 시퀀스 검사 |
| `INV-CRI-011` | 동시성 | Critical Section Nesting | nesting 0일 때만 MIE 복원 | 언더·오버플로 검사 |
| `INV-SYN-012` | 동기화 | Primitive Isolation | 락 보유 중 sleep/wait/yield 금지 | 결함 주입 |

### 3. 커널 서브시스템 기능 요구사항

#### 3.1. 스케줄러 및 인터럽트

우선순위 0~31의 고정 우선순위 선점형 스케줄러를 사용한다. Running 태스크는 `g_ready_bitmap`에서 제거하고 `g_current_tcb`로만 관리한다. `g_ready_bitmap`은 Ready 태스크만 추적하며 0일 수 있다.

```c
safe_bm = task_bitmap | (1U << 31);
v        = safe_bm & (-safe_bm);
index    = (uint32_t)(v * 0x077CB531U) >> 27;
slot     = DeBruijn_LSB_LUT[index];
```

원본 비트맵은 변경하지 않으며, 응용 Ready 태스크가 없을 때만 Guard Bit으로 31번 IDLE을 선택한다. Timer Top-Half는 누적 `mtimecmp` 갱신, 시스템 틱 증가, 최대 32개 슬립 슬롯 감산·만료 복귀, 선점 판정을 수행한다. Timer ISR에서는 UART 및 `enter/exit_critical`를 호출하지 않는다.

MSIP Bottom-Half는 현재 태스크의 128B 프레임을 보존하고 `kernel_sys_stack`으로 전환한다. MSIP 클리어, 카나리 검사, 기존 Running 태스크의 Ready 복귀, 차기 TCB 선택을 수행한 후 어셈블리 에필로그가 차기 SP를 로드하고 `mret`한다.

#### 3.2. 우선순위 영역

| 범위 | 도메인 | 정책 및 예시 |
| --- | --- | --- |
| 0~3 | Safety & Fault Handling | 블록 금지, WCET 50 µs 이하, E-Stop·페일세이프 |
| 4~16 | Hard Real-Time Control | 1~10 ms, 지터 10 µs 이하, FOC·센서 융합 |
| 17~27 | Soft Real-Time & Comm | 10~100 ms, 세마포어·링 버퍼·UART/CAN |
| 28~30 | Background & Health | 500 ms 이상, 진단·카나리 검사 |
| 31 | System IDLE | `wfi` 무한 루프, 동기화 API 금지 |

#### 3.3. 부팅 및 예외

1. `_start`에서 `_stack_top`으로 SP 설정 및 `.bss` 클리어
2. UART와 Direct `mtvec` 초기화
3. CLINT MSIE/MTIE 활성화 및 `g_last_mtimecmp = clint_get_mtime() + 10000`
4. `tasks_init()`으로 정적 태스크 등록
5. `priortos_start()`에서 슬롯 31 IDLE 등록 및 최고 우선순위 선택
6. `stack_base + 144`를 `mscratch`에 기록하고 `prio_context_first_switch()` 실행

미인가 트랩은 `unhandled_trap_freeze()`로, 명시적 오류는 MIE 차단·UART 출력·`wfi` 루프의 `kernel_panic()`으로 처리한다.

#### 3.4. 태스크 제어

`task_create()`는 범위, 포인터, 144바이트 이상 크기, 4바이트 정렬 및 중복 등록을 검증한다. 하단 16바이트에 `0x5A5A5A5A` 카나리를 기록하고 상단 128바이트에 `mstatus=0x1880`, `mepc=task_func`, `ra=task_return_guard`를 배치한다.

- `task_yield()`: 락 보유 시 패닉 후 MSIP 예약
- `task_sleep_ticks()`: 0이면 yield, 락 보유 또는 IDLE 호출은 패닉
- `idle_task()`: 슬롯 31에서 `while(1) { __asm__ volatile("wfi"); }`

#### 3.5. IPCP 및 세마포어

`mutex_lock()`은 천장 위반·이중 락을 검증하고 `saved_prio` 및 `prev_mutex`를 LIFO로 저장한 후 TCB 우선순위를 승격한다. `mutex_unlock()`은 최상단 뮤텍스 일치와 `locked`를 검증하고 우선순위를 복원한다.

세마포어는 `waiters` 비트맵을 사용한다. 대기자가 없으면 `count++`하고, 있으면 Guard Bit을 넣지 않은 `v = waiters & (-waiters)`로 최고 우선순위 대기자를 깨운다. 락 보유 태스크와 IDLE은 sleep, semaphore wait, yield를 호출할 수 없다.

#### 3.6. 임계 구역 및 시간

`enter_critical()`은 `csrci mstatus, 8` 후 nesting을 증가시키고, `exit_critical()`은 nesting이 0일 때 배리어 후 `csrsi mstatus, 8`을 실행한다. ISR에서는 두 API를 호출하지 않는다. `mtime`은 High-Low-High로 읽고 `mtimecmp`는 `HIGH=0xFFFFFFFF` → `LOW=low` → `HIGH=high` 순서로 쓴다.

## PART II. 상세 설계 명세서 (LLD)

### 4. 디렉터리 및 파일 아키텍처

```text
PrioRTOS/
├── Makefile
├── README.md
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
├── kernel/include/prio_rtos.h
├── kernel/include/kernel_internal.h
├── kernel/include/stack_guard.h
└── build/
```

> `5. 모듈별 함수 원형과 구현 명세`는 요청에 따라 제외했다.

### 6. 정밀 메모리 레이아웃 및 자료구조 명세

#### 6.1. 128바이트 문맥 보존 프레임

```text
[High Address]
  sp + 0x7C : mstatus (0x0000_1880)
  sp + 0x78 : mepc (task_func)
  sp + 0x74 : x31 (t6)
  sp + 0x70 : x30 (t5)
  ...
  sp + 0x20 : x10 (a0)
  sp + 0x1C : x9 (s1)
  sp + 0x18 : x8 (s0/fp)
  sp + 0x14 : x7 (t2)
  sp + 0x10 : x6 (t1)
  sp + 0x0C : x5 (t0)
  sp + 0x08 : x4 (tp)
  sp + 0x04 : x3 (gp)
  sp + 0x00 : x1 (ra = task_return_guard)
[Current SP: 총 32 Words = 128 Bytes]
[stack_base + 144B: mscratch 경계]
[stack_base: 0x5A5A5A5A × 4 카나리]
[Low Address]
```

#### 6.2. 핵심 자료구조

```c
#define OS_MAX_TASKS 32U

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

_Static_assert(OS_MAX_TASKS == 32U, "OS_MAX_TASKS must be exactly 32");
_Static_assert(sizeof(mutex_t) == 8, "mutex_t size mismatch");
_Static_assert(sizeof(TCB_t) == 24, "TCB_t size mismatch");
_Static_assert(offsetof(TCB_t, sp) == 0, "TCB sp offset mismatch");
_Static_assert(offsetof(TCB_t, stack_base) == 16, "TCB stack_base offset mismatch");
_Static_assert(sizeof(Semaphore_t) == 8, "Semaphore_t size mismatch");
```

#### 6.3. 커널 전역 정적 변수 레지스트리

| 변수 | 자료형 | 크기 | 정의·역할 |
| --- | --- | ---: | --- |
| `g_tcbs[32]` | `TCB_t[]` | 768 B | 슬롯·우선순위 직접 매핑 TCB |
| `g_sleep_remaining_ticks[32]` | `volatile uint32_t[]` | 128 B | 잔여 슬립 틱 |
| `g_ready_bitmap` / `g_sleep_bitmap` | `volatile uint32_t` | 4 B씩 | Ready/Sleep 비트맵 |
| `g_system_ticks` / `g_last_mtimecmp` | `volatile uint64_t` | 8 B씩 | 시스템 틱/누적 비교값 |
| `g_current_tcb` | `TCB_t *` | 4 B | 현재 TCB |
| `g_critical_nesting_count` | `volatile uint32_t` | 4 B | nesting 카운터 |
| `idle_stack[1024]` | `uint8_t[]` | 1,024 B | IDLE 스택 |
| `kernel_sys_stack[1024]` | `uint8_t[]` | 1,024 B | 부트·C 트랩 시스템 스택 |

순수 스칼라 상태 변수는 32B, 제어 구조체는 928B, 두 스택을 포함한 정적 RAM은 2,976B이다. `g_prio_to_slot[32]`와 동적 뮤텍스 풀은 없다.

#### 6.4. 비트맵 운용 및 생명주기

1. 부팅: `g_ready_bitmap = 0x0000_0000`
2. 태스크 4·5 등록: `0x0000_0030`
3. IDLE 31 등록: `0x8000_0030`
4. 태스크 4 Running: `0x8000_0020`
5. 모든 태스크가 Running/Blocked이면 `0x0000_0000` 허용
6. 평가 시 Guard Bit으로 31번 IDLE을 안전하게 선택
7. Cause 3으로 선점된 Running 태스크는 Ready 비트맵으로 복귀

## Revision History

| 버전 | 개정 일자 | 주요 개정 |
| --- | --- | --- |
| v2.2.3 | 2026-10-30 | 단일 `trap_entry` 에필로그, Ready 복귀, ISR 임계 구역 금지, 누적 `mtimecmp`, 순수 세마포어 LSB, 시스템 스택 명시 |
| v2.2.4 | 2026-10-30 | 스칼라 32B, 제어 구조체 928B, 정적 RAM 2,976B 정합성, `N <= 32` WCET, Delta Queue 로드맵, 타이머 롤오버 불변식 |
