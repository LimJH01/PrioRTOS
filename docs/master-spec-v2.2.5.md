---

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

- **정적 메모리 할당 (Zero Dynamic Heap):** 런타임 힙 할당을 배제하고 컴파일 타임에 모든 메모리를 정적으로 배치한다 (INV-MEM-001).
- **최악 실행 시간 제약 (Bounded WCET):**
    - **분기 없는 LSB 드 브루인 스케줄러:** 31번 Guard Bit를 합성하여 조건 분기문 없이 정적 역어셈블리 기준 10개 명령어 이내(RV32IMC 하드웨어 곱셈기 기준 6~8 CPU 사이클)로 슬롯을 탐색한다 (INV-ALG-002).
    - **비선점 우선순위 섀도잉 IPCP:** 뮤텍스 락 획득/해제 시 비트맵 조작을 수행하지 않으며, 최대 중첩 깊이를 4단계(`OS_MAX_MUTEX_NESTING = 4U`)로 고정한다.
- **Ready 큐 슬롯 직접 인덱싱:** 뮤텍스를 점유한 태스크(`held_mutex != NULL`)는 임계 구역 내에서 선점되지 않으며(Top-Half 요청 차단 및 Bottom-Half 방어), `TASK_STATE_RUNNING` 상태를 유지한다. 따라서 Ready 큐(`g_ready_bitmap`)에 존재하는 태스크는 `priority == base_priority == task_id`를 만족하므로 간접 포인터 테이블 없이 직접 인덱싱(`&g_tcbs[slot]`)을 수행한다.
- **스택 분리:** 태스크 스택과 커널 시스템 스택(`kernel_sys_stack`, 1024B)을 분리하여 C 호출 규약(ABI)에 의한 태스크 스택 오염을 방지한다.
- **트랩 컨텍스트 검증 플래그(`g_in_trap_context`):** `mstatus.MIE == 0` 상태 외에 트랩 진입/복귀 시 갱신되는 전역 플래그를 두어 태스크 크리티컬 섹션과 실제 ISR 실행 상태를 분리 검증한다 (INV-CTX-005).
- **메모리 풋프린트:** 커널 바이너리(`.text`) 4 KiB 이내, 순수 커널 스칼라 상태 변수 36 Bytes, 제어 구조체 및 배열 총합 920 Bytes, 시스템 정적 RAM 소모 총합 3,004 Bytes(32개 TCB 768B, 슬립 배열 128B, CrashDump 24B, 스칼라 36B, IDLE 스택 1024B, 시스템 스택 1024B 전체 포함).

### 3. v2.1 대비 변경 사항

- **간접 매핑 테이블(`g_prio_to_slot[32]`) 제거:** 우선순위 간접 역참조 배열을 제거하고, `base_priority == task_id == Bit Index`의 1:1 직접 매핑 체계를 적용한다.
- **Bottom-Half 비선점 IPCP 가드 및 LIFO Priority Shadowing:** 뮤텍스 획득 직전에 펜딩된 지연 MSIP가 `exit_critical()` 직후 처리되더라도 Bottom-Half(`trap_handler` Cause 3)에서 `held_mutex != NULL`을 확인하여 문맥 교환을 취소한다. TCB의 priority를 `ceiling_prio`로 승격하여 다중 뮤텍스 중첩(최대 4단계) 시 LIFO 검증 및 중간 언락 시 원래 우선순위를 복원한다.
- **Guard Bit 주입(Sentinel Bit Masking):** `g_ready_bitmap`은 대기 중인(`TASK_STATE_READY`) 태스크만 추적하며, 슬롯 탐색 시 `safe_bm = task_bitmap | (1U << 31)`을 합성하여 분기문 없이 IDLE(31번) 폴백을 처리한다.
- **단일 트랩 에필로그 문맥 교환:** `prio_context_switch`를 폐기하고 `trap_entry` 어셈블리 단일 루틴 내에서 sp 백업, 시스템 스택 전환, C 디스패처 호출, 차기 sp 스위칭 및 레지스터 복원을 수행한다.
- **트랩 컨텍스트 검증 플래그 통합:** 태스크 크리티컬 섹션과 인터럽트 컨텍스트 구분을 위해 `g_in_trap_context`를 BSS에 상주시키고 API 진입점에서 컨텍스트 유효성을 검증한다.

## PART I. 소프트웨어 요구사항 명세서 (SRS)

### 1. 개요 및 시스템 환경 제약 (System Constraints & Environment)

#### 1.1 하드웨어 플랫폼 규격 (HW Baseline - QEMU virt 기준)

- **CPU 및 ISA:** RISC-V 32-bit Single Hart (Hart 0)
- **확장 세트:** RV32IMC Base Integer Instruction Set (컴파일 옵션: `march=rv32imc_zicsr -mabi=ilp32`)
- **엔디안:** Little-Endian
- **특권 모드:** Single M-Mode Flat Model (커널 및 모든 태스크 동일 Machine Mode 동작)

#### 1.2 물리 메모리 맵 / 기준 보드 QEMU virt (Physical Memory Map)

| **영역 명칭** | **시작 물리 주소** | **크기 (Size)** | **속성** | **하드웨어 역할** |
| --- | --- | --- | --- | --- |
| **CLINT** | `0x0200_0000` | 64 KiB | RW (MMIO) | Core Local Interruptor (소프트웨어 및 타이머 인터럽트 제어) |
| **PLIC** | `0x0C00_0000` | 64 MiB | RW (MMIO) | Platform-Level Interrupt Controller (외부 인터럽트 제어) |
| **UART0** | `0x1000_0000` | 256 B | RW (MMIO) | NS16550A 호환 디버그 시리얼 포트 (115200-8-N-1) |
| **DRAM** | `0x8000_0000` | 128 MiB | RWX | 커널 및 애플리케이션 전체 메모리 (코드, 상수, 전역 데이터, 정적 스택) |

#### 1.3 CLINT 레지스터 오프셋 및 타임베이스 규격 (Hart 0 기준)

- `CLINT_MSIP` (Cause 3 전용): `0x0200_0000` (Bit 0 제어, 1U 기록 시 소프트웨어 인터럽트 펜딩)
- `CLINT_MTIMECMP_LOW`: `0x0200_4000` / `CLINT_MTIMECMP_HIGH`: `0x0200_4004`
- `CLINT_MTIME_LOW`: `0x0200_BFF8` / `CLINT_MTIME_HIGH`: `0x0200_BFFC` (하드웨어 클럭: 10 MHz 고정)
- **시스템 틱 주기:** 1 ms Tick = 10,000,000 Hz * 0.001 s = 10,000 cycles (`#define TIMER_INTERVAL 10000ULL`)
- **인터럽트 벡터 설정:** `mtvec` 설정은 Direct Mode(`MODE = 0x00`)로 고정하며, 베이스 주소는 `trap_entry` 레이블에 바인딩되고 4바이트 정렬을 준수한다.
    - **하드웨어 진입점 및 소프트웨어 트랩 디스패치 (`trap_handler` 내부 mcause 분기):**
        - Cause 7 (`0x80000007U`, MTIP): Top-Half 틱 디스패처로 분기 (`timer_tick_step()`).
        - Cause 3 (`0x80000003U`, MSIP): Bottom-Half 문맥 교환기(DPC)로 분기.
        - 미인가 트랩 및 동기 예외: `unhandled_trap()`로 분기하여 `g_crash_dump` 기록 후 `wfi` 무한 루프 진입.

### 2. 시스템 아키텍처 불변 원칙 (System Invariants)

| **불변식 ID** | **카테고리** | **불변식 명칭** | **제약 내용** | **검증 수단** | **위반 시 동작** |
| --- | --- | --- | --- | --- | --- |
| **INV-MEM-001** | 메모리 | Zero Dynamic Heap | 런타임 힙 동적 할당(`malloc`, `free`)을 금지하고, 모든 메모리를 컴파일 타임에 정적 배치한다. | 링커 스크립트 `.heap=0` 설정 및 `.map` 심볼 검사 | 빌드 실패 |
| **INV-ALG-002** | 복잡도 | Bounded O(1) Complexity | 스케줄러 탐색 시 분기문을 배제하고, 슬립 감산($N \le 31$) 및 뮤텍스 중첩 순회($\le 4$)의 상한을 상수로 고정한다. | 역어셈블리 내 분기문 및 루프 상한 검사 | 정적 검증 실패 |
| **INV-SCH-003** | 스케줄러 | Pure Direct Mapping & Non-Preemption | `g_ready_bitmap`의 비트 $p$는 `task_id == base_priority == p`를 의미한다. 뮤텍스 보유 태스크(`held_mutex != NULL`)는 선점되지 않으며 Ready 큐로 전이되지 않는다. | 선점 경로 및 Bottom-Half `held_mutex != NULL` 가드 단위 테스트 | 오류 검출 |
| **INV-INT-004** | 인터럽트 | Single-Pipeline Trap & DPC | 타이머 ISR(Cause 7) 내 직접 문맥 교환을 금지하고, CLINT MSIP(Cause 3)를 통해 DPC로 위임하여 `trap_entry` 에필로그에서 단일 문맥 교환을 수행한다. | 정적 Call-Graph 검사, `context.S` 실행 경로 검증 | 시스템 스택 오염 차단 |
| **INV-CTX-005** | 컨텍스트 | Context Safety Matrix & Trap Flag | ISR(Top-Half) 내부에서는 `enter_critical`, `exit_critical`, 뮤텍스 API, 블로킹 API 호출을 금지한다. `g_in_trap_context`를 두어 트랩 진입 시 1, 복귀 시 0으로 관리하며, ISR API는 `mstatus.MIE == 0 && g_in_trap_context == 1`을, 태스크 API는 `g_in_trap_context == 0`을 검증한다. | 컨텍스트 검증 단언문 및 결함 주입 테스트 | `kernel_panic("ERR_ILLEGAL_ISR_STATE")` 또는 `ERR_ILLEGAL_BLOCKING` |
| **INV-CAP-006** | 용량 | Fixed 32-Slot Bounding | 슬롯 상한을 `OS_MAX_TASKS == 32`로 고정하고, 태스크 생성 시 `base_priority == task_id` 1:1 매핑을 적용한다. | `_Static_assert(OS_MAX_TASKS == 32)` 선언, 중복 등록 검사 | `kernel_panic("ERR_DUPLICATE_PRIO")` |
| **INV-LFC-007** | 수명주기 | No Runtime Task Deletion | 커널 시동 후 동적 태스크 생성/종료/삭제를 지원하지 않으며, 모든 태스크는 무한 루프로 동작한다. | 삭제 API 배제, 태스크 반환 시 `task_return_guard` 진입 | `kernel_panic("ERR_TASK_RETURNED")` |
| **INV-STK-008** | 스택 | Dual Stack & Hardware Boundary | 재귀 호출과 VLA를 금지하며, 태스크 스택 최하단 16B에 `0x5A5A5A5AU` 카나리를 검증한다. C 트랩 핸들러 실행 시 전용 시스템 스택(`kernel_sys_stack`, 1024B)으로 전환하고 `mscratch`(`stack_base + 144`) 경계를 검사한다. | 문맥 교환 시 런타임 카나리 검증, `mscratch` 경계 검사 | `kernel_panic("ERR_STACK_OVERFLOW")` |
| **INV-ERR-009** | 에러 처리 | Bounded HW Polling | 레지스터 무한 폴링 루프를 금지하며, 고정 상한 카운터(`UART_TX_TIMEOUT_LIMIT = 100000U`) 초과 시 문자를 드롭하고 함수를 반환한다. | 정적 AST 루프 검출, 무응답 하드웨어 결함 주입 테스트 | 타임아웃 처리 |
| **INV-MUT-010** | 동기화 | Bounded Static IPCP Shadowing | 뮤텍스 획득 시 TCB priority를 `ceiling_prio`로 승격하고 `saved_prio`에 LIFO 방식으로 저장한다. 최대 중첩 깊이(`OS_MAX_MUTEX_NESTING == 4U`), 경합, 천장 위반, 역순 해제 위반 시 커널 패닉을 호출한다. | 결함 주입 단위 테스트 (경합, 중첩 5회, 순환 포인터 주입) | `kernel_panic` 진입 |
| **INV-ATM-011** | 드라이버 | 64-bit Atomic & Overrun-Safe Timer | 32비트 버스 상에서 64비트 타이머 레지스터를 원자적으로 접근한다. 정상 시 `g_last_mtimecmp += 10000ULL` 누적 가산하되, 오버런(`now >= g_last_mtimecmp + 10000ULL`) 발생 시 1틱만 처리하고 `now + 10000ULL`로 재설정한다. | 롤오버 경계값 주입 및 인터럽트 지연 오버런 테스트 | 드리프트 방지 및 틱 처리 |
| **INV-CRI-012** | 동시성 | Critical Section Nesting | 중첩 카운터(`g_critical_nesting_count`)를 관리하며, 카운터가 0일 때만 컴파일러/메모리 배리어를 거쳐 `MIE=1`을 복원한다 (태스크 컨텍스트 전용). | 카운터 언더플로/오버플로 검사, 진입/탈출 쌍 검증 | `kernel_panic("ERR_CRIT_UNDERFLOW")` |
| **INV-SYN-013** | 동기화 | Primitive Isolation | IPCP 뮤텍스를 점유(`held_mutex != NULL`)한 태스크는 `task_sleep_ticks`, `semaphore_wait`, `task_yield`를 호출할 수 없다. | 결함 주입 단위 테스트 (뮤텍스 락 상태에서 대기/양보 호출) | `kernel_panic("ERR_ILLEGAL_BLOCKING")` |

### 3. 커널 서브시스템 기능 요구사항 (Subsystem Functional Requirements)

#### 3.1 스케줄러 및 인터럽트 동작 정책

#### 3.1.1 1:1 직접 매핑 및 비선점 임계 구역 규칙 (INV-SCH-003)

- **슬롯 직접 매핑 (Pure Direct Mapping):** 우선순위 0(최고)부터 31(최저, IDLE)까지 총 32개 레벨을 사용하며, `task_id == base_priority == Bit Index` 매핑을 적용한다. 별도의 포인터 배열을 두지 않고 비트맵 슬롯 번호 $p$로 `&g_tcbs[p]`를 직접 참조한다.
- **Running / Ready 상태 분리:** 실행 중인 태스크(`TASK_STATE_RUNNING`)는 IDLE 태스크(`task_id == 31U`)를 포함하여 `g_ready_bitmap`에서 해당 비트를 클리어하고 `g_current_tcb` 포인터로 관리된다.
- **뮤텍스 보유 태스크 비선점:**
    - 태스크가 뮤텍스를 1개 이상 보유(`g_current_tcb->held_mutex != NULL`)하는 동안에는 선점되지 않으며, 블로킹 API 호출이 제한된다(INV-SYN-013).
    - 따라서 `g_ready_bitmap`에 설정된 모든 태스크는 `held_mutex == NULL` 상태이며, `priority == base_priority == task_id`를 만족한다.

#### 3.1.2 Guard Bit 주입 기반 De Bruijn LSB O(1) 스케줄러 (REQ-SCH-001)

조건 분기문 없이 상수 시간($O(1)$)으로 슬롯을 산출한다 (`sched_highest_priority_slot`).

1. **1단계 (Guard Bit 주입):**
    
    ```
    uint32_t safe_bm = task_bitmap | (1U << 31);
    ```
    
    준비 큐가 비어 `task_bitmap == 0U`인 경우에도 `safe_bm`은 최소 `0x80000000U`를 가지므로 `if (bitmap == 0)` 분기문을 사용하지 않는다.
    
2. **2단계 (LSB 단일 비트 고립):**
    
    ```
    uint32_t v = safe_bm & (0U - safe_bm);
    ```
    
3. **3단계 (De Bruijn 곱셈 해싱 및 인덱스 산출):**
    
    ```
    uint32_t index = (uint32_t)(v * 0x077CB531U) >> 27;
    ```
    
4. **4단계 (슬롯 번호 확정):**
    
    ```
    uint8_t highest_slot = DeBruijn_LSB_LUT[index];
    ```
    
    대기 중인 태스크가 있으면 해당 LSB(0 ~ 30)가 반환되고, 없으면 31(IDLE)이 반환된다.
    

#### 3.1.3 인터럽트 처리, Trap Flag 관리 및 Pending MSIP 처리 (Top-Half / Bottom-Half)

1. **트랩 진입 플래그 관리 (INV-CTX-005):**
    - 하드웨어 트랩 진입 시 `trap_handler()` 시작부에서 `g_in_trap_context = 1U;`를 설정한다.
    - 핸들러 종료 직전에 `g_in_trap_context = 0U;`를 설정하여 태스크 복귀 전 플래그를 클리어한다.
2. **타이머 ISR (Cause 7, Top-Half - `timer_tick_step`):**
    - **타이머 레지스터 갱신 (INV-ATM-011):**
        - 현재 시각 `uint64_t now = clint_get_mtime();`을 읽는다.
        - **정상 경로 (`now < g_last_mtimecmp + TIMER_INTERVAL`):** `g_last_mtimecmp += TIMER_INTERVAL;`로 설정하여 누적 드리프트를 방지한다.
        - **오버런 경로 (`now >= g_last_mtimecmp + TIMER_INTERVAL`):** 지연 발생 시 추가 루프 없이 단 1틱만 처리하고, 다음 비교값을 `g_last_mtimecmp = now + TIMER_INTERVAL;`로 재설정한다.
    - **논리 틱 증가:** `g_system_ticks++;`
    - **슬립 카운트다운 및 상태 전이:** `g_sleep_bitmap != 0U`일 경우 tid (0 ~ 30)를 순회하며 `g_sleep_remaining_ticks[tid]`를 감산한다. 카운터가 0이 되면 `g_sleep_bitmap &= ~(1U << tid); g_tcbs[tid].state = TASK_STATE_READY; g_ready_bitmap |= (1U << tid);`를 실행한다.
    - **선점 요청:** `sched_highest_priority_slot(g_ready_bitmap)`의 결과가 `candidate < g_current_tcb->priority && g_current_tcb->held_mutex == NULL`을 만족하면 `clint_send_software_interrupt()`를 호출한다.
3. **소프트웨어 ISR (Cause 3, Bottom-Half DPC):**
    - `trap_entry`에서 현재 태스크 스택에 128B 문맥을 저장하고 시스템 스택(`_stack_top`)으로 전환하여 `trap_handler()`를 호출한다.
    - 진입 시 `clint_clear_software_interrupt()`(`CLINT_MSIP = 0U`)를 호출하여 하드웨어 펜딩 비트를 해제한다.
    - 현재 태스크(`g_current_tcb`)의 스택 최하단 카나리 4워드(`0x5A5A5A5AU`)를 검증하고 이상 발생 시 `stack_overflow_panic()`을 호출한다.
    - **비선점 임계 구역 처리 (Pending MSIP Bypass Guard):**
        - 태스크가 `mutex_lock()` 진입 직전에 MSIP가 펜딩되어 `exit_critical()` 직후 트랩이 실행된 경우를 처리하기 위해 다음 검사를 수행한다:
        
        ```
        if (g_current_tcb->held_mutex != NULL) {
            g_in_trap_context = 0U;
            return;
        }
        ```
        
    - **태스크 상태 전이:** `g_current_tcb->state == TASK_STATE_RUNNING`인 경우 Ready 상태로 전환한다(`g_current_tcb->state = TASK_STATE_READY; g_ready_bitmap |= (1U << g_current_tcb->task_id);`).
    - `sched_select_next_task()`를 호출하여 차기 태스크를 선택하고 `g_current_tcb = next`를 갱신한다.
    - `trap_entry` 에필로그에서 차기 태스크의 sp 및 `mscratch`(`stack_base + 144`)를 갱신하고 레지스터를 복원한 뒤 `mret`을 실행한다.

#### 3.2 32개 우선순위 레벨(0 ~ 31) 영역 분할 설계 (REQ-SCH-002)

| **우선순위 범위** | **도메인 명칭** | **주 실행 주기 / 트리거** | **특성 및 연산 규칙** | **대표 예시 태스크** |
| --- | --- | --- | --- | --- |
| **0 ~ 3 (4레벨)** | Safety & Fault Handling | 이벤트 발생 시 즉시 실행 | `task_sleep_ticks`, `semaphore_wait` 호출 금지. WCET $\le 50\ \mu\text{s}$ | 비상 정지(E-Stop), 과전압 차단, 워치독 페일세이프 |
| **4 ~ 16 (13레벨)** | Hard Real-Time Control | 1 ms ~ 10 ms 고정 주기 | IPCP 뮤텍스 사용 가능 (`semaphore_wait` 지양). 지터 허용치 $\le 10\ \mu\text{s}$ | 모터 FOC 정밀 각도 제어, 인버터 제어, IMU 필터 |
| **17 ~ 27 (11레벨)** | Soft Real-Time & Comm | 10 ms ~ 100 ms 또는 통신 ISR | 세마포어 기반 대기 허용. 1~2틱 지연 허용 | UART 커맨드 파서, 패킷 CRC 검증, 텔레메트리 |
| **28 ~ 30 (3레벨)** | Background & Health | 500 ms 이상 비동기 | CPU 유휴 시간에 동작. 상위 태스크에 의해 선점 허용 | 태스크 카나리 백그라운드 스캔, 시스템 통계 |
| **31 (1레벨)** | System IDLE | 상시 실행 | 블로킹 API 및 뮤텍스 API 호출 금지. `wfi` 저전력 대기 루프 실행 | `idle_task` |

#### 3.3 부팅 시퀀스 및 예외 처리 정책

- **부팅 시퀀스 (Step 0 ~ Step 5):**
    - **Step 0 (`_start` 진입):** 링커 스크립트 RAM 시작 주소(`0x8000_0000`)의 `_start`로 진입.
    - **Step 1 (스택 및 BSS 초기화):** sp를 `_stack_top`으로 설정하고 `.bss` 영역을 0으로 클리어(`g_in_trap_context = 0` 포함)한 후 `main()` 호출.
    - **Step 2 (플랫폼 및 트랩 초기화):** `uart_init()`으로 UART0 초기화, `trap_init()`으로 `trap_entry` 주소를 Direct Mode(`0x00`)로 `mtvec`에 설정.
    - **Step 3 (CLINT 및 태스크 초기화):** `clint_software_interrupt_init()`으로 MSIP 클리어 및 `mie.MSIE = 1` 설정, `clint_timer_init()`으로 `g_last_mtimecmp = clint_get_mtime() + TIMER_INTERVAL` 장전 및 `mie.MTIE = 1` 설정. `tasks_init()`으로 정적 태스크 등록.
    - **Step 4 (커널 시동):** `priortos_start()`에서 슬롯 31에 `idle_task` 등록 확인, `sched_select_next_task()`로 최고 우선순위 태스크를 선택하여 `TASK_STATE_RUNNING` 전환 및 `g_current_tcb` 바인딩.
    - **Step 5 (최초 문맥 복원):** `(uint32_t)(g_current_tcb->stack_base + 144U)`를 `mscratch`에 기록하고 `prio_context_first_switch(g_current_tcb)`를 호출하여 레지스터 및 CSR(`mstatus=0x00001880U`, `mepc`) 복원 후 `mret` 실행.
- **예외 처리 및 패닉 정책:**
    - **미인가 트랩 (Cause 3, 7 이외):** `unhandled_trap()`에서 `g_crash_dump`에 mcause, mepc, mtval, sp, task_id를 기록하고 에러 문자열을 출력한 후 `wfi` 루프에 머문다.
    - **명시적 커널 패닉 (`kernel_panic`):** `csrci mstatus, 8`로 전역 인터럽트를 차단하고 `g_crash_dump` 기록 및 UART 문자열 출력 후 `wfi` 루프에 머문다.

#### 3.4 태스크 제어 서브시스템 (Task Control Subsystem)

- **REQ-TSK-001 (정적 태스크 등록 및 스택 빌드 - `task_create`):**
    - 인자 검증: `prio >= OS_MAX_TASKS`, `stack == NULL`, `task_func == NULL`, `stack_size_bytes < 144U`, 또는 `((uintptr_t)stack & 0x3U) != 0U`인 경우 `TASK_CREATE_ERR_INVALID_PARAM` 반환.
    - `task_id = (uint8_t)prio`, `base_priority = (uint8_t)prio`, `priority = (uint8_t)prio`로 설정하며, 슬롯 중복 등록(`g_tcbs[prio].state != TASK_STATE_UNUSED`) 시 `kernel_panic("ERR_DUPLICATE_PRIO")`를 호출한다.
    - 스택 최하단 16바이트에 `0x5A5A5A5AU`를 기록하고, 최상단 128바이트에 초기 문맥(`mstatus = 0x00001880U`, `mepc = (uint32_t)task_func`, `ra = (uint32_t)task_return_guard`)을 구성한다.
    - TCB 필드 갱신 후 `g_ready_bitmap |= (1U << prio)`를 설정한다.
- **REQ-TSK-002 (자발적 CPU 양보 - `task_yield`):**
    - `assert_task_context()`를 수행하여 `g_in_trap_context != 0U`일 경우 `kernel_panic("ERR_ILLEGAL_ISR_STATE")`를 호출한다 (INV-CTX-005).
    - 호출 태스크가 뮤텍스를 점유 중(`g_current_tcb->held_mutex != NULL`)이거나 IDLE 태스크(`g_current_tcb->task_id == 31U`)이면 `kernel_panic("ERR_ILLEGAL_YIELD")`를 호출한다 (INV-SYN-013).
    - `enter_critical()` $\to$ `clint_send_software_interrupt()` $\to$ `exit_critical()`을 수행한다.
- **REQ-TSK-003 (지정 틱 슬립 - `task_sleep_ticks`):**
    - `assert_task_context()`를 수행하여 `g_in_trap_context != 0U`일 경우 `kernel_panic("ERR_ILLEGAL_ISR_STATE")`를 호출한다 (INV-CTX-005).
    - 호출 태스크가 뮤텍스를 점유 중(`g_current_tcb->held_mutex != NULL`)이거나 IDLE 태스크(`g_current_tcb->task_id == 31U`)이면 `kernel_panic("ERR_ILLEGAL_SLEEP")`를 호출한다 (INV-SYN-013).
    - `ticks == 0U`이면 `task_yield()` 호출 후 반환한다.
    - `enter_critical()` 진입 $\to$ `g_sleep_remaining_ticks[tid] = ticks;` $\to$ `g_sleep_bitmap |= (1U << tid);` $\to$ `g_current_tcb->state = TASK_STATE_BLOCKED;` $\to$ `clint_send_software_interrupt();` $\to$ `exit_critical()`을 수행한다.
- **REQ-TSK-004 (유휴 태스크 본체 - `idle_task`):**
    - 최하위 우선순위 31(`task_id == 31U`)로 등록되며, 본체는 `while (1) { __asm__ volatile("wfi"); }`이다.
- **REQ-TSK-005 (커널 시동 - `priortos_start`):**
    - 슬롯 31의 `idle_task` 등록을 확인하고, `sched_select_next_task()`로 최고 우선순위 태스크를 선정하여 `mscratch` 장전 후 `prio_context_first_switch(g_current_tcb)`를 실행한다.

#### 3.5 상호 배제 서브시스템 (바운디드 LIFO 우선순위 섀도잉 IPCP)

- **우선순위 섀도잉 적용 목적:**
    
    뮤텍스 보유 중 비선점 정책(INV-SCH-003)과 함께 `g_current_tcb->priority`를 `ceiling_prio`로 승격하는 이유는 다음과 같다:
    
    1. 다중 뮤텍스 중첩 시 LIFO 단계별 천장 자격 검증 및 `saved_prio` 보존.
    2. 중첩된 락 해제 시 바깥쪽 락의 천장 우선순위로 정확한 복원.
    3. 크래시 덤프 및 진단 시 현재 실행 중인 임계 구역의 천장 권한 식별.
- **REQ-SYN-001 (IPCP 뮤텍스 정적 초기화 - `mutex_init`):**
    - `m == NULL` 또는 `ceiling_prio >= 31U`인 경우 `kernel_panic("ERR_MUTEX_INIT_PARAM")`를 호출한다.
    - 구조체 필드를 0 및 NULL로 초기화한다.
- **REQ-SYN-002 (뮤텍스 획득 및 중첩 검사 - `mutex_lock`):**
    1. `assert_task_context()` 수행: `g_in_trap_context != 0U` 시 `kernel_panic("ERR_ILLEGAL_ISR_STATE")`를 호출한다 (INV-CTX-005).
    2. `enter_critical()` 진입.
    3. `m == NULL || g_current_tcb->task_id == 31U`이면 `kernel_panic("ERR_MUTEX_INVALID_CALL")`를 호출한다.
    4. `m->locked != 0U`이면 `kernel_panic("ERR_IPCP_CONTENTION")`를 호출한다 (COND-MUT-03).
    5. `g_current_tcb->base_priority < m->ceiling_prio`이면 `kernel_panic("ERR_IPCP_CEILING_VIOLATION")`를 호출한다 (COND-MUT-02).
    6. 중첩 검사:
        
        ```
        uint32_t nest_cnt = 0U;
        mutex_t *curr_m = g_current_tcb->held_mutex;
        while (curr_m != NULL && nest_cnt < OS_MAX_MUTEX_NESTING) {
            nest_cnt++;
            curr_m = curr_m->prev_mutex;
        }
        if (curr_m != NULL || nest_cnt >= OS_MAX_MUTEX_NESTING) {
            kernel_panic("ERR_IPCP_NESTING");
        }
        ```
        
    7. 상태 기록: `m->locked = 1U; m->saved_prio = g_current_tcb->priority; m->prev_mutex = g_current_tcb->held_mutex; g_current_tcb->held_mutex = m;`
    8. 우선순위 승격: `if (m->ceiling_prio < g_current_tcb->priority) { g_current_tcb->priority = m->ceiling_prio; }`
    9. `exit_critical()` 탈출.
- **REQ-SYN-003 (뮤텍스 해제 및 선점 판정 - `mutex_unlock`):**
    1. `assert_task_context()` 수행: `g_in_trap_context != 0U` 시 `kernel_panic("ERR_ILLEGAL_ISR_STATE")`를 호출한다 (INV-CTX-005).
    2. `enter_critical()` 진입.
    3. `m == NULL || m->locked == 0U`이면 `kernel_panic("ERR_IPCP_UNLOCK_UNLOCKED")`를 호출한다 (COND-MUT-06).
    4. `g_current_tcb->held_mutex != m`이면 `kernel_panic("ERR_IPCP_LIFO_VIOLATION")`를 호출한다 (COND-MUT-05).
    5. 상태 복원: `g_current_tcb->held_mutex = m->prev_mutex; m->prev_mutex = NULL; m->locked = 0U; g_current_tcb->priority = m->saved_prio;`
    6. 최외곽 해제 검사: `g_current_tcb->held_mutex == NULL` 상태에서 `sched_highest_priority_slot(g_ready_bitmap) < g_current_tcb->priority`이면 `clint_send_software_interrupt()`를 호출한다.
    7. `exit_critical()` 탈출.

#### IPCP 예외 판정 매트릭스 (COND-MUT-01 ~ 06)

| **조건 ID** | **판정 조건식 (0 = 최고 우선순위)** | **시스템 동작** | **분석** |
| --- | --- | --- | --- |
| **COND-MUT-01** | `m->locked == 0U && base_priority >= ceiling_prio && nest_cnt < 4U` | 락 획득, `saved_prio` 저장, `priority` 승격 | 정상 실행 경로 |
| **COND-MUT-02** | `g_current_tcb->base_priority < m->ceiling_prio` | `kernel_panic("ERR_IPCP_CEILING_VIOLATION")` | 기본 우선순위가 천장보다 높은 태스크의 락 요청 |
| **COND-MUT-03** | `m->locked != 0U` | `kernel_panic("ERR_IPCP_CONTENTION")` | 뮤텍스 중복 점유 또는 재귀 호출 |
| **COND-MUT-04** | `nest_cnt >= 4U` 또는 `curr_m != NULL` | `kernel_panic("ERR_IPCP_NESTING")` | 중첩 깊이 초과(최대 4단계) 및 링크 순환 오류 |
| **COND-MUT-05** | `g_current_tcb->held_mutex != m` | `kernel_panic("ERR_IPCP_LIFO_VIOLATION")` | LIFO 해제 순서 위반 |
| **COND-MUT-06** | `m->locked == 0U` | `kernel_panic("ERR_IPCP_UNLOCK_UNLOCKED")` | 잠기지 않은 뮤텍스 해제 시도 |

#### 3.6 세마포어 서브시스템 (Counting Semaphore)

- **REQ-SYN-004 (세마포어 대기 - `semaphore_wait`, 태스크 전용):**
    - `assert_task_context()` 수행: `g_in_trap_context != 0U`일 경우 `kernel_panic("ERR_ILLEGAL_ISR_STATE")`를 호출한다 (INV-CTX-005).
    - `sem == NULL`이면 `SEM_ERR_INVALID_PARAM` 반환.
    - `g_current_tcb->held_mutex != NULL || g_current_tcb->task_id == 31U`이면 `kernel_panic("ERR_ILLEGAL_BLOCKING")`를 호출한다 (INV-SYN-013).
    - `enter_critical()` 진입 후 `sem->count > 0U`이면 `sem->count--` 후 `exit_critical()` 및 `SEM_OK` 반환.
    - `sem->count == 0U`이면 `sem->waiters |= (1U << g_current_tcb->task_id)` 설정, `g_current_tcb->state = TASK_STATE_BLOCKED` 전이 후 `clint_send_software_interrupt()` 호출, `exit_critical()` 및 `SEM_OK` 반환.
- **REQ-SYN-005 (세마포어 시그널 - `semaphore_signal` [Task 전용] / `semaphore_signal_from_isr` [ISR 전용]):**
    - `semaphore_signal(sem)`: 태스크 전용. `assert_task_context()` 확인 후 `enter_critical()` / `exit_critical()` 구간에서 수행.
    - `semaphore_signal_from_isr(sem)`: ISR 전용. `assert_isr_context()`를 호출하여 `mstatus.MIE == 0 && g_in_trap_context == 1U` 상태를 검증한다. 조건 미충족 시 `kernel_panic("ERR_ILLEGAL_ISR_STATE")`를 호출한다.
    - **시그널 처리 절차:**
        1. `sem == NULL`이면 `SEM_ERR_INVALID_PARAM` 반환.
        2. `sem->waiters == 0U`이면 `sem->count++` 후 `SEM_OK` 반환.
        3. `sem->waiters != 0U`이면 Guard Bit 없이 LSB 분리(`uint32_t v = sem->waiters & (0U - sem->waiters);`) 후 `wakeup_slot = Sem_DeBruijn_LSB_LUT[(uint32_t)(v * 0x077CB531U) >> 27]`를 구한다.
        4. `sem->waiters &= ~(1U << wakeup_slot);` 제거 후 `g_tcbs[wakeup_slot].state = TASK_STATE_READY; g_ready_bitmap |= (1U << wakeup_slot);`를 실행한다.
        5. `wakeup_slot < g_current_tcb->priority && g_current_tcb->held_mutex == NULL`이면 `clint_send_software_interrupt()`를 호출한다.

#### 3.7 임계 구역, 시간 및 UART 서브시스템 (API 계약)

- **REQ-CRI-001 (`enter_critical` 태스크 전용):**
    - `assert_task_context()` 수행: `g_in_trap_context != 0U` 시 `kernel_panic("ERR_ILLEGAL_ISR_STATE")`를 호출한다.
    - `csrci mstatus, 8` $\to$ 컴파일러 배리어 $\to$ 오버플로 검사(`0xFFFFFFFFU` 시 `kernel_panic("ERR_CRIT_OVERFLOW")`) $\to$ `g_critical_nesting_count++`.
- **REQ-CRI-002 (`exit_critical` 태스크 전용):**
    - `assert_task_context()` 수행: `g_in_trap_context != 0U` 시 `kernel_panic("ERR_ILLEGAL_ISR_STATE")`를 호출한다.
    - 언더플로 검사(`0U` 시 `kernel_panic("ERR_CRIT_UNDERFLOW")`) $\to$ `g_critical_nesting_count--` $\to$ 카운터가 0이면 컴파일러 배리어 $\to$ `fence rw, rw` $\to$`csrsi mstatus, 8` 실행.
- **REQ-TMR-001 (`get_tick_count` [Task 전용] / `get_tick_count_from_isr` [ISR 전용]):**
    - `uint64_t get_tick_count(void)`: `assert_task_context()` 확인 후 `enter_critical()` 보호 하에 `g_system_ticks` 복사 및 반환.
    - `uint64_t get_tick_count_from_isr(void)`: `assert_isr_context()` 검증 후 `g_system_ticks` 반환.
- **REQ-TMR-002 (`timer_tick_step` Cause 7 Top-Half ISR 전용):**
    - `trap_handler()`의 `mcause == 0x80000007U` 분기에서 호출된다.
- **REQ-DRV-001 (`clint_get_mtime` / `clint_set_mtimecmp`):**
    - `uint64_t clint_get_mtime(void)`: High-Low-High 루프로 64비트 mtime 판독.
    - `void clint_set_mtimecmp(uint64_t compare_value)`: HIGH = `0xFFFFFFFFU` $\to$ LOW = low $\to$ HIGH = high 순서로 설정.
- **REQ-DRV-002 (`uart_putc` / `uart_puts` 폴링 상한):**
    - `UART_TX_TIMEOUT_LIMIT` (100000U) 루프 초과 시 문자를 드롭하고 반환한다 (INV-ERR-009). `uart_puts`는 최대 `UART_MAX_STRING_LEN` (256U)까지 출력한다.

#### 3.8 컨텍스트 안전 제약 매트릭스 (Context Safety Matrix)

| **커널 및 드라이버 API** | **Task Context (g_in_trap_context==0)** | **ISR (g_in_trap_context==1)** | **뮤텍스 점유 (held_mutex != NULL)** | **위반 시 동작** |
| --- | --- | --- | --- | --- |
| `enter_critical()` / `exit_critical()` | 허용 | 금지 | 허용 (중첩 카운팅) | `kernel_panic("ERR_ILLEGAL_ISR_STATE")` |
| `task_create()` / `priortos_start()` | 금지 (부트 전용) | 금지 | 금지 | `kernel_panic("ERR_DUPLICATE_PRIO")` |
| `task_yield()` | 허용 | 금지 | 금지 | `kernel_panic("ERR_ILLEGAL_YIELD")` 또는 `ERR_ILLEGAL_ISR_STATE` |
| `task_sleep_ticks()` | 허용 | 금지 | 금지 | `kernel_panic("ERR_ILLEGAL_SLEEP")` 또는 `ERR_ILLEGAL_ISR_STATE` |
| `mutex_lock()` / `mutex_unlock()` | 허용 | 금지 | 허용 (최대 4단계) | 조건별 `kernel_panic` 호출 |
| `semaphore_wait()` | 허용 | 금지 | 금지 | `kernel_panic("ERR_ILLEGAL_BLOCKING")` 또는 `ERR_ILLEGAL_ISR_STATE` |
| `semaphore_signal()` | 허용 | 금지 | 허용 | ISR에서는 `_from_isr` 사용 |
| `semaphore_signal_from_isr()` | 금지 | 허용 (`MIE==0 && Flag==1`) | 해당 없음 | `kernel_panic("ERR_ILLEGAL_ISR_STATE")` |
| `get_tick_count()` | 허용 | 금지 | 허용 | ISR에서는 `_from_isr` 사용 |
| `get_tick_count_from_isr()` | 금지 | 허용 (`MIE==0 && Flag==1`) | 해당 없음 | `kernel_panic("ERR_ILLEGAL_ISR_STATE")` |
| `timer_tick_step()` | 금지 | 허용 (Cause 7 전용) | 해당 없음 | 틱 처리 수행 |
| `clint_get_mtime()` / `clint_set_mtimecmp()` | 허용 | 허용 | 허용 | MMIO 접근 |
| `uart_putc()` / `uart_puts()` | 허용 | 금지 (Panic 예외) | 허용 | 타임아웃 초과 시 문자 드롭 |

## PART II. 상세 설계 명세서 (LLD)

### 4. 디렉토리 및 파일 아키텍처

```c
PrioRTOS/
├── Makefile                               # 빌드 스크립트 (-march=rv32imc_zicsr -mabi=ilp32 -O2)
├── README.md                              # 프로젝트 매뉴얼
├── linker.ld                              # RAM(0x8000_0000) 섹션 배치 및 _stack_top
│
├── boot/
│   └── startup.S                          # _start 진입점: SP 초기화(la sp, _stack_top), .bss 클리어 후 main()
│
├── app/
│   ├── main.c                             # 초기화, tasks_init(), priortos_start()
│   ├── tasks.h                            # 응용 태스크 헤더
│   └── tasks.c                            # 응용 태스크 구현 및 task_create() 등록
│
├── bsp/
│   ├── clint.h                            # CLINT 레지스터 오프셋 및 TIMER_INTERVAL(10000ULL)
│   ├── clint.c                            # 타이머 설정, MSIP 트리거/클리어
│   ├── uart.h                             # UART 16550 레지스터 오프셋 및 프로토타입
│   └── uart.c                             # UART 초기화 및 입출력
│
├── kernel/
│   ├── context.S                          # 문맥 복원, 단일 트랩 진입/탈출 및 sp 전환
│   ├── trap.c                             # mtvec 등록, g_in_trap_context 토글, Cause 3/7 디스패처
│   ├── sched.c                            # De Bruijn O(1) 스케줄러, 슬립 큐, timer_tick_step
│   ├── mutex.c                            # IPCP 뮤텍스 모듈
│   ├── semaphore.c                        # 카운팅 세마포어 모듈
│   ├── panic.c                            # Crash Dump 기록 및 wfi 루프
│   └── include/
│       ├── prio_rtos.h                    # 응용 계층 공개 API 헤더 (OS_MAX_TASKS == 32)
│       ├── kernel_internal.h              # 커널 구조체, g_in_trap_context 선언 및 매크로
│       └── stack_guard.h                  # 스택 카나리(0x5A5A5A5AU) 매크로
│
└── build/                                 # 빌드 출력 디렉토리
```

### 5. 모듈별 함수 원형과 구현 명세

#### 5.1 `boot/startup.S`

| **상태** | **심볼 및 함수 원형** | **입출력 규약** | **역할 및 제어 흐름** |
| --- | --- | --- | --- |
| **[구현 완료]** | `void _start(void)` | 입력: 없음 / 출력: 없음 (noreturn) | 리셋 진입점. 스택 포인터(`la sp, _stack_top`) 설정, `.bss` 메모리 클리어 후 `main` 호출. |

#### 5.2 `kernel/context.S`

| **상태** | **심볼 및 함수 원형** | **입출력 규약** | **역할 및 제어 흐름** |
| --- | --- | --- | --- |
| **[구현 완료]** | `void trap_entry(void)` | 입력: 없음 / 출력: 없음 | `mtvec` 진입 레이블. `mscratch` 검증 $\to$ 태스크 스택에 128B 저장 $\to$ `g_current_tcb->sp` 저장 $\to$ `la sp, _stack_top` 스위칭 $\to$ `call trap_handler` $\to$ 차기 `g_current_tcb->sp` 복원 $\to$ `mscratch` 갱신 $\to$ 128B 복원 후 `mret` 실행. |
| **[구현 완료]** | `void prio_context_first_switch(TCB_t *next)` | 입력: a0 (TCB 포인터) / 출력: 없음 (noreturn) | `mscratch`에 `next->stack_base + 144` 설정 $\to$ `next->sp`에서 레지스터 및 CSR 복원 후 `mret` 실행. |
| **[공식 폐기]** | `prio_context_switch` | 해당 없음 | `trap_entry` 단일 파이프라인으로 통합하여 폐기. |
| **[구현 완료]** | `void task_return_guard(void)` | 입력: 없음 / 출력: 없음 (noreturn) | 태스크 초기 스택의 ra 레지스터 바인딩 레이블. 태스크 함수가 반환될 때 `kernel_panic("ERR_TASK_RETURNED")` 호출. |

#### 5.3 `kernel/trap.c`

| **상태** | **심볼 및 함수 원형** | **입출력 규약** | **역할 및 제어 흐름** |
| --- | --- | --- | --- |
| **[구현 완료]** | `void trap_init(void)` | 입력: 없음 / 출력: 없음 | `trap_entry` 주소를 Direct Mode로 `mtvec`에 기록. |
| **[구현 완료]** | `void trap_handler(void)` | 입력: 없음 / 출력: 없음 | `g_in_trap_context = 1U;` 설정. `mcause` 확인. Cause 7: `timer_tick_step()` 호출. Cause 3: `clint_clear_software_interrupt()` $\to$ 카나리 검증 $\to$ `held_mutex != NULL` 시 조기 복귀 $\to$ 실행 중인 태스크를 Ready 큐로 전이 $\to$ `sched_select_next_task()` 호출 $\to$ 종료 전 `g_in_trap_context = 0U;` 클리어. |
| **[구현 완료]** | `void unhandled_trap(void)` | 입력: 없음 / 출력: 없음 (noreturn) | 미인가 예외 발생 시 `g_crash_dump` 기록 후 UART 메시지 출력 및 `wfi` 무한 루프 진입. |

#### 5.4 `kernel/sched.c`

| **상태** | **심볼 및 함수 원형** | **입출력 규약** | **역할 및 제어 흐름** |
| --- | --- | --- | --- |
| **[구현 완료]** | `TaskCreateResult_t task_create(uint32_t prio, void (*task_func)(void), uint8_t *stack, uint32_t stack_size_bytes)` | 입력: 우선순위, 함수 포인터, 스택 포인터, 크기 / 출력: TaskCreateResult_t | 인자 검증, 중복 우선순위 검사, 카나리(`0x5A5A5A5AU`) 주입, 128B 프레임 구성, TCB 바인딩 및 Ready 비트맵 설정. |
| **[구현 완료]** | `void priortos_start(void)` | 입력: 없음 / 출력: 없음 (noreturn) | 슬롯 31의 IDLE 태스크를 확인하고 최고 우선순위 태스크를 선택하여 `prio_context_first_switch` 호출. |
| **[구현 완료]** | `void task_yield(void)` | 입력: 없음 / 출력: 없음 | 태스크 전용. 뮤텍스 보유 여부 및 IDLE 검증 후 소프트웨어 인터럽트 트리거. |
| **[구현 완료]** | `void idle_task(void)` | 입력: 없음 / 출력: 없음 (무한 루프) | 슬롯 31 태스크 본체. `while (1) { __asm__ volatile("wfi"); }`. |
| **[구현 완료]** | `uint8_t sched_highest_priority_slot(uint32_t task_bitmap)` | 입력: 비트마스크 / 출력: 슬롯 번호 (0~31) | 31번 Guard Bit 합성 후 De Bruijn 매핑을 통해 슬롯 반환. |
| **[구현 완료]** | `TCB_t* sched_select_next_task(void)` | 입력: 없음 / 출력: 차기 TCB_t* | 최고 대기 슬롯을 선정하여 비트맵에서 제거하고 상태를 Running으로 전이한 뒤 TCB 포인터 반환. |
| **[구현 완료]** | `void sched_request_reschedule(void)` | 입력: 없음 / 출력: 없음 | `clint_send_software_interrupt()`를 호출하여 Cause 3 소프트웨어 트랩 요청. |
| **[구현 완료]** | `void stack_overflow_panic(void)` | 입력: 없음 / 출력: 없음 (noreturn) | 카나리 손상 시 `kernel_panic("ERR_STACK_OVERFLOW")` 호출. |
| **[구현 완료]** | `void task_sleep_ticks(uint32_t ticks)` | 입력: 틱 수 / 출력: 없음 | 태스크 전용. 슬립 카운터 기록 후 태스크를 Blocked 상태로 전환하고 소프트웨어 인터럽트 트리거. |
| **[구현 완료]** | `void timer_tick_step(void)` | 입력: 없음 / 출력: 없음 | Cause 7 전용. 타이머 재설정, 누적 틱 증가, 슬립 카운트 감산 및 만료 태스크 Ready 전이, 선점 판정. |
| **[구현 완료]** | `uint64_t get_tick_count(void)` | 입력: 없음 / 출력: 64비트 틱 | 태스크 전용. 크리티컬 섹션 내에서 `g_system_ticks` 복사 후 반환. |
| **[구현 완료]** | `uint64_t get_tick_count_from_isr(void)` | 입력: 없음 / 출력: 64비트 틱 | ISR 전용. 컨텍스트 검증 후 `g_system_ticks` 반환. |

#### 5.5 `kernel/mutex.c`

| **상태** | **심볼 및 함수 원형** | **입출력 규약** | **역할 및 제어 흐름** |
| --- | --- | --- | --- |
| **[구현 완료]** | `void mutex_init(mutex_t *m, uint8_t ceiling_prio)` | 입력: 뮤텍스 포인터, 천장 우선순위 / 출력: 없음 | 인자 검증 후 구조체 필드 초기화. |
| **[구현 완료]** | `void mutex_lock(mutex_t *m)` | 입력: 뮤텍스 포인터 / 출력: 없음 | 태스크 전용. 천장 조건, 경합, 중첩 깊이($<4$) 검증 후 락 상태 설정 및 우선순위 승격. |
| **[구현 완료]** | `void mutex_unlock(mutex_t *m)` | 입력: 뮤텍스 포인터 / 출력: 없음 | 태스크 전용. 락 상태 및 LIFO 해제 순서 검증 후 우선순위 복원 및 최외곽 해제 시 선점 판정. |

#### 5.6 `kernel/semaphore.c`

| **상태** | **심볼 및 함수 원형** | **입출력 규약** | **역할 및 제어 흐름** |
| --- | --- | --- | --- |
| **[구현 완료]** | `SemaphoreResult_t semaphore_init(Semaphore_t *sem, uint32_t initial_count)` | 입력: 세마포어 포인터, 초기 카운트 / 출력: SemaphoreResult_t | 구조체 초기화 (`count`, `waiters = 0`). |
| **[구현 완료]** | `SemaphoreResult_t semaphore_wait(Semaphore_t *sem)` | 입력: 세마포어 포인터 / 출력: SemaphoreResult_t | 태스크 전용. 뮤텍스 보유 여부 검증 후 카운트 감소 또는 Blocked 상태 전이. |
| **[구현 완료]** | `SemaphoreResult_t semaphore_signal(Semaphore_t *sem)` | 입력: 세마포어 포인터 / 출력: SemaphoreResult_t | 태스크 전용. 크리티컬 섹션 내에서 대기자 복귀 또는 카운트 증가. |
| **[구현 완료]** | `SemaphoreResult_t semaphore_signal_from_isr(Semaphore_t *sem)` | 입력: 세마포어 포인터 / 출력: SemaphoreResult_t | ISR 전용. 컨텍스트 검증 후 대기자 복귀 또는 카운트 증가. |

#### 5.7 `kernel/panic.c`, 5.8 `kernel_internal.h`, 5.9 `bsp/clint.c`, 5.10 `bsp/uart.c`, 5.11~12 `app/`

| **파일명** | **핵심 심볼** | **역할 요약** |
| --- | --- | --- |
| **`kernel/panic.c`** | `kernel_panic` | 인터럽트 차단, 덤프 저장, 에러 출력 후 `wfi` 루프 진입. |
| **`kernel_internal.h`** | `enter_critical` / `exit_critical` | 태스크 전용 중첩 카운터 관리 및 배리어 처리. |
| **`bsp/clint.c`** | `clint_get_mtime` / `clint_set_mtimecmp` / `clint_set_next_timer` | 64비트 원자적 접근 및 오버런 방어 타이머 설정. |
| **`bsp/uart.c`** | `uart_init` / `uart_putc` / `uart_puts` | 상한(100,000회) 폴링 기반 UART 입출력. |
| **`app/main.c` & `tasks.c`** | `main` / `tasks_init` / `task_a` / `task_b` | 시스템 초기화 및 태스크 실행 루프. |

## 6. 메모리 레이아웃 및 자료구조 명세

### 6.1 128바이트 문맥 보존 스택 프레임 (`kernel/context.S`)

```
[High Address: 태스크 스택 상단]
  sp + 0x7C : mstatus (CSR 0x300, 초기값 0x0000_1880 -> MPP=M, MPIE=1)
  sp + 0x78 : mepc    (CSR 0x341, 초기값 task_func 진입 주소)
  sp + 0x74 ~ 0x04 : x31 (t6) ~ x3 (gp) 범용 레지스터 29개
  sp + 0x00 : x1  (ra, 초기값: (uint32_t)task_return_guard)
[Current SP : TCB->sp가 가리키는 지점 (총 32 Words = 128 Bytes)]
  ... (런타임 가용 스택 영역) ...
[stack_base + 144B : mscratch에 장전되는 최소 스택 헤드룸 한계선 (16B 카나리 + 128B 프레임)]
  ...
[stack_base : 스택 버퍼 최하단 (16바이트 카나리 가드 영역)]
  base + 0x0C ~ 0x00 : 0x5A5A5A5A (Canary Word 3 ~ Word 0)
[Low Address]
```

### 6.2 커널 핵심 구조체 및 인라인 배리어 C 정의 (`kernel_internal.h`, `prio_rtos.h`)

```
#ifndef KERNEL_INTERNAL_H
#define KERNEL_INTERNAL_H

#include <stdint.h>
#include <stddef.h>

#define OS_MAX_TASKS         32U
#define OS_MAX_MUTEX_NESTING 4U

_Static_assert(OS_MAX_TASKS == 32U, "OS_MAX_TASKS must be 32 for Guard Bit 31");

typedef enum {
    TASK_CREATE_OK                = 0,
    TASK_CREATE_ERR_INVALID_PARAM = -1
} TaskCreateResult_t;

typedef enum {
    SEM_OK                = 0,
    SEM_ERR_INVALID_PARAM = -1
} SemaphoreResult_t;

/* 1. IPCP 뮤텍스 구조체 (8 Bytes) */
typedef struct mutex {
    uint8_t       ceiling_prio; /* 0x00 (1B): 고정 천장 우선순위 (0~30) */
    uint8_t       saved_prio;   /* 0x01 (1B): 직전 유효 우선순위 저장 */
    uint8_t       locked;       /* 0x02 (1B): 점유 상태 플래그 (0: Free, 1: Locked) */
    uint8_t       padding;      /* 0x03 (1B): 4바이트 정렬 패딩 */
    struct mutex *prev_mutex;   /* 0x04 (4B): 중첩 락 LIFO 링크 포인터 */
} mutex_t;

_Static_assert(sizeof(mutex_t) == 8U, "mutex_t size must be 8 bytes");

/* 2. 태스크 상태 열거형 */
typedef enum {
    TASK_STATE_UNUSED  = 0,
    TASK_STATE_READY   = 1,
    TASK_STATE_RUNNING = 2,
    TASK_STATE_BLOCKED = 3
} TaskState_t;

/* 3. 태스크 제어 블록 구조체 (24 Bytes) */
typedef struct {
    uint32_t     *sp;               /* 0x00 (4B): 스택 포인터 */
    mutex_t      *held_mutex;       /* 0x04 (4B): 점유 중인 최상단 뮤텍스 포인터 */
    uint32_t      stack_size_bytes; /* 0x08 (4B): 할당된 스택 버퍼 크기 */
    uint32_t      reserved;         /* 0x0C (4B): 24B 정렬 예약 필드 */
    uint8_t      *stack_base;       /* 0x10 (4B): 스택 버퍼 최하단 주소 */
    uint8_t       priority;         /* 0x14 (1B): 현재 동적 유효 우선순위 (0~31) */
    uint8_t       base_priority;    /* 0x15 (1B): 고유 기본 우선순위 (0~31) */
    uint8_t       task_id;          /* 0x16 (1B): 태스크 고유 슬롯 ID */
    uint8_t       state;            /* 0x17 (1B): 태스크 상태 (TaskState_t) */
} TCB_t;

_Static_assert(sizeof(TCB_t) == 24U, "TCB_t size must be 24 bytes");
_Static_assert(offsetof(TCB_t, sp) == 0U, "TCB_t sp offset must be 0");
_Static_assert(offsetof(TCB_t, stack_base) == 16U, "TCB_t stack_base offset must be 16");

/* 4. 카운팅 세마포어 구조체 (8 Bytes) */
typedef struct {
    uint32_t count;   /* 0x00 (4B): 가용 자원 수 */
    uint32_t waiters; /* 0x04 (4B): 대기 태스크 비트맵 */
} Semaphore_t;

_Static_assert(sizeof(Semaphore_t) == 8U, "Semaphore_t size must be 8 bytes");

/* 5. 크래시 덤프 구조체 (24 Bytes) */
typedef struct {
    uint32_t mcause;       /* 0x00 (4B): 트랩 원인 레지스터 */
    uint32_t mepc;         /* 0x04 (4B): 트랩 발생 PC */
    uint32_t mtval;        /* 0x08 (4B): 트랩 부가 정보 */
    uint32_t sp;           /* 0x0C (4B): 트랩 당시 스택 포인터 */
    uint32_t task_id;      /* 0x10 (4B): 실행 중이던 태스크 ID */
    uint32_t reserved_pad; /* 0x14 (4B): 패딩 */
} CrashDump_t;

_Static_assert(sizeof(CrashDump_t) == 24U, "CrashDump_t size must be 24 bytes");

__attribute__((noreturn)) void kernel_panic(const char *msg);

extern volatile uint32_t g_critical_nesting_count;
extern volatile uint32_t g_in_trap_context;

static inline void assert_isr_context(void){
    uint32_t mstatus_val;
    __asm__ volatile("csrr %0, mstatus" : "=r"(mstatus_val));
    if (((mstatus_val & 0x8U) != 0U) || (g_in_trap_context != 1U)) {
        kernel_panic("ERR_ILLEGAL_ISR_STATE");
    }
}

static inline void assert_task_context(void){
    if (g_in_trap_context != 0U) {
        kernel_panic("ERR_ILLEGAL_ISR_STATE");
    }
}

static inline void enter_critical(void){
    assert_task_context();
    __asm__ volatile("csrci mstatus, 8" ::: "memory");
    __asm__ volatile("" ::: "memory");
    if (g_critical_nesting_count == 0xFFFFFFFFU) {
        kernel_panic("ERR_CRIT_OVERFLOW");
    }
    g_critical_nesting_count++;
}

static inline void exit_critical(void){
    assert_task_context();
    if (g_critical_nesting_count == 0U) {
        kernel_panic("ERR_CRIT_UNDERFLOW");
    }
    g_critical_nesting_count--;
    if (g_critical_nesting_count == 0U) {
        __asm__ volatile("" ::: "memory");
        __asm__ volatile("fence rw, rw" ::: "memory");
        __asm__ volatile("csrsi mstatus, 8" ::: "memory");
    }
}

#endif /* KERNEL_INTERNAL_H */
```

### 6.3 커널 BSS 메모리 레지스트리 및 정적 RAM 집계 (3,004 Bytes)

| **심볼 식별자** | **자료형** | **정의 파일** | **크기 (Bytes)** | **역할** |
| --- | --- | --- | --- | --- |
| `g_tcbs[32]` | `TCB_t[32]` | `kernel/sched.c` | 768 B | 32개 태스크 제어 블록 배열 |
| `g_sleep_remaining_ticks[32]` | `volatile uint32_t[32]` | `kernel/sched.c` | 128 B | 태스크별 잔여 슬립 틱 배열 |
| `g_crash_dump` | `volatile CrashDump_t` | `kernel/panic.c` | 24 B | 트랩 레지스터 덤프 구조체 |
| `g_ready_bitmap` | `volatile uint32_t` | `kernel/sched.c` | 4 B | 준비 상태 태스크 비트마스크 |
| `g_sleep_bitmap` | `volatile uint32_t` | `kernel/sched.c` | 4 B | 슬립 상태 태스크 비트마스크 |
| `g_system_ticks` | `volatile uint64_t` | `kernel/sched.c` | 8 B | 64비트 시스템 틱 카운터 |
| `g_last_mtimecmp` | `volatile uint64_t` | `bsp/clint.c` | 8 B | 직전 타이머 비교값 레지스터 |
| `g_current_tcb` | `TCB_t*` | `kernel/sched.c` | 4 B | 현재 실행 중인 태스크 TCB 포인터 |
| `g_critical_nesting_count` | `volatile uint32_t` | `kernel/sched.c` | 4 B | 임계 구역 중첩 카운터 |
| `g_in_trap_context` | `volatile uint32_t` | `kernel/trap.c` | 4 B | 트랩 실행 상태 플래그 (0: Task, 1: Trap) |
| `idle_stack[1024]` | `uint8_t[1024]` | `kernel/sched.c` | 1,024 B | IDLE 태스크 스택 |
| `kernel_sys_stack[1024]` | `uint8_t[1024]` | `linker.ld` | 1,024 B | 부트 및 C 트랩 핸들러 시스템 스택 |
- **스칼라 상태 변수 합계:** 4 + 4 + 8 + 8 + 4 + 4 + 4 = 36 Bytes
- **제어 구조체 및 배열 합계:** 768 + 128 + 24 = 920 Bytes
- **스택 제외 커널 메모리 합계:** 36 + 920 = 956 Bytes
- **시스템 스택 합계:** 1,024 + 1,024 = 2,048 Bytes
- **시스템 정적 RAM 총합:** 956 + 2,048 = **3,004 Bytes**

### 6.4 서브시스템 구현 코드

#### 6.4.1 `kernel/sched.c`

C

```
#include "kernel_internal.h"
#include "prio_rtos.h"
#include "bsp/clint.h"

TCB_t             g_tcbs[OS_MAX_TASKS];
volatile uint32_t g_sleep_remaining_ticks[OS_MAX_TASKS];
volatile uint32_t g_ready_bitmap = 0U;
volatile uint32_t g_sleep_bitmap = 0U;
volatile uint64_t g_system_ticks = 0ULL;
TCB_t            *g_current_tcb  = NULL;
volatile uint32_t g_critical_nesting_count = 0U;
uint8_t           idle_stack[1024] __attribute__((aligned(16)));

static const uint8_t DeBruijn_LSB_LUT[32] = {
    0,  1, 28,  2, 29, 14, 24,  3,
   30, 22, 20, 15, 25, 17,  4,  8,
   31, 27, 13, 23, 21, 19, 16,  7,
   26, 12, 18,  6, 11,  5, 10,  9
};

uint8_t sched_highest_priority_slot(uint32_t task_bitmap){
    uint32_t safe_bm = task_bitmap | (1U << 31);
    uint32_t v       = safe_bm & (0U - safe_bm);
    uint32_t index   = (uint32_t)(v * 0x077CB531U) >> 27;
    return DeBruijn_LSB_LUT[index];
}

TCB_t* sched_select_next_task(void){
    uint8_t next_slot = sched_highest_priority_slot(g_ready_bitmap);
    g_ready_bitmap &= ~(1U << next_slot);
    TCB_t *next_tcb = &g_tcbs[next_slot];
    next_tcb->state = TASK_STATE_RUNNING;
    return next_tcb;
}

void timer_tick_step(void){
    clint_set_next_timer();
    g_system_ticks++;

    uint32_t sleep_bm = g_sleep_bitmap;
    if (sleep_bm != 0U) {
        for (uint8_t tid = 0U; tid < 31U; tid++) {
            uint32_t mask = (1U << tid);
            if ((sleep_bm & mask) != 0U) {
                if (g_sleep_remaining_ticks[tid] > 0U) {
                    g_sleep_remaining_ticks[tid]--;
                }
                if (g_sleep_remaining_ticks[tid] == 0U) {
                    g_sleep_bitmap &= ~mask;
                    g_tcbs[tid].state = TASK_STATE_READY;
                    g_ready_bitmap |= mask;
                }
            }
        }
    }

    uint8_t candidate = sched_highest_priority_slot(g_ready_bitmap);
    if (candidate < g_current_tcb->priority && g_current_tcb->held_mutex == NULL) {
        clint_send_software_interrupt();
    }
}

uint64_t get_tick_count(void){
    assert_task_context();
    enter_critical();
    uint64_t ticks = g_system_ticks;
    exit_critical();
    return ticks;
}

uint64_t get_tick_count_from_isr(void){
    assert_isr_context();
    return g_system_ticks;
}

void task_yield(void){
    assert_task_context();
    if (g_current_tcb->held_mutex != NULL || g_current_tcb->task_id == 31U) {
        kernel_panic("ERR_ILLEGAL_YIELD");
    }
    enter_critical();
    clint_send_software_interrupt();
    exit_critical();
}

void task_sleep_ticks(uint32_t ticks){
    assert_task_context();
    if (g_current_tcb->held_mutex != NULL || g_current_tcb->task_id == 31U) {
        kernel_panic("ERR_ILLEGAL_SLEEP");
    }
    if (ticks == 0U) {
        task_yield();
        return;
    }
    enter_critical();
    uint8_t tid = g_current_tcb->task_id;
    g_sleep_remaining_ticks[tid] = ticks;
    g_sleep_bitmap |= (1U << tid);
    g_current_tcb->state = TASK_STATE_BLOCKED;
    clint_send_software_interrupt();
    exit_critical();
}
```

#### 6.4.2 `kernel/trap.c`

C

```
#include "kernel_internal.h"
#include "prio_rtos.h"
#include "bsp/clint.h"

extern TCB_t *g_current_tcb;
extern volatile uint32_t g_ready_bitmap;
void timer_tick_step(void);
TCB_t* sched_select_next_task(void);
__attribute__((noreturn)) void unhandled_trap(void);

volatile uint32_t g_in_trap_context = 0U;

void trap_handler(void){
    g_in_trap_context = 1U;

    uint32_t mcause_val;
    __asm__ volatile("csrr %0, mcause" : "=r"(mcause_val));

    /* Cause 7: Machine Timer Interrupt */
    if (mcause_val == 0x80000007U) {
        timer_tick_step();
        g_in_trap_context = 0U;
        return;
    }

    /* Cause 3: Machine Software Interrupt */
    if (mcause_val == 0x80000003U) {
        clint_clear_software_interrupt();

        uint32_t *canary = (uint32_t *)g_current_tcb->stack_base;
        if (canary[0] != 0x5A5A5A5AU || canary[1] != 0x5A5A5A5AU ||
            canary[2] != 0x5A5A5A5AU || canary[3] != 0x5A5A5A5AU) {
            kernel_panic("ERR_STACK_OVERFLOW");
        }

        if (g_current_tcb->held_mutex != NULL) {
            g_in_trap_context = 0U;
            return;
        }

        if (g_current_tcb->state == TASK_STATE_RUNNING) {
            g_current_tcb->state = TASK_STATE_READY;
            g_ready_bitmap |= (1U << g_current_tcb->task_id);
        }

        g_current_tcb = sched_select_next_task();
        g_in_trap_context = 0U;
        return;
    }

    unhandled_trap();
}
```

#### 6.4.3 `kernel/mutex.c`

C

```
#include "kernel_internal.h"
#include "prio_rtos.h"
#include "bsp/clint.h"

extern TCB_t *g_current_tcb;
extern volatile uint32_t g_ready_bitmap;
uint8_t sched_highest_priority_slot(uint32_t task_bitmap);

void mutex_init(mutex_t *m, uint8_t ceiling_prio){
    if (m == NULL || ceiling_prio >= 31U) {
        kernel_panic("ERR_MUTEX_INIT_PARAM");
    }
    m->ceiling_prio = ceiling_prio;
    m->saved_prio   = 0U;
    m->locked       = 0U;
    m->padding      = 0U;
    m->prev_mutex   = NULL;
}

void mutex_lock(mutex_t *m){
    assert_task_context();
    enter_critical();
    if (m == NULL || g_current_tcb->task_id == 31U) {
        kernel_panic("ERR_MUTEX_INVALID_CALL");
    }
    if (m->locked != 0U) {
        kernel_panic("ERR_IPCP_CONTENTION");
    }
    if (g_current_tcb->base_priority < m->ceiling_prio) {
        kernel_panic("ERR_IPCP_CEILING_VIOLATION");
    }

    uint32_t nest_cnt = 0U;
    mutex_t *curr_m   = g_current_tcb->held_mutex;
    while (curr_m != NULL && nest_cnt < OS_MAX_MUTEX_NESTING) {
        nest_cnt++;
        curr_m = curr_m->prev_mutex;
    }
    if (curr_m != NULL || nest_cnt >= OS_MAX_MUTEX_NESTING) {
        kernel_panic("ERR_IPCP_NESTING");
    }

    m->locked                 = 1U;
    m->saved_prio             = g_current_tcb->priority;
    m->prev_mutex             = g_current_tcb->held_mutex;
    g_current_tcb->held_mutex = m;

    if (m->ceiling_prio < g_current_tcb->priority) {
        g_current_tcb->priority = m->ceiling_prio;
    }
    exit_critical();
}

void mutex_unlock(mutex_t *m){
    assert_task_context();
    enter_critical();
    if (m == NULL || m->locked == 0U) {
        kernel_panic("ERR_IPCP_UNLOCK_UNLOCKED");
    }
    if (g_current_tcb->held_mutex != m) {
        kernel_panic("ERR_IPCP_LIFO_VIOLATION");
    }

    g_current_tcb->held_mutex = m->prev_mutex;
    m->prev_mutex             = NULL;
    m->locked                 = 0U;
    g_current_tcb->priority   = m->saved_prio;

    if (g_current_tcb->held_mutex == NULL) {
        uint8_t candidate = sched_highest_priority_slot(g_ready_bitmap);
        if (candidate < g_current_tcb->priority) {
            clint_send_software_interrupt();
        }
    }
    exit_critical();
}
```

#### 6.4.4 `kernel/semaphore.c`

C

```
#include "kernel_internal.h"
#include "prio_rtos.h"
#include "bsp/clint.h"

extern TCB_t  g_tcbs[OS_MAX_TASKS];
extern TCB_t *g_current_tcb;
extern volatile uint32_t g_ready_bitmap;

static const uint8_t Sem_DeBruijn_LSB_LUT[32] = {
    0,  1, 28,  2, 29, 14, 24,  3,
   30, 22, 20, 15, 25, 17,  4,  8,
   31, 27, 13, 23, 21, 19, 16,  7,
   26, 12, 18,  6, 11,  5, 10,  9
};

SemaphoreResult_t semaphore_init(Semaphore_t *sem, uint32_t initial_count){
    if (sem == NULL) {
        return SEM_ERR_INVALID_PARAM;
    }
    sem->count   = initial_count;
    sem->waiters = 0U;
    return SEM_OK;
}

SemaphoreResult_t semaphore_wait(Semaphore_t *sem){
    assert_task_context();
    if (sem == NULL) {
        return SEM_ERR_INVALID_PARAM;
    }
    if (g_current_tcb->held_mutex != NULL || g_current_tcb->task_id == 31U) {
        kernel_panic("ERR_ILLEGAL_BLOCKING");
    }

    enter_critical();
    if (sem->count > 0U) {
        sem->count--;
        exit_critical();
        return SEM_OK;
    }

    sem->waiters |= (1U << g_current_tcb->task_id);
    g_current_tcb->state = TASK_STATE_BLOCKED;
    clint_send_software_interrupt();
    exit_critical();
    return SEM_OK;
}

static inline void semaphore_signal_core(Semaphore_t *sem){
    if (sem->waiters == 0U) {
        sem->count++;
        return;
    }
    uint32_t v           = sem->waiters & (0U - sem->waiters);
    uint32_t index       = (uint32_t)(v * 0x077CB531U) >> 27;
    uint8_t  wakeup_slot = Sem_DeBruijn_LSB_LUT[index];

    sem->waiters &= ~(1U << wakeup_slot);
    g_tcbs[wakeup_slot].state = TASK_STATE_READY;
    g_ready_bitmap |= (1U << wakeup_slot);

    if (wakeup_slot < g_current_tcb->priority && g_current_tcb->held_mutex == NULL) {
        clint_send_software_interrupt();
    }
}

SemaphoreResult_t semaphore_signal(Semaphore_t *sem){
    assert_task_context();
    if (sem == NULL) {
        return SEM_ERR_INVALID_PARAM;
    }
    enter_critical();
    semaphore_signal_core(sem);
    exit_critical();
    return SEM_OK;
}

SemaphoreResult_t semaphore_signal_from_isr(Semaphore_t *sem){
    assert_isr_context();
    if (sem == NULL) {
        return SEM_ERR_INVALID_PARAM;
    }
    semaphore_signal_core(sem);
    return SEM_OK;
}
```

#### 6.4.5 `bsp/clint.c`

C

```
#include <stdint.h>
#include "bsp/clint.h"

#define CLINT_MSIP_REG         (*(volatile uint32_t *)0x02000000UL)
#define CLINT_MTIMECMP_LOW_REG (*(volatile uint32_t *)0x02004000UL)
#define CLINT_MTIMECMP_HI_REG  (*(volatile uint32_t *)0x02004004UL)
#define CLINT_MTIME_LOW_REG    (*(volatile uint32_t *)0x0200BFF8UL)
#define CLINT_MTIME_HI_REG     (*(volatile uint32_t *)0x0200BFFCUL)

volatile uint64_t g_last_mtimecmp = 0ULL;

uint64_t clint_get_mtime(void){
    uint32_t hi1, lo, hi2;
    do {
        hi1 = CLINT_MTIME_HI_REG;
        lo  = CLINT_MTIME_LOW_REG;
        hi2 = CLINT_MTIME_HI_REG;
    } while (hi1 != hi2);
    return ((uint64_t)hi1 << 32) | (uint64_t)lo;
}

void clint_set_mtimecmp(uint64_t compare_value){
    CLINT_MTIMECMP_HI_REG  = 0xFFFFFFFFU;
    CLINT_MTIMECMP_LOW_REG = (uint32_t)(compare_value & 0xFFFFFFFFULL);
    CLINT_MTIMECMP_HI_REG  = (uint32_t)(compare_value >> 32);
}

void clint_set_next_timer(void){
    uint64_t now  = clint_get_mtime();
    uint64_t next = g_last_mtimecmp + TIMER_INTERVAL;
    if (now >= next) {
        g_last_mtimecmp = now + TIMER_INTERVAL;
    } else {
        g_last_mtimecmp = next;
    }
    clint_set_mtimecmp(g_last_mtimecmp);
}

void clint_send_software_interrupt(void){
    CLINT_MSIP_REG = 1U;
}

void clint_clear_software_interrupt(void){
    CLINT_MSIP_REG = 0U;
}
```

## 7. 정량적 검증 기준 (Acceptance Criteria)

- **측정 환경:**
    - Platform: QEMU RISC-V 32 virt (`qemu-system-riscv32 -M virt -cpu rv32 -icount shift=0,align=off`)
    - Toolchain: `riscv-none-elf-gcc -O2 -march=rv32imc_zicsr -mabi=ilp32`
    - Timebase: 10 MHz CLINT Clock (`TIMER_INTERVAL = 10000ULL`)
- **합격 기준:**
    1. **스케줄러 구조 상한 (REQ-SCH-001):** `sched_highest_priority_slot` 본체 명령어가 10개 이하이며 조건 분기문이 없을 것.
    2. **IPCP 뮤텍스 상한 (REQ-SYN-002):** `mutex_lock` 중첩 검사 루프가 최대 4회(`OS_MAX_MUTEX_NESTING`)로 제한될 것.
    3. **컨텍스트 격리 검증 (INV-CTX-005):** 태스크 문맥에서 `g_in_trap_context == 0`, 트랩 진입 후 `g_in_trap_context == 1`을 유지할 것. 크리티컬 섹션 내부에서 ISR API 호출 시 패닉이 발생할 것.
    4. **타이머 오차 및 오버런 복구 (INV-ATM-011):** 정상 동작 시 주기적 오차가 누적되지 않고, 강제 지연 주입 후 단 1회 처리 후 `now + TIMER_INTERVAL`로 재설정될 것.
    5. **메모리 예산 (INV-MEM-001):** 커널 코어 `.text` 4,096 Bytes 이하, 정적 RAM 3,004 Bytes, `.heap` 0 Byte일 것.

## 8. 요구사항 추적성 매트릭스 (Traceability Matrix)

| **요구사항 ID** | **SRS 명세 항목** | **대상 불변식** | **구현 심볼 / 파일** | **검증 단위 테스트 케이스** |
| --- | --- | --- | --- | --- |
| **REQ-SCH-001** | Branchless O(1) De Bruijn 스케줄러 | INV-ALG-002 | `kernel/sched.c:sched_highest_priority_slot` | `test_sched_debruijn_all_slots` |
| **REQ-SCH-002** | 1:1 직접 매핑 및 비선점 방어 | INV-SCH-003 | `kernel/trap.c:trap_handler` | `test_pending_msip_bypass_blocked_in_mutex` |
| **REQ-TSK-001** | 정적 태스크 등록 및 128B 프레임 빌드 | INV-CAP-006 | `kernel/sched.c:task_create` | `test_task_create_duplicate_panic` |
| **REQ-TSK-002** | 자발적 CPU 양보 및 뮤텍스 점유 검증 | INV-SYN-013 | `kernel/sched.c:task_yield` | `test_yield_with_mutex_panic` |
| **REQ-TSK-003** | 지정 틱 슬립 및 뮤텍스 점유 검증 | INV-SYN-013 | `kernel/sched.c:task_sleep_ticks` | `test_sleep_with_mutex_panic` |
| **REQ-TSK-004** | 유휴 태스크(슬롯 31) 상주 및 블로킹 금지 | INV-LFC-007 | `kernel/sched.c:idle_task` | `test_idle_fallback_on_empty_ready` |
| **REQ-TSK-005** | 커널 시동 및 최초 문맥 복원 | INV-STK-008 | `kernel/sched.c:priortos_start` | `test_cold_boot_first_switch` |
| **REQ-SYN-001** | 정적 IPCP 뮤텍스 초기화 | INV-MUT-010 | `kernel/mutex.c:mutex_init` | `test_mutex_static_init` |
| **REQ-SYN-002** | 바운디드 섀도잉 락 및 경합 패닉 | INV-MUT-010 | `kernel/mutex.c:mutex_lock` | `test_mutex_bounded_nesting_and_cycle_panic` |
| **REQ-SYN-003** | LIFO 역순 해제 검증 및 언락 선점 | INV-MUT-010 | `kernel/mutex.c:mutex_unlock` | `test_mutex_lifo_violation_and_deferred_preempt` |
| **REQ-SYN-004** | 세마포어 대기 및 뮤텍스 격리 검증 | INV-SYN-013 | `kernel/semaphore.c:semaphore_wait` | `test_sem_blocking_in_mutex_panic` |
| **REQ-SYN-005** | LSB 시그널 및 컨텍스트 단언 | INV-CTX-005 | `kernel/semaphore.c:semaphore_signal_from_isr` | `test_sem_isr_mie_and_flag_assertion` |
| **REQ-CRI-001** | 태스크 전용 중첩 임계 구역 진입 | INV-CRI-012 | `kernel/include/kernel_internal.h:enter_critical` | `test_critical_nesting_overflow` |
| **REQ-CRI-002** | 태스크 전용 중첩 임계 구역 탈출 | INV-CRI-012 | `kernel/include/kernel_internal.h:exit_critical` | `test_critical_nesting_underflow` |
| **REQ-TMR-001** | 64비트 시스템 틱 획득 (Task/ISR) | INV-CTX-005 | `kernel/sched.c:get_tick_count_from_isr` | `test_timer_atomic_tick_read` |
| **REQ-TMR-002** | 타이머 갱신 및 슬립 만료 처리 | INV-ATM-011 | `kernel/sched.c:timer_tick_step` | `test_timer_overrun_storm_prevention` |
| **REQ-DRV-001** | CLINT 판독 및 비교값 설정 | INV-ATM-011 | `bsp/clint.c:clint_get_mtime` | `test_clint_64bit_rollover_atomic` |
| **REQ-DRV-002** | 타임아웃(100,000회) UART 송신 | INV-ERR-009 | `bsp/uart.c:uart_putc` | `test_uart_tx_timeout_drop` |

## 9. Revision History

| **버전** | **개정 일자** | **개정 항목** | **수정 내용** |
| --- | --- | --- | --- |
| **v2.2.3-a** | 2026-10-30 | SRS 3.1, LLD 5.2 | `prio_context_switch` 폐기, `trap_entry`로 통합. |
| **v2.2.3-b** | 2026-10-30 | INV-CTX-005, REQ-CRI-001 | ISR 내부 `enter_critical` 호출 금지 규약 명시. |
| **v2.2.3-c** | 2026-10-30 | REQ-TMR-002, INV-ATM-011 | 1 ms 누적 장전 및 슬립 언더플로 방어 처리. |
| **v2.2.3-d** | 2026-10-30 | INV-STK-008, LLD 6.1 | `kernel_sys_stack`(1024B) 분리 및 `mscratch` 144B 카나리 경계 명시. |
| **v2.2.4** | 2026-10-30 | LLD 6.3 | BSS 메모리 레지스트리 수치 정리. |
| **v2.2.4-Final** | 2026-10-30 | 전 섹션 | Bottom-Half `held_mutex != NULL` 가드 추가, `g_ready_tcbs` 제거, `mutex_lock` 중첩 검사 상한 강제, `assert_isr_context()` 추가. |
| **v2.2.5-Final** | 2026-11-06 | INV-CTX-005, LLD 5.3, 6.2, 6.3, 6.4.2 | `volatile uint32_t g_in_trap_context` 선언 및 `trap_handler` 진입/탈출 시 토글 적용. `assert_isr_context()`에 플래그 검증 추가 및 `assert_task_context()` 신설. BSS 레지스트리 4B 추가(총 RAM 3,004 Bytes). |
