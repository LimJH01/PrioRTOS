# PrioRTOS 아키텍처 및 상세 설계 통합 명세서

## Final Master Baseline v2.2.4

- **Project Name:** PrioRTOS (Deterministic Hard-RTOS for RISC-V Single-Core)
- **Architecture / ISA:** RV32IMC (`march=rv32imc_zicsr -mabi=ilp32`)
- **Target Hardware / Board:** Primary: RISC-V QEMU `virt`; Secondary: ESP32-C3 (BSP abstraction 및 IRAM pinning)
- **Interrupt Model:** Standard RISC-V CLINT (Core Local Interruptor)
- **Document Status:** Frozen — Pure Direct-Mapped LSB De Bruijn with Upper Guard Bit Insertion & Unified Assembly Trap Pipeline Architecture
- **Author / Maintainer:** 임준형

---

# [EXECUTIVE SUMMARY] 타깃 도메인, 핵심 셀링 포인트 및 아키텍처 차별점

## 1. 목표 타깃 도메인

PrioRTOS는 파일 시스템이나 범용 동적 네트워크 스택을 제공하는 범용 OS가 아니라, ISO 26262 ASIL-D 및 DO-178C DAL-A 도메인의 로컬 안전 제어기에 필요한 엄격한 타이밍 결정론과 무지터 실행을 목표로 하는 초경량 실시간 마이크로커널이다.

- 자율주행 및 x-by-Wire 스마트 액추에이터: Steer-by-Wire, Brake-by-Wire, FOC, 라이다/레이다 제어
- UAM 및 항공우주 FCC: 1~4 kHz IMU attitude EKF, 모터 믹싱, 페일세이프 제어
- 스마트 팩토리 액추에이터: 마이크로초 동기화 서보 드라이브 및 EtherCAT 슬레이브 루프
- 방위산업 유도 조종 장치: 고속 롤/피치 핀 제어기 및 Fail-Stop 안전 통제

## 2. 핵심 셀링 포인트

- **Zero Dynamic Heap:** 모든 메모리를 정적으로 배치하여 단편화와 OOM을 제거한다 (`INV-MEM-001`).
- **증명 가능한 WCET:** Guard Bit을 합성한 LSB De Bruijn 스케줄러는 분기 없이 `O(1)`로 동작한다.
- **IPCP Priority Shadowing:** 뮤텍스 획득 시 비트맵을 조작하지 않고 TCB의 유효 우선순위만 천장값으로 승격한다.
- **데드락 및 우선순위 역전 억제:** Baker SRP 기반의 천장 정책과 LIFO 락 해제로 블로킹 상한을 제한한다.
- **단일 어셈블리 트랩 파이프라인:** 태스크 스택과 1,024바이트 커널 시스템 스택을 분리하고 문맥 교환을 `trap_entry` 에필로그로 일원화한다.
- **고정 메모리 풋프린트:** 32개 TCB, 슬립 배열, IDLE 스택 및 시스템 스택을 포함한 정적 RAM 목표는 2,976바이트이다.

## 3. v2.1 대비 v2.2.4 핵심 변경

1. `g_prio_to_slot[32]` 및 동적 뮤텍스 풀을 폐기하고 `Priority == Slot == Bit Index` 직접 매핑으로 통일했다.
2. Running 태스크를 `g_ready_bitmap` 밖에서 관리하고, IPCP 뮤텍스는 TCB의 LIFO 체인으로 우선순위를 섀도잉한다.
3. 스케줄러가 평가하는 순간 `safe_bm = task_bitmap | (1U << 31)`을 합성하여 빈 Ready 큐도 분기 없이 IDLE 슬롯으로 처리한다.
4. C 함수 `prio_context_switch`를 폐기하고, `trap_entry` 어셈블리 루틴이 SP 저장·시스템 스택 전환·C 디스패처 호출·차기 SP 복원·레지스터 복원을 단일 파이프라인으로 수행한다.

---

# PART I. 소프트웨어 요구사항 명세서 (SRS)

## 1. 시스템 환경 및 하드웨어 제약

### 1.1 QEMU `virt` 기준 하드웨어

- RV32 single Hart, Hart 0 고정, Little-Endian, 단일 M-Mode flat model
- ISA: `RV32IMC`, CSR 접근을 위해 `zicsr` 사용
- CLINT: `0x0200_0000`, 64 KiB
- PLIC: `0x0C00_0000`, 64 MiB
- UART0: `0x1000_0000`, 256 B, NS16550A 호환
- DRAM: `0x8000_0000`, 128 MiB, 커널·애플리케이션·정적 스택 배치

### 1.2 CLINT 및 타이머 규격

| 레지스터 | 주소 | 용도 |
| --- | ---: | --- |
| `MSIP0` | `0x0200_0000` | Cause 3 소프트웨어 인터럽트 |
| `MTIMECMP_LOW/HIGH` | `0x0200_4000/0x0200_4004` | 타이머 비교값 |
| `MTIME_LOW/HIGH` | `0x0200_BFF8/0x0200_BFFC` | 64비트 실시간 타이머 |

타임베이스는 10 MHz이며, 1 ms 틱은 10,000 cycles이다.

```c
#define TIMER_INTERVAL 10000ULL
```

`mtvec`는 Direct Mode로 설정한다. 모든 트랩은 4바이트 정렬된 `trap_entry`로 진입하며, `mcause`가 `0x80000007`이면 타이머 Top-Half, `0x80000003`이면 MSIP Bottom-Half/DPC로 디스패치한다. 그 밖의 트랩과 동기 예외는 진단 출력 없이 `wfi` Fail-Stop 루프로 동결한다.

## 2. 시스템 불변식

| ID | 불변식 | 핵심 제약 | 검증 수단 |
| --- | --- | --- | --- |
| `INV-MEM-001` | Zero Dynamic Heap | 런타임 동적 할당 금지, 정적 파티셔닝 | 링커 `.heap=0`, map 심볼 검사 |
| `INV-ALG-002` | Bounded `O(1)` Complexity | De Bruijn 스케줄러는 분기 없이 상수 시간, 슬립 순회는 최대 32개 | 역어셈블리 분기·점프 검사 |
| `INV-INT-003` | Single-Pipeline Trap & DPC | 타이머 ISR에서 문맥 교환 금지, MSIP로 DPC 위임 | call graph 및 `context.S` 검사 |
| `INV-TIM-004` | Context-Aware Critical Section | ISR에서 `enter/exit_critical` 호출 금지 | ISR CSR 접근 검사 |
| `INV-CAP-005` | Fixed 32-Slot Bounding | `OS_MAX_TASKS == 32`, 1:1 슬롯 매핑 | `_Static_assert`, 중복 등록 검출 |
| `INV-LFC-006` | No Runtime Task Deletion | 시동 후 생성·종료·삭제 금지 | 공개 삭제 API 제거, return guard |
| `INV-STK-007` | Dual Stack & Hardware Boundary | 16바이트 카나리, 144바이트 `mscratch` 경계, 커널 스택 1,024B | 카나리 및 경계 검사 |
| `INV-ERR-008` | Bounded HW Polling | 모든 하드웨어 폴링에 고정 상한 | 루프 정적 분석 및 결함 주입 |
| `INV-MUT-009` | Deterministic IPCP Shadowing | 비트맵 조작 없는 천장 승격, 이중 락·비순차 해제 패닉 | 결함 주입 및 cycle 측정 |
| `INV-ATM-010` | CLINT 64-bit Atomic & Drift-Free | 누적 `mtimecmp`, 원자적 64비트 접근 | 명령 시퀀스 및 롤오버 검사 |
| `INV-CRI-011` | Critical Section Nesting | 카운터가 0일 때만 `MIE` 복원 | 언더/오버플로 검사 |
| `INV-SYN-012` | Primitive Isolation | 락 보유 태스크의 sleep/wait/yield 금지 | 동기화 결함 주입 테스트 |

## 3. 스케줄러 및 인터럽트 정책

### 3.1 직접 매핑 및 Running/Ready 분리

우선순위 0이 최고, 31이 최저이며, `1 Priority = 1 Task = 1 Slot`이다. Running 태스크는 반드시 Ready 비트맵에서 제거하고 `g_current_tcb`로만 관리한다. `g_ready_bitmap`은 CPU를 기다리는 태스크만 표시하며, 아무도 Ready가 아니면 `0x00000000`을 허용한다.

### 3.2 Guard Bit LSB De Bruijn 스케줄러

스케줄러는 다음의 고정 시퀀스를 사용한다.

```c
safe_bm = task_bitmap | (1U << 31);
v        = safe_bm & (-safe_bm);
index    = (uint32_t)(v * 0x077CB531U) >> 27;
slot     = DeBruijn_LSB_LUT[index];
```

32바이트 LUT와 승수 `0x077CB531U`를 사용한다. 응용 태스크가 있으면 가장 작은 슬롯이 선택되고, 없으면 합성된 31번 Guard Bit이 IDLE을 선택한다. 원본 `g_ready_bitmap`은 변경하지 않는다.

### 3.3 Timer Top-Half

`timer_tick_step()`은 다음을 수행한다.

1. `g_last_mtimecmp += TIMER_INTERVAL` 후 원자적으로 `mtimecmp`를 재장전한다.
2. `g_system_ticks`를 증가시킨다.
3. 최대 32개 슬립 슬롯의 잔여 틱을 감산한다.
4. 만료 시 `g_sleep_bitmap`에서 슬롯을 제거하고 같은 원자 구간에서 Ready로 복귀시킨다.
5. 더 높은 우선순위 태스크가 Ready이면 MSIP를 기록하여 DPC를 예약한다.

ISR 내부에서는 UART 출력과 `enter_critical`/`exit_critical` 호출을 금지한다. 메모리 배리어만 사용한다.

### 3.4 MSIP Bottom-Half / DPC

`trap_entry`는 현재 태스크 스택에 128바이트 프레임을 저장하고 `kernel_sys_stack`으로 전환하여 `trap_handler`를 호출한다. 핸들러는 MSIP를 클리어하고 카나리를 검증한다. 기존 태스크가 Running이면 Ready로 복귀시킨 후, `sched_select_next_task()`로 다음 TCB를 선택하고 상태·비트맵·`g_current_tcb`를 갱신한다. 반환 시 어셈블리 에필로그가 차기 TCB의 SP를 로드하고 128바이트를 복원한 뒤 `mret`한다.

## 4. 우선순위 영역

| 우선순위 | 도메인 | 정책 |
| --- | --- | --- |
| 0~3 | Safety & Fault Handling | 이벤트성, 블록 금지, WCET 50 µs 이하 목표 |
| 4~16 | Hard Real-Time Control | 1~10 ms 주기, IPCP 공유 가능, 지터 10 µs 이하 목표 |
| 17~27 | Soft Real-Time & Communication | 10~100 ms, 세마포어 대기 및 링 버퍼 처리 |
| 28~30 | Background & Health | 500 ms 이상, 언제든 선점 가능 |
| 31 | System IDLE | `wfi` 무한 루프, 모든 동기화 API 금지 |

## 5. 부팅 및 예외 처리

1. `_start`에서 `_stack_top`으로 SP를 설정하고 `.bss`를 0으로 초기화한다.
2. UART와 Direct `mtvec`를 초기화한다.
3. CLINT MSIE/MTIE를 활성화하고 `g_last_mtimecmp = clint_get_mtime() + 10000`을 장전한다.
4. `tasks_init()`으로 정적 태스크를 등록한다.
5. `priortos_start()`에서 슬롯 31 IDLE을 등록하고 첫 TCB를 선택한다.
6. `mscratch`에 `stack_base + 144`를 기록하고 `prio_context_first_switch()`로 `mret` 진입한다.

미인가 트랩은 `unhandled_trap_freeze()`로, 명시적 커널 오류는 `kernel_panic()`으로 진입한다. 패닉 루틴은 `MIE`를 차단하고 bounded UART 출력 후 `wfi` 무한 루프에 머문다.

## 6. 태스크 제어 요구사항

`task_create()`는 `prio < 32`, 함수·스택 비NULL, 스택 크기 144바이트 이상, 4바이트 정렬을 검증한다. 스택 하단 16바이트에는 `0x5A5A5A5A` 카나리를 쓰고, 상단 128바이트에 `mstatus = 0x1880`, `mepc = task_func`, `ra = task_return_guard`를 포함한 초기 프레임을 만든다.

- `task_yield()`: 락 보유 시 패닉, 그 외에는 임계 구역에서 MSIP를 예약한다.
- `task_sleep_ticks()`: 0이면 yield, 락 보유 또는 IDLE 호출은 패닉, 만료 시 Ready 복귀한다.
- `idle_task()`: 슬롯 31 고정, `while (1) { __asm__ volatile ("wfi"); }`.
- 태스크 함수가 반환하면 `task_return_guard()`가 `kernel_panic("task returned")`를 호출한다.

## 7. IPCP 뮤텍스 및 세마포어

### 7.1 뮤텍스

뮤텍스는 정적으로 초기화하고 대기 큐를 할당하지 않는다. `mutex_lock()`은 임계 구역에서 천장 위반 및 `locked` 이중 락을 검사한 뒤 `saved_prio`와 `prev_mutex`를 기록하고 현재 우선순위를 천장값으로 낮은 숫자 방향으로 승격한다. 비트맵은 조작하지 않는다.

`mutex_unlock()`은 최상단 뮤텍스 일치 및 `locked` 상태를 검사하고 LIFO로 팝한다. 저장된 우선순위를 복원한 뒤 Ready 큐를 재평가하고 필요하면 MSIP를 예약한다. 비순차 해제는 즉시 패닉이다.

### 7.2 세마포어

`count > 0`이면 감소하고 반환한다. `count == 0`이면 현재 슬롯을 `waiters`에 기록하고 Blocked 상태로 전환한 뒤 MSIP를 요청한다. `semaphore_signal()`은 대기자가 없을 때만 `count++`하고, 대기자가 있으면 Guard Bit을 넣지 않은 순수 LSB 고립으로 깨울 슬롯을 정한다.

락을 보유한 태스크 또는 IDLE은 sleep, semaphore wait, yield를 호출할 수 없다 (`INV-SYN-012`).

## 8. 임계 구역 및 CLINT 원자성

임계 구역 API는 태스크 컨텍스트 전용이다.

```c
static inline void enter_critical(void) {
    __asm__ volatile ("csrci mstatus, 8" ::: "memory");
    __asm__ volatile ("" ::: "memory");
    g_critical_nesting_count++;
    if (g_critical_nesting_count == 0xFFFFFFFFU)
        kernel_panic("critical overflow");
}

static inline void exit_critical(void) {
    if (g_critical_nesting_count == 0U)
        kernel_panic("critical underflow");
    g_critical_nesting_count--;
    if (g_critical_nesting_count == 0U) {
        __asm__ volatile ("" ::: "memory");
        __asm__ volatile ("fence rw, rw" ::: "memory");
        __asm__ volatile ("csrsi mstatus, 8" ::: "memory");
    }
}
```

`mtime`는 High-Low-High로 읽고, `mtimecmp`는 `HIGH = 0xFFFFFFFF` → `LOW = low` → `HIGH = high` 순서로 쓴다. 비교값은 매 틱 현재 시각이 아니라 직전 비교값에 10,000을 누적하여 드리프트를 막는다.

---

# PART II. 상세 설계 명세서 (LLD)

## 9. 디렉터리 및 파일 아키텍처

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

컴파일 기준은 `-march=rv32imc_zicsr -mabi=ilp32 -O2`이다. `kernel/ipcp.c`와 `g_prio_to_slot[]`는 이 기준선에서 존재하지 않는다.

## 10. 모듈별 구현 계약

| 파일 | 핵심 심볼 | 계약 |
| --- | --- | --- |
| `boot/startup.S` | `_start` | SP 초기화, BSS 클리어, `main` 호출 |
| `kernel/context.S` | `trap_entry` | 128B 프레임 저장, 시스템 스택 전환, C 디스패치, SP 교체 및 복원 |
| `kernel/context.S` | `prio_context_first_switch` | 첫 TCB의 CSR·레지스터 복원 및 `mret` |
| `kernel/context.S` | `task_return_guard` | 반환 태스크 패닉 |
| `kernel/trap.c` | `trap_init`, `trap_handler` | Direct `mtvec`, Cause 3/7 디스패치 |
| `kernel/sched.c` | `task_create`, `sched_highest_priority_slot` | 정적 등록 및 branchless LSB 선택 |
| `kernel/sched.c` | `sched_select_next_task`, `task_sleep_ticks` | 상태·비트맵 전이 및 슬립 등록 |
| `kernel/sched.c` | `timer_tick_step`, `get_tick_count` | 틱 갱신, 만료 복귀, 원자적 읽기 |
| `kernel/mutex.c` | `mutex_init`, `mutex_lock`, `mutex_unlock` | 정적 IPCP Shadowing 및 LIFO 검증 |
| `kernel/semaphore.c` | `semaphore_init`, `semaphore_wait`, `semaphore_signal` | 카운팅 및 순수 LSB 대기자 선택 |
| `kernel/panic.c` | `kernel_panic` | 인터럽트 차단, UART 보고, Fail-Stop |
| `bsp/clint.c` | CLINT API | MSIP, MTIME, MTIMECMP 원자 접근 |
| `bsp/uart.c` | UART API | bounded polling 기반 16550 입출력 |
| `app/main.c` | `main` | 플랫폼·트랩·CLINT·태스크 초기화 |
| `app/tasks.c` | `tasks_init`, `task_a`, `task_b` | 정적 응용 태스크 바인딩 |

## 11. 128바이트 문맥 프레임

TCB의 `sp`는 프레임 시작을 가리킨다. 프레임은 32개 32비트 워드이며, `sp + 0x00`은 `ra`, `sp + 0x78`은 `mepc`, `sp + 0x7C`는 `mstatus`이다. 런타임 트랩 진입 시 동일한 레이아웃을 사용하여 ABI 의존적인 C 문맥 교환을 제거한다.

스택 하단은 다음 순서의 16바이트 카나리로 보호한다.

```text
stack_base + 0x00..0x0C: 0x5A5A5A5A × 4
mscratch boundary:       stack_base + 144
```

## 12. 핵심 자료구조

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
```

필수 단언:

```c
_Static_assert(OS_MAX_TASKS == 32U, "OS_MAX_TASKS must be exactly 32");
_Static_assert(sizeof(mutex_t) == 8, "mutex_t size mismatch");
_Static_assert(sizeof(TCB_t) == 24, "TCB_t size mismatch");
_Static_assert(offsetof(TCB_t, sp) == 0, "TCB sp offset mismatch");
_Static_assert(offsetof(TCB_t, stack_base) == 16, "TCB stack_base offset mismatch");
_Static_assert(sizeof(Semaphore_t) == 8, "Semaphore_t size mismatch");
```

## 13. 정적 메모리 레지스트리

| 심볼 | 크기 | 역할 |
| --- | ---: | --- |
| `g_tcbs[32]` | 768 B | 슬롯 인덱스와 우선순위가 동일한 TCB 배열 |
| `g_sleep_remaining_ticks[32]` | 128 B | 태스크별 잔여 슬립 틱 |
| Ready/Sleep bitmap | 8 B | Ready 및 Sleep 슬롯 상태 |
| `g_system_ticks` / `g_last_mtimecmp` | 16 B | 64비트 시스템 틱 및 누적 비교값 |
| `g_current_tcb` / nesting counter | 8 B | 현재 TCB 및 임계 구역 깊이 |
| `idle_stack[1024]` | 1,024 B | IDLE 전용 스택 |
| `kernel_sys_stack[1024]` | 1,024 B | 부트 및 C 트랩 핸들러 전용 스택 |

제어 구조체 합계는 928바이트, 두 정적 스택을 포함한 시스템 정적 RAM 목표는 2,976바이트이다. `g_prio_to_slot[32]`와 동적 뮤텍스 풀은 없다.

## 14. 비트맵 상태 예시

1. 초기 상태: `g_ready_bitmap = 0x00000000`
2. 태스크 4, 5 등록 후: `0x00000030`
3. IDLE 31 등록 후: `0x80000030`
4. 태스크 4 Running 후: `0x80000020`
5. 응용 태스크가 모두 Blocked이고 IDLE이 Running이면: `0x00000000`
6. 이 경우에도 스케줄러의 연산 시점 Guard Bit으로 슬롯 31을 선택한다.
7. MSIP 선점 시 이전 Running 태스크는 반드시 Ready 비트맵으로 복귀한다.

---

# 15. 검증 체크리스트 및 구현 기준선

- [ ] 링커에서 `.heap=0` 및 RAM 배치 확인
- [ ] `OS_MAX_TASKS == 32` 및 구조체 오프셋 단언 통과
- [ ] De Bruijn LUT가 32개 인덱스를 모두 포함하는지 검증
- [ ] 스케줄러와 세마포어가 서로 다른 Guard Bit 정책을 사용하는지 검증
- [ ] ISR call graph에 `enter_critical`/`exit_critical`가 없는지 검사
- [ ] `trap_entry` 외부에 문맥 교환 구현이 없는지 검사
- [ ] `mtimecmp` High-Low-High 쓰기와 누적 비교값을 검증
- [ ] 중복 우선순위, 이중 락, 비순차 unlock, 락 보유 중 block/yield를 결함 주입
- [ ] 카나리 및 `mscratch` 144바이트 경계를 문맥 교환마다 검사
- [ ] UART 및 기타 하드웨어 폴링의 고정 timeout을 검사

---

# Revision History

| 버전 | 일자 | 변경 사항 |
| --- | --- | --- |
| v2.2.3 | 2026-10-30 | 단일 `trap_entry` 에필로그, 선점 태스크 Ready 복귀, ISR 임계 구역 금지, 슬립 만료 비트 클리어, 누적 `mtimecmp`, 순수 세마포어 LSB, 시스템 스택 및 컴파일 플래그 명시 |
| v2.2.4 | 2026-10-30 | 메모리 레지스트리 수치 정합성(스칼라 32 B, 제어 구조체 928 B, 시스템 정적 RAM 2,976 B), 32슬롯 슬립 WCET, v2.3.0 Delta Queue 로드맵, 64비트 타이머 롤오버 불변식 반영 |

> 본 문서는 PrioRTOS Baseline v2.2.4의 통합 기준선이다. 실제 인증, WCET, 사이클 수 및 하드웨어 이식성은 대상 보드에서 별도 측정·검증해야 한다.
