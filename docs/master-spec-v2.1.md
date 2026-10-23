# PrioRTOS 아키텍처 및 상세 설계 통합 명세서 (Final Master Baseline v2.1)

- **Project Name**: PrioRTOS (Deterministic Hard-RTOS for RISC-V Single-Core)
- **Architecture / ISA**: RV32I Base Integer Instruction Set (F/D Extension Disabled, No Zbb Dependency)
- **Target Hardware / Board**: Primary: RISC-V QEMU virt / Secondary (Porting): ESP32-C3 (via BSP Abstraction)
- **Interrupt Model**: Standard RISC-V CLINT (Core Local Interruptor)
- **Document Status**: Frozen (Baseline v2.1 - Code-Free Clean Architecture Spec with Concurrency Hardening)
- **Author / Maintainer**: 임준형

# PART I. 소프트웨어 요구사항 명세서 (SRS: System Requirements Specification)

> **[계층 선언]** PART I은 시스템 전체를 지배하는 최상위 기준이다. 본 파트는 "무엇을 보장해야 하는가"만 정의하며, 구체적인 디렉토리 매핑, C 자료구조 오프셋 및 모듈별 알고리즘은 PART II(LLD)에서 규정한다.

## 1. 개요 및 시스템 환경 제약 (System Constraints & Environment)

### 1.1. 하드웨어 플랫폼 규격 (HW Baseline - QEMU virt 기준)

- **CPU 및 ISA**:

- 코어: RISC-V 32-bit Single Hart (Hart 0 강제 고정)
- 확장 세트: RV32IMC
- 엔디안: Little-Endian
- 특권 모드: Single M-Mode Flat Model (커널 및 태스크 동일 M-Mode 동작, 특권 수준 전환 오버헤드 0)
- **물리 메모리 맵 / 기준 보드 QEMU virt (Physical Memory Map)**:

**영역 명칭**
**시작 물리 주소**
**크기 (Size)**
**속성**
**설명 및 하드웨어 역할**

**CLINT**
0x0200_0000
64 KB
RW (MMIO)
Core Local Interruptor (소프트웨어 및 타이머 인터럽트 제어)

**PLIC**
0x0C00_0000
64 MB
RW (MMIO)
Platform-Level Interrupt Controller (외부 인터럽트 제어)

**UART0**
0x1000_0000
256 B
RW (MMIO)
NS16550A 호환 디버그 시리얼 포트

**DRAM**
0x8000_0000
128 MB
RWX
커널 및 애플리케이션 전체 메모리 (코드, 상수, 전역 데이터, 정적 스택)

- **CLINT 상세 레지스터 오프셋 및 타임베이스 규격 (Hart 0 기준)**:

- `MSIP` (Cause 3 전용): 0x0200_0000 (Bit 0 제어, 1 기록 시 소프트웨어 인터럽트 펜딩)
- `MTIMECMP_LOW`: 0x0200_4000 / `MTIMECMP_HIGH`: 0x0200_4004
- `MTIME_LOW`: 0x0200_BFF8 / `MTIME_HIGH`: 0x0200_BFFC (하드웨어 클럭: 10 MHz 고정)
- 시스템 틱 주기: $1\text{ms Tick} = 10,000,000\text{ Hz} \times 0.001\text{s} = 10,000\text{ cycles}$
- **인터럽트 벡터 아키텍처 규약**:

- `mtvec` 설정: 반드시 Direct Mode (`MODE = 0x00`)로 고정하며, 베이스 주소는 `trap_entry` 레이블에 바인딩되고 RISC-V 특권 아키텍처 규격에 따라 최소 4바이트(워드) 정렬을 준수한다.
- 단일 하드웨어 진입점: 비동기 인터럽트 및 동기 예외를 포함한 모든 하드웨어 트랩은 원인(`mcause`)에 관계없이 `trap_entry` 단일 진입점으로 직행한다 (`MODE = 0x00`).
- 소프트웨어 트랩 디스패치 (`trap_handler` 내부 `mcause` 판독 분기):

- `Cause 7` (`0x80000007`, MTIP): Top-Half 틱 디스패처(Tick Dispatcher)로 분기.
- `Cause 3` (`0x80000003`, MSIP): Bottom-Half 문맥 교환기(Context Switcher)로 분기.
- 미인가 트랩 및 동기 예외: 진단 출력 없이 Fail-Stop 안전 동결 루틴(`wfi` 무한 루프) 또는 명시적 커널 패닉으로 분기.

## 2. 시스템 아키텍처 불변 원칙 (System Invariants)

아래 12대 불변 원칙은 시스템 전체에 적용되는 최상위 제약 조건이며, PART II(LLD)의 세부 설계나 구현은 이를 위반할 수 없다.

**ID**
**카테고리**
**불변식 명칭**
**핵심 제약 내용 (Specification)**
**검증 수단 (Verification)**
**방지 대상 결함 (Hazard)**

**INV-MEM-001**
메모리
Zero Dynamic Heap
런타임 힙 동적 할당 전면 금지, 모든 메모리 컴파일 타임 정적 배치
링커 스크립트 .heap=0 강제 및 .map 심볼 검사
외부 단편화, OOM, 가변 할당 지연

**INV-ALG-002**
복잡도
Strict O(1) Complexity
스케줄러 가변 루프 금지, De Bruijn MSB 및 간접 매핑 테이블 기반 Branchless 인덱싱 강제
역어셈블리 내 분기·역방향 점프 정적 검사
대기 태스크($N$) 비례 WCET 폭증

**INV-INT-003**
인터럽트
Deferred-Dispatch Trap
하드웨어 ISR 내 문맥 교환 금지, CLINT MSIP(Cause 3) 소프트웨어 트랩으로 디스패치 위임
정적 Call-Graph 검사, ISR 명령어 수 상한 검증
인터럽트 지연 폭증, CSR 복귀 상태 오염

**INV-TIM-004**
타이밍
Bounded Critical Section
커널 API 내부 전역 인터럽트 차단(MIE=0) 구간을 고정 사이클 상한으로 제한
Cycle-accurate 시뮬레이터 로그 분석 및 GPIO 펄스 실측
외부 고속 펄스/센서 유실 (Jitter)

**INV-CAP-005**
용량
Static Task Bounding
아키텍처 상한 $\le 32$, $1 \le \text{OS\_MAX\_TASKS} \le 32$ 정적 설정, 1 Priority = 1 Task 원칙 강제
`_Static_assert` 범위 단언, 정적 비트마스크 중복 검출
다단계 FIFO 큐 전락 및 비트맵 붕괴

**INV-LFC-006**
수명주기
No Runtime Task Deletion
커널 시동 후 동적 태스크 생성/종료/삭제 금지, 무한 루프 상주
공개 삭제 API 배제, 태스크 리턴 시 패닉 트랩 유도
자원 고아화, $O(N)$ 자원 회수 지터

**INV-STK-007**
스택
Static Stack & Canary
재귀·VLA 금지, 스택 최하단 `0x5A5A5A5A` ($\times 4$, 16B) 카나리 배치 및 문맥 교환 시 검사
`-fstack-usage` 정적 분석, 문맥 교환 시 런타임 무결성 검증
스택 오버플로로 인한 인접 메모리 침범

**INV-ERR-008**
에러 처리
Bounded HW Polling
레지스터 무한 폴링 루프 금지, 고정 상한 카운터 초과 시 즉시 에러 반환
정적 AST 루프 검출, 무응답 하드웨어 결함 주입 테스트
외부 페리페럴 고장 시 CPU 프리징(Hang)

**INV-MUT-009**
동기화
IPCP (Priority Ceiling)
뮤텍스 획득 시 Ceiling 우선순위로 즉시 승격 및 간접 매핑 갱신, 해제 시 즉시 복원
L-M-H 우선순위 역전 결함 주입 단위 테스트
우선순위 역전 현상, 상호 대기 데드락

**INV-ATM-010**
드라이버
CLINT 64-bit Atomic
32비트 버스 상에서 64비트 타이머 레지스터 원자적 접근 시퀀스 준수
역어셈블리 명령어 시퀀스 검사, 32비트 롤오버 경계값 주입
비원자적 갱신으로 인한 가짜 타이머 인터럽트

**INV-CRI-011**
동시성
Critical Section Nesting
중첩 진입 카운터를 유지하고 탈출 시 메모리/명령어 배리어를 거쳐 카운터가 0일 때만 MIE=1 복원
카운터 언더플로 패닉 트랩, 정적 진입/탈출 쌍 검증
임계 구역 조기 활성화로 인한 원자성 파괴, 메모리 재정렬 버그

**INV-SYN-012**
동기화
Primitive Isolation
IPCP 뮤텍스를 점유하여 우선순위가 승격된 태스크는 어떠한 경우에도 세마포어 대기(`semaphore_wait`) 상태로 전이될 수 없으며, 시도 시 `kernel_panic`을 격발함
결함 주입 단위 테스트 (`mutex_lock` 후 `semaphore_wait` 호출 검증)
천장 오염, 간접 데드락, 우선순위 역전 복합 결함

```
불변 원칙의 연쇄 의존성 구조 (Dependency Chain)
[INV-MEM-001 (동적 힙 0바이트)] ──► [INV-CAP-005 (OS_MAX_TASKS <= 32 정적 바운딩)] ──► [INV-ALG-002 (O(1) Branchless 스케줄러)]
             │                                        │                                         │
             ▼                                        ▼                                         ▼
[INV-LFC-006 (태스크 삭제 금지)]          [INV-STK-007 (스택 정적 바운딩/카나리)]     [INV-TIM-004 (임계 구역 고정 사이클 제한)]
                                                                                                │
             ┌──────────────────────────────────────────────────────────────────────────────────┘
             ▼
[INV-INT-003 (DPC 지연 디스패치)] ──► [INV-CRI-011 (중첩 임계 구역 보호 및 배리어)]
             │
             ├─► [INV-MUT-009 (IPCP 뮤텍스)]: 우선순위 역전 및 데드락 원천 차단
             │         │
             │         └─► [INV-SYN-012 (동기화 프리미티브 격리)]: 뮤텍스 점유 중 세마포어 블록 원천 방지
             ├─► [INV-ATM-010 (CLINT 원자적 조작)]: 64비트 타임베이스 무결성 사수
             └─► [INV-ERR-008 (Bounded Polling)]: 외부 하드웨어 고장 시 커널 생존성 사수
```

## 3. 커널 서브시스템 기능 요구사항 (Subsystem Functional Requirements)

```
3. 커널 서브시스템 기능 요구사항
 ├── 3.1. 스케줄러 및 인터럽트 동작 정책
 │    ├── 선점형 고정 우선순위 정책 (1 Priority = 1 Task, 간접 매핑)
 │    ├── REQ-SCH-001 (De Bruijn MSB O(1) 간접 매핑 스케줄러)
 │    ├── REQ-SCH-002 (32개 우선순위 레벨 영역 분할 설계)
 │    └── 이원화 인터럽트 (Top-Half Cause 7 / Bottom-Half Cause 3)
 ├── 3.2. 부팅 시퀀스 및 예외 처리 정책
 │    ├── 결정론적 부팅 (Step 0 ~ Step 5)
 │    ├── 트랩 디스패치 (Cause 3, Cause 7)
 │    ├── 미인가 트랩 격리 (wfi 동결)
 │    └── 명시적 패닉 (kernel_panic)
 ├── 3.3. 태스크 제어 서브시스템 (Lifecycle & Context)
 │    ├── REQ-TSK-001 (task_create - 스택 빌드 및 매핑 등록)
 │    ├── REQ-TSK-002 (task_yield - 자발적 양보)
 │    ├── REQ-TSK-003 (task_sleep_ticks - 슬립 전이)
 │    ├── REQ-TSK-004 (idle_task - 저전력 유휴 루프)
 │    └── REQ-TSK-005 (priortos_start - 커널 시동 진입점)
 ├── 3.4. 상호 배제 및 동기화 서브시스템 (Synchronization - IPCP & Semaphore)
 │    ├── REQ-SYN-001 (mutex_init)
 │    ├── REQ-SYN-002 (mutex_lock - 경합 배제, 즉시 승격 및 매핑 이전)
 │    ├── REQ-SYN-003 (mutex_unlock - 복원 및 MSIP 트리거)
 │    ├── REQ-SYN-004 (semaphore_wait - 블록 전이 및 뮤텍스 점유 상호작용 검증)
 │    └── REQ-SYN-005 (semaphore_signal - 대기자 복구 및 레디 전이)
 ├── 3.5. 임계 구역 관리 서브시스템 (Critical Section)
 │    ├── REQ-CRI-001 (enter_critical)
 │    └── REQ-CRI-002 (exit_critical - 명령어/메모리 배리어 및 원자적 탈출)
 └── 3.6. 타이머 및 시간 서브시스템 (Time)
      ├── REQ-TMR-001 (get_tick_count - 원자적 틱 조회)
      └── REQ-TMR-002 (timer_tick_step)
```

### 3.1. 스케줄러 및 인터럽트 동작 정책

- **선점형 고정 우선순위 (Preemptive Fixed-Priority)**:

- 우선순위 0(Idle 전용)부터 31(최고 우선순위)까지 총 32개의 고유 레벨을 운용하며, 1 Priority = 1 Task 원칙을 적용한다.
- 숫자가 클수록 높은 우선순위를 가지며(MSB 우선 체계), 비선점 임계 구역을 제외한 전 구간에서 상위 우선순위 태스크가 준비(Ready) 상태로 전이되면 선점을 실행한다.
- **우선순위-슬롯 분리 및 간접 매핑(Indirect Mapping)**: `g_ready_bitmap`의 각 비트 $p$는 태스크 슬롯 번호가 아닌 현재 활성화된 우선순위 레벨($p$)을 나타낸다. IPCP 락 획득 등으로 인한 동적 우선순위 승격 시에도 TCB 데이터 복제를 방지하기 위해 32바이트 정적 간접 매핑 테이블(`g_prio_to_slot[32]`)을 운용하여 우선순위 $p$로부터 실제 TCB 슬롯 인덱스를 $O(1)$로 역참조한다.
- **De Bruijn MSB** $O(1)$ **간접 매핑 스케줄러 (REQ-SCH-001)**:

- 분기문(Branch)과 가변 루프를 일절 배제한 엄격한 상수 시간($O(1)$) 산출 시퀀스를 따른다.
- **1단계 (Bit Smearing)**: `g_ready_bitmap`에서 최상위 세트 비트(MSB, 최고 우선순위) 아래의 모든 하위 비트를 1로 전파한다.

$\begin{aligned}

b_0 &= \text{g\_ready\_bitmap} \\

b_1 &= b_0 \mid (b_0 \gg 1) \\

b_2 &= b_1 \mid (b_1 \gg 2) \\

b_3 &= b_2 \mid (b_2 \gg 4) \\

b_4 &= b_3 \mid (b_3 \gg 8) \\

\text{smear} &= b_4 \mid (b_4 \gg 16)

\end{aligned}$
- **2단계 (MSB 고립)**: 전파된 마스크에서 우측으로 1비트 시프트한 값을 감산하여 최고 우선순위 1비트만 $2^{\text{prio}}$ 형태의 $2$의 거듭제곱 값($v$)으로 고립시킨다.

$v = \text{smear} - (\text{smear} \gg 1)$
- **3단계 (De Bruijn 곱셈 해싱 및 인덱스 산출)**: 고립된 비트 $v$에 32비트 De Bruijn 승수(`0x077CB531U`)를 곱하고 상위 5비트를 추출하여 32바이트 룩업 테이블(`DeBruijn_LUT`)로부터 최고 우선순위 번호를 획득한다.

$\text{index} = (v \times \text{0x077CB531U}) \gg 27$

$\text{highest\_prio} = \text{DeBruijn\_LUT}[\text{index}]$
- **4단계 (간접 슬롯 매핑 및 TCB 확정)**: 산출된 `highest_prio`($0 \sim 31$)를 인덱스로 간접 매핑 테이블을 조회하여 실제 태스크 슬롯 번호(`slot`)를 즉시 획득하고 `g_tcbs` 배열을 인덱싱한다.

$\text{slot} = \text{g\_prio\_to\_slot}[\text{highest\_prio}]$

$\text{g\_current\_tcb} = \&\text{g\_tcbs}[\text{slot}]$
- Idle 태스크(우선순위 0)가 항시 1로 상주하므로 비트맵은 0이 될 수 없으며, 준비 상태 태스크 수에 관계없이 **14 ~ 18 CPU 사이클 이내에 결정론적으로 대상 태스크를 확정**한다.
- **이원화된 인터럽트 처리 (Top-Half / Bottom-Half)**:

- **타이머 ISR (Cause 7, Top-Half)**:

- 64비트 `mtimecmp` 원자적 갱신, 전역 시스템 틱 카운터 증가(`g_system_ticks++`).
- 슬립 비트맵(`g_sleep_bitmap`)이 0이 아닐 경우 활성 슬립 슬롯(`slot`)의 잔여 틱(`g_sleep_remaining_ticks[slot]`)을 감산하고, 만료 태스크를 `g_ready_bitmap`으로 복귀시키며 해당 우선순위의 `g_prio_to_slot` 매핑을 유효화한다.
- 선점 판정: REQ-SCH-001 알고리즘(1~3단계)으로 산출된 준비 큐 최고 우선순위가 현재 실행 중인 태스크의 동적 유효 우선순위보다 높은 경우(`highest_prio > g_current_tcb->priority`), CLINT의 `MSIP` 비트에 1을 기록하여 지연된 문맥 교환(DPC)을 예약하고 `mret`으로 탈출한다 ($WCET \le 150\text{ cycles}$).
- 타이머 ISR 내부에서의 직접적인 UART 폴링 출력은 지터 방지 및 $WCET$ 보장을 위해 엄격히 금지된다.
- **소프트웨어 ISR (Cause 3, Bottom-Half DPC)**:

- CLINT `MSIP` 비트를 0으로 즉시 클리어한다.
- 현재 태스크 스택에 128바이트 트랩 문맥을 보존하고 TCB 오프셋 0x00에 `sp`를 갱신한다.
- 스택 하단 16바이트 카나리 가드 워드(`0x5A5A5A5A` $\times 4$)의 훼손 여부를 검증한다.
- REQ-SCH-001 De Bruijn MSB 및 간접 매핑 알고리즘(1~4단계)을 실행하여 최고 우선순위 태스크 슬롯을 확정하고 `g_current_tcb`를 교체한다.
- 교체된 태스크의 스택에서 레지스터 32단어(128 Bytes)를 복원한 뒤 `mret`을 실행하여 태스크 제어권을 전환한다.
- **32개 우선순위 레벨(0 ~ 31) 영역 분할 설계 (REQ-SCH-002)**:

```
[Highest: 31] ──────────────────────────────────────────────────────────
              ▲  [도메인 4] Safety & Critical System Tasks (28 ~ 31)
              │    - 하드웨어 긴급 방어, 전원 이상/페일세이프, 워치독 킥
              ├──────────────────────────────────────────────────────────
              │  [도메인 3] Hard Real-Time Periodic Control Tasks (16 ~ 27)
              │    - 모터 제어(FOC), 센서 융합 루프(1kHz 이상 초고속 주기 제어)
              ├──────────────────────────────────────────────────────────
              │  [도메인 2] Soft Real-Time & Communication Tasks (4 ~ 15)
              │    - CAN/UART 통신 패킷 처리, 외부 프로토콜 스택, 로깅 파싱
              ├──────────────────────────────────────────────────────────
              ▼  [도메인 1] Background & Diagnostic Tasks (1 ~ 3)
                   - 시스템 헬스 체크, 플래시 마모도 검사, 유휴 통계
[Lowest: 0]   ──────────────────────────────────────────────────────────
                 [도메인 0] IDLE 태스크 고정 (0)
```

**우선순위 범위**
**도메인 명칭**
**주 실행 주기 / 트리거**
**특성 및 허용 연산 규칙**
**대표 예시 태스크**

**31 ~ 28** (4레벨)
**Safety & Fault Handling** (안전 및 긴급 차단)
이벤트성 (Fault 발생 즉시)
• 절대 블록(`task_sleep_ticks`) 금지

• IPCP 뮤텍스 소유 태스크보다 무조건 상위

• 실행 시간 상한 $WCET \le 50\,\mu\text{s}$ | 비상 정지(E-Stop), 과전압/과전류 차단, 외부 하드웨어 고장 페일세이프 |

| **27 ~ 16** (12레벨) | **Hard Real-Time Control** (고정 주기 정밀 제어) | 1ms ~ 10ms 고정 주기 (하드웨어 틱 동기) | • IPCP 뮤텍스를 공유하여 자원 접근 가능

• 지터(Jitter) 허용치 $\le 10\,\mu\text{s}$

• 연산 시간 엄격히 바운딩 | 모터 정밀 각도 제어, 인버터 제어, IMU/센서 칼만 필터링 |

| **15 ~ 4** (12레벨) | **Soft Real-Time & Comm** (통신 및 데이터 처리) | 10ms ~ 100ms 또는 UART/통신 인터럽트 | • 세마포어 기반 시그널링 대기(wait) 빈번

• 1~2틱 정도의 지연(Jitter) 허용

• 출력 버퍼 및 링 버퍼 소비 | UART 커맨드 파서, 패킷 CRC 검증, 센서 수치 텔레메트리 패킹 |

| **3 ~ 1** (3레벨) | **Background & Health** (유휴 진단 및 통계) | 500ms 이상 비동기 | • CPU 여유 시간(Slack Time)에만 동작

• 언제든 상위 태스크에 의해 선점 허용 | 스택 카나리 무결성 전수 검사, 런타임 틱/CPU 사용률 계산, 디버그 출력 |

| **0** (1레벨) | **System IDLE** (유휴 상태 고정) | 상시 상주 | • `task_sleep_ticks` 및 `mutex_lock` 호출 절대 금지

• `wfi` 무한 루프 저전력 대기 | `idle_task` |

### 3.2. 부팅 시퀀스 및 예외 처리 정책

- **결정론적 부팅 시퀀스 (Step 0 ~ Step 5)**:

- **Step 0 (엔트리 진입)**: 링커 스크립트에 정의된 RAM 시작 주소(`0x8000_0000`)를 기반으로 실행 환경 또는 로더가 엔트리 심볼인 `_start`로 CPU 제어를 넘긴다.
- **Step 1 (초기 스택 및 BSS 설정)**: `_start`는 부트 스택 포인터(`sp`)를 `_stack_top`으로 설정하고, `.bss` 영역을 0으로 초기화한 뒤 `main()`을 호출한다. (하드웨어 스레드 `mhartid` 검사 및 `gp` 초기화는 수행하지 않으며, `.data` 영역은 링커 스크립트에 의해 이미 RAM에 상주하므로 LMA $\rightarrow$ VMA 복사를 거치지 않는다.)
- **Step 2 (플랫폼 및 트랩 벡터 초기화)**: `main()`은 UART 하드웨어를 초기화하여 부팅 메시지를 출력한 뒤, `trap_init()`을 호출하여 `trap_entry`의 기본 주소를 `mtvec` 레지스터에 기록한다. 동작 모드는 Direct Mode(`0x00`)로 설정한다.
- **Step 3 (CLINT 서브시스템 및 응용 태스크 초기화)**:

1. CLINT 소프트웨어 인터럽트(MSIE)를 활성화한다.
2. CLINT 타이머 인터럽트(MTIE)를 활성화하며 첫 번째 타이머 비교 레지스터를 `mtime + TIMER_INTERVAL`($10\text{ MHz}$ timebase 기준 $1\text{ms}$ 상수 `10,000`)로 설정한다. (이 단계에서는 `mstatus.MIE`를 설정하지 않아 전역 인터럽트 비활성화 상태를 유지한다.)
3. `tasks_init()`을 호출하여 사용자 응용 태스크들을 정적 등록한다.
- **Step 4 (커널 무결성 검증 및 Idle 등록)**: `priortos_start()`에 진입하여 시스템 기본 Idle 태스크를 등록하고, IPCP 무결성 설정 및 준비 상태 태스크 유무를 확인한 뒤 최고 우선순위 태스크 슬롯을 확정한다.
- **Step 5 (최초 문맥 복원 및 전역 인터럽트 활성화)**: `prio_context_first_switch()`를 호출하여 선택된 태스크의 초기 문맥을 CPU 레지스터로 복원한다. 초기 스택에 사전 주입된 `mstatus` 값인 `0x1880`(MPP=Machine Mode, MPIE=1)을 CSR로 복원한 뒤, `mret` 명령어를 실행하여 태스크 엔트리 함수로 점프함과 동시에 전역 인터럽트(`mstatus.MIE`)를 활성화한다.
- **트랩 및 예외 처리 정책**:

- **인가된 하드웨어 인터럽트 디스패치 (`trap_handler`)**:

- Machine Software Interrupt (`mcause == 0x80000003`): CLINT의 pending 비트를 클리어하고 스케줄러를 호출하여 지연된 문맥 교환을 수행한다.
- Machine Timer Interrupt (`mcause == 0x80000007`): 다음 주기의 타이머 비교 레지스터(`mtimecmp`)를 재예약하고 `timer_tick_step()`을 호출하여 태스크 슬립 만료 및 선점 조건을 평가한다.
- **처리되지 않은 트랩 격리 (Unhandled Trap Lock-up)**:

- 위 2종류의 인가된 인터럽트를 제외한 모든 트랩 사유는 원인별 진단이나 CSR 출력 및 복구 절차를 거치지 않는다.
- `kernel_panic()`을 경유하지 않고, `trap_handler()` 내부의 `wfi` 무한 루프로 진입하여 CPU 실행을 동결한다.
- **명시적 커널 패닉 정책 (`kernel_panic(msg)`)**:

- 런타임 스택 guard/headroom 검사 실패 시 `stack_overflow_panic()`을 거쳐 `kernel_panic("task stack overflow")`가 호출되는 등, 코드 내에서 명시적으로 시스템 불변식 위반이 감지된 경우에 실행된다.
- 진입 시 `mstatus.MIE`를 비트 클리어하여 전역 인터럽트를 차단한다.
- 전달된 `msg` 포인터가 null이 아니면 UART를 통해 `[KERNEL PANIC]` 접두사와 해당 에러 메시지 문자열을 송신한다.
- 메시지 전송 후 `wfi` 무한 루프에 진입하여 시스템을 정지 상태로 유지한다.

### 3.3. 태스크 제어 서브시스템 (Task Control Subsystem)

- **REQ-TSK-001 (정적 태스크 등록 및 스택 빌드 - `task_create`)**:

- 동작 명세: 정적 스택 영역과 함수 포인터를 바인딩하여 태스크 제어 블록(TCB)을 구성하고 간접 매핑 테이블 및 준비 비트맵에 등록한다.
- 실행 시퀀스:

1. 인자 검증(`prio < 32`, `slot < OS_MAX_TASKS`, `stack != NULL`, `task_func != NULL`, `stack_size_bytes >= 144`)을 수행하며, 중복 우선순위 등록 시 `kernel_panic("duplicate priority")`을 일으킨다.
2. 스택 최하단 16바이트 영역(`stack_base + 0x00 ~ 0x0C`)에 스택 카나리 매직 넘버(`0x5A5A5A5A` $\times 4$ 워드)를 기록한다.
3. 스택 최상단에서 128바이트(32워드) 프레임을 확보하고 초기 문맥을 설정한다:

- `mstatus` 오프셋: `0x1880` (MPP=Machine, MPIE=1)
- `mepc` 오프셋: 태스크 엔트리 함수 포인터 주소(`task_func`)
- `ra` (`x1`) 오프셋: `task_exit_trap` (또는 `task_return_guard`) 방어 루틴 주소
- 나머지 범용 레지스터(`t0~t6`, `s0~s11`, `a0~a7`, `tp`, `gp`): 0으로 초기화
4. TCB의 `sp` 필드를 프레임 시작 주소로 설정하고, `priority`와 `base_priority`를 `prio`로 초기화한다.
5. 간접 매핑 테이블을 갱신(`g_prio_to_slot[prio] = slot`)하고, `g_ready_bitmap`에 해당 우선순위 비트를 세트(`g_ready_bitmap |= (1U << prio)`)한다.
- **REQ-TSK-002 (자발적 CPU 양보 - `task_yield`)**:

- 동작 명세: 실행 상태(Running)인 태스크가 다른 준비 상태 태스크에게 실행권을 넘기기 위해 호출한다.
- 실행 시퀀스:

1. 임계 구역에 진입한다 (`enter_critical`).
2. 호출 태스크의 현재 유효 우선순위 비트가 `g_ready_bitmap`에 유지되고 있는지 확인한다.
3. CLINT의 MSIP(`0x0200_0000`)를 1로 세트하여 소프트웨어 트랩을 요청한다.
4. 임계 구역을 탈출한다 (`exit_critical`).
- 제약 사항: $O(1)$ 복잡도를 유지하며 루프나 연결 리스트 순회를 배제한다.
- **REQ-TSK-003 (지정 틱 슬립 - `task_sleep_ticks`)**:

- 동작 명세: 호출 태스크를 지정한 정수 시스템 틱(`ticks > 0`) 동안 대기(Blocked) 상태로 전이시킨다.
- 호출 태스크가 뮤텍스를 점유하여 우선순위가 승격된 상태(`g_current_tcb->base_priority != g_current_tcb->priority`)일 경우 `kernel_panic("sleep with mutex")`을 발생시킨다.
- 실행 시퀀스:

1. 인자 검증(`ticks > 0`) 후 임계 구역에 진입한다 (`enter_critical`).
2. 현재 태스크 슬롯(`slot`)에 대응하는 정적 슬립 카운터 배열(`g_sleep_remaining_ticks[slot]`)에 `ticks` 인자값을 기록한다.
3. `g_ready_bitmap`에서 현재 태스크의 우선순위 비트를 클리어(`g_ready_bitmap &= ~(1U << prio)`)한다.
4. `g_sleep_bitmap`에 현재 태스크의 슬롯 비트를 세트(`g_sleep_bitmap |= (1U << slot)`)하고 TCB 상태를 `TASK_STATE_BLOCKED`로 변경한다.
5. `CLINT_MSIP = 1`을 마킹하여 디스패치를 유도하고 임계 구역을 탈출한다 (`exit_critical`).
- 제약 사항: 지연 큐 정렬($O(N)$)을 금지하고 비트맵 설정만으로 상태를 전환한다.
- **REQ-TSK-004 (유휴 태스크 본체 - `idle_task`)**:

- 동작 명세: 실행 가능한 응용 태스크가 없을 때 CPU 저전력 대기를 수행한다.
- 실행 시퀀스:

1. 우선순위 0(슬롯 0)으로 고정 등록되며 준비 비트맵(`g_ready_bitmap` bit 0)에 상주한다.
2. 본체는 `while(1) { __asm__ volatile("wfi"); }` 무한 루프로 동작한다.
- 제약 사항: 유휴 태스크 내부에서 `task_sleep_ticks`, `mutex_lock`, `semaphore_wait` 호출을 금지한다.
- **REQ-TSK-005 (커널 시동 - `priortos_start`)**:

- 동작 명세: 기본 시스템 태스크를 등록하고 IPCP 무결성을 검증한 뒤, 최고 우선순위 태스크로 최초 문맥을 복원하여 커널 스케줄링을 개시한다.
- 실행 시퀀스:

1. `task_create(IDLE_TASK_PRIORITY, idle_task, idle_stack, sizeof(idle_stack))`를 호출하여 유휴 태스크를 등록하며, 반환값이 `TASK_CREATE_OK`가 아니면 `kernel_panic("scheduler started without ready tasks")`을 일으킨다.
2. `ipcp_validate_configuration()`을 호출하여 바인딩된 뮤텍스 천장 우선순위 구성을 검증하며, 실패 시 `kernel_panic("invalid IPCP configuration")`을 일으킨다.
3. 준비 상태 비트맵을 검사하여 `g_ready_bitmap == 0`일 경우 `kernel_panic("scheduler started without ready tasks")`을 일으킨다.
4. `sched_highest_priority_slot(g_ready_bitmap)`을 호출해 준비 비트맵에서 최고 우선순위 태스크의 슬롯(`top_slot`)을 산출하며, 반환값이 `OS_MAX_TASKS` 이상일 경우 `kernel_panic("scheduler has no ready tasks")`을 일으킨다.
5. 정적 TCB 배열에서 대상 태스크의 참조를 획득(`g_current_tcb = &g_tcbs[top_slot]`)하고 상태를 `TASK_STATE_RUNNING`으로 전이한다.
6. `ipcp_scheduler_started()`를 호출하여 동기화 서브시스템의 스케줄러 가동 플래그를 활성화한다.
7. `prio_context_first_switch(g_current_tcb)`를 호출하여 대상 태스크의 스택 프레임(`0x1880`, `mepc`)을 복원하고 `mret` 명령어로 태스크 엔트리 함수로 분기함과 동시에 전역 인터럽트(`mstatus.MIE`)를 활성화한다.
- 제약 사항: 본 함수는 반환되지 않으며(No-Return), 호출 이후의 `main()` 코드는 실행되지 않는다.

### 3.4. 상호 배제 및 동기화 서브시스템 (Synchronization Subsystem - IPCP & Semaphore)

- **REQ-SYN-001 (IPCP 뮤텍스 초기화 - `mutex_init` / `mutex_configure`)**:

- 동작 명세: 정적 뮤텍스 객체를 초기화하고 보호 대상 공유 자원의 천장 우선순위(`ceiling`)를 바인딩한다.
- 초기화 규격:

- `locked = false`, `owner_slot = NO_TASK` (미점유 상태)
- `ceiling` = 공유 태스크들의 `base_priority` 최댓값 ($1 \sim 31$)
- 제약 사항: 해당 뮤텍스를 공유하는 모든 태스크의 기본 우선순위 최댓값 이상으로 `ceiling`이 설정되었는지 초기화 시점에 검증한다. 대기 큐는 할당하지 않는다.
- **REQ-SYN-002 (뮤텍스 획득 - `mutex_lock`)**:

- 동작 명세: 천장 우선순위 승격 및 간접 매핑 이전을 통해 락을 획득한다.
- 실행 시퀀스:

1. 임계 구역에 진입한다 (`enter_critical`).
2. 호출 태스크의 기본 우선순위(`base_priority`)가 `ceiling`보다 높으면 `kernel_panic("invalid mutex ceiling")`을 일으킨다.
3. 락이 이미 점유된 상태(`locked == true`)라면 IPCP 불변식 위반으로 간주하여 `kernel_panic("mutex contention in IPCP")`을 일으킨다.
4. `locked = true`, `owner_slot = current_slot`으로 설정한다.
5. 호출 태스크의 현재 유효 우선순위(`old_prio = g_current_tcb->priority`)가 `ceiling`보다 낮을 경우:

- `g_ready_bitmap`에서 `old_prio` 비트를 클리어하고 `ceiling` 위치의 비트를 세트한다.
- 간접 매핑 테이블에 승격된 우선순위의 소유 슬롯을 기록(`g_prio_to_slot[ceiling] = current_slot`)한다.
- `g_current_tcb->priority = ceiling`으로 갱신한다.
6. 임계 구역을 탈출한다 (`exit_critical`).
- **REQ-SYN-003 (뮤텍스 해제 - `mutex_unlock`)**:

- 동작 명세: 점유 중인 락을 반환하고 원래 우선순위로 복원한다.
- 실행 시퀀스:

1. 임계 구역에 진입한다 (`enter_critical`).
2. 호출 태스크가 현재 소유자(`owner_slot == current_slot`)가 아니면 `kernel_panic("unlock non-owned mutex")`을 일으킨다.
3. `locked = false`로 해제하고 `owner_slot = NO_TASK`로 설정한다.
4. `g_ready_bitmap`에서 승격되었던 `ceiling` 비트를 제거하고, 원래 우선순위(`base_priority`, 또는 잔여 보유 뮤텍스 중 최대 천장값) 비트를 세트한다.
5. 간접 매핑 테이블을 원복(`g_prio_to_slot[restored_prio] = current_slot`)하고 `g_current_tcb->priority = restored_prio`로 복원한다.
6. 우선순위 강등에 따른 스케줄링 재평가를 위해 `CLINT_MSIP = 1`을 기록한다.
7. 임계 구역을 탈출한다 (`exit_critical`).
- **REQ-SYN-004 (세마포어 대기 - `semaphore_wait` 및 상호작용 격리 검증)**:

- 동작 명세: 가용 카운트를 1 감소시키며 자원 부족 시 태스크를 블록 상태로 전이한다.
- **동시성 불변식 검증 (`INV-SYN-012`)**:

- 호출 태스크가 뮤텍스를 점유하여 우선순위가 승격된 상태(`g_current_tcb->base_priority != g_current_tcb->priority`)에서 호출 시 즉시 `kernel_panic("semaphore wait with mutex")`을 격발한다.
- 실행 시퀀스:

1. 임계 구역에 진입한다 (`enter_critical`).
2. 불변식 `INV-SYN-012`를 검증하여 위반 시 패닉을 유발한다.
3. `sem->count > 0`인 경우: `sem->count--` 후 임계 구역을 탈출하고 즉시 성공 반환한다.
4. `sem->count == 0`인 경우:

- 현재 태스크의 슬롯 비트를 `sem->waiters`에 등록한다 (`sem->waiters |= (1U << current_slot)`).
- 현재 태스크의 준비 비트를 클리어(`g_ready_bitmap &= ~(1U << current_prio)`)하고 TCB 상태를 `TASK_STATE_BLOCKED`로 전이한다.
- `CLINT_MSIP = 1`을 기록하여 디스패치를 유도한다.
- 임계 구역을 탈출한다 (`exit_critical`).
- **REQ-SYN-005 (세마포어 시그널 - `semaphore_signal`)**:

- 동작 명세: 가용 카운트를 1 증가시키거나 대기 중인 최고 우선순위 태스크를 깨운다.
- 실행 시퀀스:

1. 임계 구역에 진입한다 (`enter_critical`).
2. `sem->waiters == 0`인 경우: `sem->count++` 후 임계 구역을 탈출한다.
3. `sem->waiters != 0`인 경우:

- 대기 슬롯 중 최고 기본 우선순위를 가진 슬롯(`target_slot`)을 선별한다.
- `sem->waiters &= ~(1U << target_slot)`으로 대기 비트를 해제한다.
- 대상 태스크의 상태를 `TASK_STATE_READY`로 전이하고, `prio = g_tcbs[target_slot].priority` 확인 후 `g_prio_to_slot[prio] = target_slot`, `g_ready_bitmap |= (1U << prio)`를 세트한다.
- `prio > g_current_tcb->priority`인 경우 `CLINT_MSIP = 1`을 기록하여 선점을 요청한다.
- 임계 구역을 탈출한다 (`exit_critical`).

### 3.5. 임계 구역 관리 서브시스템 (Critical Section Subsystem)

- **REQ-CRI-001 (중첩 임계 구역 진입 - `enter_critical`)**:

- 동작 명세: 전역 인터럽트를 차단하고 중첩 카운터를 갱신한다.
- 실행 시퀀스:

1. `csrci mstatus, 8`을 실행하여 코어 하드웨어 인터럽트(MIE)를 비활성화한다.
2. 컴파일러 최적화 방지 메모리 배리어(`__asm__ volatile("" ::: "memory");`)를 실행한다.
3. 전역 중첩 진입 카운터(`g_critical_nesting_count`)를 1 증가시킨다.
- 제약 사항: 카운터 오버플로(`count == 0xFFFFFFFFU`) 발생 시 `kernel_panic("critical overflow")`을 일으킨다.
- **REQ-CRI-002 (중첩 임계 구역 탈출 - `exit_critical`)**:

- 동작 명세: 중첩 진입 카운터를 감산하고, 최상위 호출자가 탈출했을 때 파이프라인 및 메모리를 완전히 동기화한 뒤 인터럽트를 원자적으로 복원한다.
- **원자적 탈출 및 배리어 규약 (`INV-CRI-011`)**:

- `CLINT_MSIP = 1`과 같은 지연 디스패치 트리거 명령어가 인터럽트 활성화 이전에 메모리 버스에 완전히 반영되도록 **컴파일러 배리어와 하드웨어 명령어/메모리 배리어(`fence`)를 강제**한다.
- 실행 시퀀스:

1. 카운터 언더플로 검증: `g_critical_nesting_count == 0`인 상태에서 호출 시 `kernel_panic("critical underflow")`을 일으킨다.
2. `g_critical_nesting_count`를 1 감소시킨다.
3. 감산 후 카운터 값이 0에 도달한 경우에 한하여:

- 컴파일러 재정렬 방지: `__asm__ volatile("" ::: "memory");`
- 하드웨어 메모리/파이프라인 배리어: `__asm__ volatile("fence rw, rw" ::: "memory");`
- 전역 하드웨어 인터럽트 복원: `csrsi mstatus, 8` 실행. (직후 보류 중이던 Cause 3 소프트웨어 트랩이 결정론적으로 점화된다.)

### 3.6. 타이머 및 시간 서브시스템 (Time Subsystem)

- **REQ-TMR-001 (시스템 업타임 원자적 조회 - `get_tick_count`)**:

- 동작 명세: 부팅 이후 타이머 ISR(Cause 7)에 의해 누적된 64비트 시스템 틱 카운터(`g_system_ticks`) 값을 단절 없이 반환한다.
- **원자적 읽기 표준 규약**:

C

- 32비트 RV32I 환경에서 64비트 정수 읽기 시 32비트 롤오버 경계 경쟁(Race Condition)을 원천 차단하기 위해 `enter_critical()`로 임계 구역을 감싼 상태에서 64비트 복사를 수행하거나, 인터럽트를 끄지 않는 환경에서는 High-Low-High 롤오버 비교 알고리즘을 강제한다.

```
uint64_t get_tick_count(void){
    uint64_t ticks;
    enter_critical();
    ticks = g_system_ticks;
    exit_critical();
    return ticks;
}
```
- **REQ-TMR-002 (주기적 틱 감산 및 만료 비트맵 전이 - `timer_tick_step`)**:

- 동작 명세: 타이머 ISR(Cause 7, Top-Half) 내부에서 호출되어 슬립 태스크의 잔여 시간을 감산하고 만료 태스크를 복구한다.
- 실행 시퀀스:

1. 64비트 `g_system_ticks`를 1 증가시킨다.
2. `g_sleep_bitmap`이 0이 아니면 활성 슬립 슬롯(`slot`)들을 대상으로 `g_sleep_remaining_ticks[slot]`을 1씩 감산한다.
3. `g_sleep_remaining_ticks[slot] == 0`에 도달한 태스크는 `g_sleep_bitmap`에서 해당 슬롯 비트를 지우고, 태스크의 우선순위(`prio = g_tcbs[slot].priority`)를 기준으로 `g_prio_to_slot[prio] = slot` 매핑을 보장한 뒤 `g_ready_bitmap |= (1U << prio)`를 세트한다.
4. 깨어난 태스크의 우선순위가 현재 실행 중인 태스크보다 높으면 `CLINT_MSIP = 1`을 기록하여 선점 트랩을 예약한다.
- 제약 사항: 태스크 총수가 `OS_MAX_TASKS <= 32`로 제한되므로 고정 상한 루프로 처리하며, 실행 시간을 $WCET \le 150\text{ cycles}$ 이내로 제한한다.

# PART II. 상세 설계 명세서 (LLD: Low-Level Design Specification)

> **[요약]** PART II는 PART I(SRS)의 규칙을 구현하기 위한 청사진이다. 소스 코드의 직접 나열을 배제하고, 파일 아키텍처, 바이트 오프셋 테이블, 인터페이스 계약, 그리고 정적 메모리 사이징 및 스냅샷을 정의한다.

## 4. 디렉토리 및 파일 아키텍처 (Repository File Mapping)

```
PrioRTOS/
├── Makefile                               # 빌드 자동화 스크립트 (컴파일러 플래그, 링커 연결, QEMU 실행)
├── README.md                              # 프로젝트 개요 및 빌드 매뉴얼
├── linker.ld                              # RAM(0x8000_0000) 섹션(.text/.data/.bss) 및 스택 배치 링커 스크립트
│
├── boot/
│   └── startup.S                          # _start 진입점: SP 설정, .bss 0 클리어 후 main() 호출
│
├── app/
│   ├── main.c                             # HW 초기화, tasks_init() 호출, priortos_start()로 커널 개시
│   ├── tasks.h                            # 응용 태스크 엔트리 함수 선언
│   └── tasks.c                            # 응용 태스크 본체 구현 및 task_create() 정적 등록(tasks_init)
│
├── bsp/
│   ├── clint.h                            # CLINT MMIO 레지스터(MSIP, MTIMECMP, MTIME) 주소 및 상수 정의
│   ├── clint.c                            # 하드웨어 타이머 장전, Cause 3 MSIP 소프트웨어 트랩 트리거/클리어
│   ├── uart.h                             # UART 16550 레지스터 오프셋 및 I/O 프로토타입 선언
│   └── uart.c                             # UART 초기화, 문자 1바이트 및 문자열 폴링 출력
│
├── kernel/
│   ├── context.S                          # [어셈블리] 최초 문맥 복원(first_switch) 및 128B 레지스터 저장/복원
│   ├── trap.c                             # mtvec 등록(trap_init) 및 C 트랩 디스패처 (Cause 7 타이머 / Cause 3 스케줄러)
│   ├── sched.c                            # De Bruijn O(1) 간접 매핑 스케줄러, task_create/sleep/yield, timer_tick_step
│   ├── mutex.c                            # 순수 IPCP 뮤텍스 (mutex_init, 무경합 lock 즉시 승격, unlock 복원)
│   ├── ipcp.c                             # IPCP 천장 우선순위 검증(ipcp_validate_configuration) 및 부팅 플래그
│   ├── semaphore.c                        # 카운팅 세마포어 모듈 (INV-SYN-012 상호작용 격리 검증 반영)
│   ├── panic.c                            # MIE 차단, UART 에러 출력, wfi 무한 루프 시스템 동결(Fail-Stop)
│   └── include/
│       ├── prio_rtos.h                    # 외부 공개 API 헤더 (태스크/뮤텍스 제어, 시스템 틱 조회)
│       ├── kernel_internal.h              # 커널 내부 공유 헤더 (TCB_t 구조체, enter/exit_critical 인라인 배리어)
│       └── stack_guard.h                  # 스택 카나리 매직 넘버(0x5A5A5A5A) 정의 및 오버플로 검사 매크로
│
├── docs/
│   ├── requirements.md                    # 커널 기능 요구사항 명세서 (SRS 3.1 ~ 3.6)
│   ├── design-decisions.md                # 아키텍처 결정 기록 (ADR: 동적 힙 배제, De Bruijn 선택 등)
│   ├── ipcp-change-record.md              # 대기 큐(wait_bitmap) 제거 및 순수 IPCP 전환 이력 기록
│   ├── traceability-matrix.md             # 요구사항(REQ)과 코드/불변식 간 1:1 양방향 추적성 표
│   ├── implementation-map.md              # 구현 완료 현황 및 모듈 간 의존성 맵
│   ├── task-sync-progress.md              # 태스크 제어 및 동기화 개발 진행 상황 트래커
│   └── images/
│       ├── architecture.png               # 시스템 아키텍처 다이어그램
│       └── call-graph.svg                 # 함수 호출 흐름도
│
└── build/                                 # 빌드 산출물 (ELF, 바이너리, 맵 파일, 분해 어셈블리, .o, .d)
```

### 4.2. 소스 파일-링커 섹션 매핑 및 책임 명세

**소스 파일 경로**
**생성 목적 파일**
**상주 메모리 섹션**
**담당 서브시스템 및 핵심 책무**

**boot/startup.S**
startup.o
.text.boot
초기 부트 스택 포인터(sp) 설정, .bss 영역 0 클리어, main() 호출

**kernel/context.S**
context.o
.text
128바이트 문맥 저장/복원 어셈블리, prio_context_first_switch() 최초 점화

**kernel/sched.c**
sched.o
.text, .rodata, .bss
De Bruijn $O(1)$ 간접 매핑 스케줄러, task_create/sleep/yield, timer_tick_step

**kernel/trap.c**
trap.o
.text
Direct Mode 트랩 벡터 진입점(trap_entry), Cause 3/7 C 디스패치, 미인가 트랩 동결

**kernel/mutex.c**
mutex.o
.text
IPCP 뮤텍스 획득(무경합 즉시 승격) 및 반환(원래 우선순위 복원, MSIP 트리거)

**kernel/ipcp.c**
ipcp.o
.text, .bss
정적 뮤텍스 천장 우선순위 유효성 검증(ipcp_validate_configuration)

**kernel/semaphore.c**
semaphore.o
.text
카운팅 세마포어 제어 및 `INV-SYN-012` 뮤텍스 중첩 블록 패닉 검증

**kernel/panic.c**
panic.o
.text
MIE 비활성화, UART 에러 문자열 송신, wfi 무한 루프 진입(kernel_panic)

**bsp/clint.c**
clint.o
.text
mtimecmp 비교값 갱신, MSIP 소프트웨어 트랩 비트 세트/클리어 MMIO 제어

**bsp/uart.c**
uart.o
.text
16550 호환 UART 하드웨어 초기화 및 폴링 기반 바이트/문자열 송신

**app/main.c**
main.o
.text
플랫폼 주변장치 초기화, tasks_init() 호출, priortos_start() 커널 시동 진입

**app/tasks.c**
tasks.o
.text, .bss
사용자 태스크 함수 본체(task_a, task_b) 및 태스크별 정적 스택 배열 할당

### 4.3. 헤더 파일 가시성 및 의존성 규칙 (Header Visibility & Scope)

- **공개 헤더 (Public Interface - `kernel/include/prio_rtos.h`)**:

- 대상 사용자: 응용 계층(`app/main.c`, `app/tasks.c`).
- 노출 범위: 태스크 및 동기화 제어 함수 프로토타입(`task_create`, `task_yield`, `task_sleep_ticks`, `mutex_lock`, `mutex_unlock`, `semaphore_wait`, `semaphore_signal`, `priortos_start`, `get_tick_count`), 공용 반환 열거형(`TaskCreateResult_t`, `MutexResult_t`, `SemaphoreResult_t`), 동기화 제어 블록(`Semaphore_t`, `mutex_t`).
- 은닉 대상: TCB 내부 멤버 구조체(`TCB_t`), 스케줄러 비트맵 변수, 간접 매핑 테이블, 하드웨어 레지스터 주소.
- **커널 내부 헤더 (Kernel Internal Interface - `kernel/include/kernel_internal.h`)**:

- 대상 사용자: 커널 핵심 소스(`kernel/*.c`, `kernel/*.S`) 전용.
- 노출 범위: TCB 구조체 정의(`TCB_t`), 전역 상태 변수 `extern` 선언(`g_ready_bitmap`, `g_sleep_bitmap`, `g_prio_to_slot[]`, `g_sleep_remaining_ticks[]`, `g_system_ticks`, `g_current_tcb`, `g_tcbs[]`, `g_critical_nesting_count`), 배리어를 포함한 내부 스케줄링 및 임계 구역 인라인 함수.
- 제약 사항: `app/` 계층 소스 파일에서의 인클루드를 일절 금지한다.
- **시스템 보호 헤더 (System Guard - `kernel/include/stack_guard.h`)**:

- 대상 사용자: `kernel/sched.c`, `kernel/context.S`, `kernel/trap.c`.
- 노출 범위: 스택 카나리 매직 넘버(`0x5A5A5A5A`), 16바이트 가드 영역 크기 매크로(`STACK_GUARD_SIZE`), 스택 오버플로 검사 매크로 및 `stack_overflow_panic()` 프로토타입.
- **하드웨어 추상화 헤더 (BSP Interface - `bsp/clint.h`, `bsp/uart.h`)**:

- 대상 사용자: 커널 트랩/부팅 모듈(`kernel/trap.c`, `app/main.c`, `kernel/panic.c`).
- 노출 범위: MMIO 베이스 물리 주소 및 비트마스크, 하드웨어 제어 함수 프로토타입.

## 5. 모듈별 함수 원형과 구현

### 5.1. boot/startup.S (부트 및 하드웨어 초기화)

**상태**
**심볼 및 함수 원형**
**입출력 규약**
**담당 역할 및 제어 흐름**

**[구현 완료]**
`void _start(void)`
입력: 없음

출력: 없음 | 하드웨어 리셋 벡터 진입점. 부트 스택 포인터(`la sp, _stack_top`) 설정, `.bss` 메모리 0 클리어 후 `tail main` 호출. |

### 5.2. kernel/context.S (저수준 문맥 전환 및 하드웨어 트랩 진입)

**상태**
**심볼 및 함수 원형**
**입출력 규약**
**담당 역할 및 제어 흐름**

**[구현 완료]**
`void trap_entry(void)`
입력: 없음 (하드웨어 트랩 진입)

출력: 없음 | `mtvec`에 등록되는 단일 트랩 진입 레이블. 128바이트 프레임을 스택에 확보하여 레지스터를 보존하고 C 핸들러(`trap_handler`)로 분기. |

| **[구현 완료]** | `void prio_context_first_switch(TCB_t *next)` | 입력: a0 (최초 실행 대상 태스크의 TCB 포인터)

출력: 없음 (Non-returning) | 첫 번째 태스크의 스택에서 레지스터와 CSR(`mstatus=0x1880`, `mepc`)을 복원하고 `mret`을 실행하여 태스크로 진입 (INV-LFC-006). |

| **[★ 미구현/추가 필요]** | `void prio_context_switch(TCB_t *curr, TCB_t *next)` | 입력: a0 (현재 TCB), a1 (다음 TCB)

출력: 없음 | 소프트웨어 인터럽트(Cause 3) 경로에서 현재 실행 문맥 128B를 백업하고 다음 태스크로 CPU 제어권을 전환하는 어셈블리 루틴. |

| **[★ 미구현/추가 필요]** | `void task_return_guard(void)` | 입력: 없음

출력: 없음 (Non-returning) | 태스크 함수가 `while(1)`을 탈출하여 정상 반환을 시도할 때 즉시 `kernel_panic("task returned")`을 격발하는 방어 트랩 레이블 (INV-LFC-006). |

### 5.3. kernel/trap.c (트랩 벡터 설정 및 인터럽트 디스패치)

**상태**
**심볼 및 함수 원형**
**입출력 규약**
**담당 역할 및 제어 흐름**

**[구현 완료]**
`void trap_init(void)`
입력: 없음

출력: 없음 | `trap_entry` 레이블 주소를 Direct Mode(`00b`)로 CSR `mtvec`에 기록. |

| **[구현 완료]** | `void trap_handler(void)` | 입력: 없음 (트랩 진입점에서 호출)

출력: 없음 | CSR `mcause` 판독 후 Cause 7(타이머) 및 Cause 3(소프트웨어) 인터럽트 분기 처리. |

| **[★ 미구현/추가 필요]** | `(로직 보완) 미인가 트랩 wfi 동결` | 입력: 없음

출력: 없음 | Cause 3, Cause 7 이외의 미인가 예외 발생 시 진단 출력 없이 `for(;;) { __asm__ volatile("wfi"); }`로 직행하는 안전 동결 루틴 (SRS 3.2). |

### 5.4. kernel/sched.c (비트맵 스케줄러 및 태스크 생명주기)

**상태**
**심볼 및 함수 원형**
**입출력 규약**
**담당 역할 및 제어 흐름**

**[구현 완료]**
`TaskCreateResult_t task_create(uint32_t prio, void (*task_func)(void), uint8_t *stack, uint32_t stack_size_bytes)`
입력: 우선순위, 태스크 함수 포인터, 스택 버퍼, 크기

출력: 성공/실패 열거형 | 스택 최하단 16B 카나리(`0x5A5A5A5A` $\times 4$) 주입, 128B 초기 문맥 프레임 빌드, `g_prio_to_slot[prio]` 바인딩 및 `g_ready_bitmap` 세트. |

| **[구현 완료]** | `void priortos_start(void)` | 입력: 없음

출력: 없음 (Non-returning) | Idle 태스크 등록, IPCP 무결성 검증, 최고 우선순위 슬롯 선정 후 `prio_context_first_switch` 호출. |

| **[구현 완료]** | `void task_yield(void)` | 입력: 없음

출력: 없음 | `enter_critical()` $\rightarrow$ `CLINT_MSIP = 1` $\rightarrow$`exit_critical()` 순서로 안전하게 선점 유발 (REQ-TSK-002). |

| **[구현 완료]** | `void idle_task(void)` | 입력: 없음

출력: 없음 (무한 루프) | 우선순위 0 유휴 태스크 본체. `while(1) { __asm__ volatile("wfi"); }` 저전력 대기 (REQ-TSK-004). |

| **[구현 완료]** | `uint32_t sched_highest_priority_slot(uint32_t task_bitmap)` | 입력: 준비 상태 우선순위 비트마스크

출력: 최고 우선순위 태스크의 슬롯 번호 (0~31) | Bit Smearing과 De Bruijn 승수(`0x077CB531U`) 및 32B LUT를 이용해 분기 없이 $O(1)$로 최상위 세트 비트(MSB)를 산출한 뒤, `g_prio_to_slot[highest_prio]`를 참조하여 실제 TCB 슬롯 번호를 반환 (REQ-SCH-001). |

| **[구현 완료]** | `void sched_schedule(void)` | 입력: 없음

출력: 없음 | 준비 비트맵을 평가하고 최고 우선순위 태스크 슬롯으로 스위칭을 유도하는 내부 스케줄러 함수. |

| **[구현 완료]** | `void sched_request_reschedule(void)` | 입력: 없음

출력: 없음 | `CLINT_MSIP = 1`을 세트하여 지연된 문맥 교환(DPC) 요청. |

| **[구현 완료]** | `bool sched_block_current(uint32_t saved_mstatus)` | 입력: 인터럽트 상태 값

출력: 블록 전이 성공 여부 | 현재 태스크의 유효 우선순위 비트를 `g_ready_bitmap`에서 제거하고 Blocked 상태로 전환. |

| **[구현 완료]** | `bool sched_wake_task(uint32_t slot)` | 입력: 대상 태스크 슬롯 번호

출력: 성공 여부 | 특정 태스크를 Ready 상태로 복귀시키고 해당 태스크의 우선순위 비트를 `g_ready_bitmap`에 세트. |

| **[구현 완료]** | `void stack_overflow_panic(void)` | 입력: 없음

출력: 없음 (Non-returning) | 스택 카나리 손상 감지 시 `kernel_panic("task stack overflow")` 격발. |

| **[구현 완료]** | `static void task_exit_trap(void)` | 입력: 없음

출력: 없음 | 태스크 함수가 반환되었을 때 진입하는 디버그 격리 루프. |

| **[★ 미구현/추가 필요]** | `void task_sleep_ticks(uint32_t ticks)` | 입력: 대기할 틱 수 (`ticks > 0`)

출력: 없음 | [REQ-TSK-003 필수] 뮤텍스 점유 검증(`base_priority != priority` 시 패닉) $\rightarrow$ `g_sleep_remaining_ticks[slot]` 기록 $\rightarrow$ `g_sleep_bitmap` 등록 $\rightarrow$ `g_ready_bitmap` 클리어 $\rightarrow$ `MSIP = 1` 선점 유도. |

| **[★ 미구현/추가 필요]** | `void timer_tick_step(void)` | 입력: 없음

출력: 없음 | [REQ-TMR-002 필수] 타이머 ISR(Cause 7)에서 호출되어 `g_system_ticks++`, 잔여 틱 1 감산, 만료자 `g_ready_bitmap` 복구 및 선점 트리거 처리 ($WCET \le 150\text{ cycles}$). |

| **[★ 미구현/추가 필요]** | `uint64_t get_tick_count(void)` | 입력: 없음

출력: 64비트 시스템 누적 틱 | [REQ-TMR-001 필수] `enter_critical()` 보호 하에 64비트 `g_system_ticks` 값을 원자적으로 읽어 반환하는 공개 API. |

### 5.5. kernel/ipcp.c (Immediate Priority Ceiling Protocol 동기화)

**상태**
**심볼 및 함수 원형**
**입출력 규약**
**담당 역할 및 제어 흐름**

**[구현 완료]**
`MutexResult_t mutex_configure(uint32_t mutex_id, uint32_t user_slots)`
입력: 뮤텍스 ID, 공유 태스크 슬롯 비트마스크

출력: 설정 결과 코드 | 뮤텍스에 대한 공유 태스크들을 정적 바인딩하고 천장 우선순위(`ceiling`)를 산출. |

| **[구현 완료]** | `MutexResult_t mutex_lock(uint32_t mutex_id)` | 입력: 획득 대상 뮤텍스 ID

출력: 결과 코드 | 락 획득 및 천장 우선순위 승격. `g_ready_bitmap`의 우선순위 비트를 교체하고 `g_prio_to_slot[ceiling] = slot` 매핑을 갱신 (REQ-SYN-002). |

| **[구현 완료]** | `MutexResult_t mutex_unlock(uint32_t mutex_id)` | 입력: 해제 대상 뮤텍스 ID

출력: 결과 코드 | 락 해제, 원래 우선순위 비트 및 `g_prio_to_slot` 매핑 복원 후 `CLINT_MSIP = 1` 선점 유발 (REQ-SYN-003). |

| **[구현 완료]** | `bool ipcp_validate_configuration(void)` | 입력: 없음

출력: 무결성 검증 결과 (true/false) | 시스템 시동 전, 정의된 뮤텍스들의 천장 우선순위가 공유자들의 최대 우선순위 이상인지 정적 검증. |

| **[구현 완료]** | `void ipcp_scheduler_started(void)` | 입력: 없음

출력: 없음 | 스케줄러 기동 이후 추가 뮤텍스 설정을 금지하는 락다운 플래그 활성화. |

| **[구현 완료]** | `static bool current_task_slot(uint32_t *slot_out)` | 입력: [out] 슬롯 번호 포인터

출력: 유효 여부 | 현재 실행 중인 태스크의 슬롯 번호를 추출. |

### 5.6. kernel/mutex.c (경량 포인터형 IPCP 뮤텍스 모듈)

**상태**
**심볼 및 함수 원형**
**입출력 규약**
**담당 역할 및 제어 흐름**

**[★ 미구현/추가 필요]**
`void mutex_init(mutex_t *m, uint8_t ceiling_prio)`
입력: 뮤텍스 포인터, 천장 우선순위

출력: 없음 | [REQ-SYN-001 필수] 정적 뮤텍스 객체 초기화 (`owner = NO_TASK`, `ceiling_prio` 설정). 대기 큐 미할당. |

| **[★ 미구현/추가 필요]** | `void mutex_lock(mutex_t *m)` | 입력: 뮤텍스 포인터

출력: 없음 (위반 시 패닉) | [REQ-SYN-002 필수] 임계 구역 진입 $\rightarrow$ 천장 유효성 검사 $\rightarrow$ 무경합 즉시 승격 $\rightarrow$ `g_ready_bitmap` 및 `g_prio_to_slot` 갱신 $\rightarrow$ 탈출. |

| **[★ 미구현/추가 필요]** | `void mutex_unlock(mutex_t *m)` | 입력: 뮤텍스 포인터

출력: 없음 (위반 시 패닉) | [REQ-SYN-003 필수] 소유권 검사 $\rightarrow$ 우선순위 및 매핑 원복 $\rightarrow$ `owner = NO_TASK` $\rightarrow$ `CLINT_MSIP = 1` $\rightarrow$ 탈출. |

### 5.7. kernel/semaphore.c (카운팅 세마포어 모듈)

**상태**
**심볼 및 함수 원형**
**입출력 규약**
**담당 역할 및 제어 흐름**

**[구현 완료]**
`SemaphoreResult_t semaphore_init(Semaphore_t *sem, uint32_t initial_count)`
입력: 세마포어 포인터, 초기 카운트

출력: 결과 코드 | 가용 자원 수(`count`) 초기화 및 대기 슬롯 비트맵(`waiters = 0`) 설정. |

| **[구현 완료]** | `SemaphoreResult_t semaphore_wait(Semaphore_t *sem)` | 입력: 세마포어 포인터

출력: 결과 코드 | `INV-SYN-012` 뮤텍스 중첩 검사(승격 상태 시 패닉) $\rightarrow$ `count` 감산. 자원 부족 시 태스크 슬롯 비트를 `waiters`에 등록하고 Blocked 전이 후 재스케줄링 (REQ-SYN-004). |

| **[구현 완료]** | `SemaphoreResult_t semaphore_signal(Semaphore_t *sem)` | 입력: 세마포어 포인터

출력: 결과 코드 | `count` 가산. 대기자가 존재할 경우 최고 우선순위 대기 태스크 슬롯을 깨우고 비트맵 및 매핑 갱신 (REQ-SYN-005). |

### 5.8. kernel/panic.c (커널 패닉 및 안전 정지)

**상태**
**심볼 및 함수 원형**
**입출력 규약**
**담당 역할 및 제어 흐름**

**[구현 완료]**
`__attribute__((noreturn)) void kernel_panic(const char *msg)`
입력: msg (출력할 에러 문자열)

출력: 없음 (Non-returning) | `csrci mstatus, 8`로 전역 인터럽트 차단, UART로 `[KERNEL PANIC]`과 문자열 송신 후 `wfi` 무한 루프 진입 (SRS 3.2). |

### 5.9. kernel/include/kernel_internal.h (중첩 임계 구역 인라인 API 및 배리어 규약)

**상태**
**심볼 및 함수 원형**
**입출력 규약**
**담당 역할 및 제어 흐름**

**[★ 미구현/추가 필요]**
`static inline void enter_critical(void)`
입력: 없음

출력: 없음 | [REQ-CRI-001 필수] `csrci mstatus, 8` $\rightarrow$ 컴파일러 메모리 배리어 $\rightarrow$ `g_critical_nesting_count++` $\rightarrow$ 오버플로 시 `kernel_panic("critical overflow")`. |

| **[★ 미구현/추가 필요]** | `static inline void exit_critical(void)` | 입력: 없음

출력: 없음 | [REQ-CRI-002 / INV-CRI-011 필수] 언더플로 검사 $\rightarrow$ `g_critical_nesting_count--` $\rightarrow$ 카운터 0 도달 시 컴파일러 배리어 및 `fence rw, rw` 실행 후 `csrsi mstatus, 8`로 인터럽트 원자적 복원. |

C

```
/* kernel/include/kernel_internal.h 배리어 구현 명세 */
static inline void enter_critical(void){
    __asm__ volatile("csrci mstatus, 8" ::: "memory");
    __asm__ volatile("" ::: "memory");
    g_critical_nesting_count++;
    if (g_critical_nesting_count == 0xFFFFFFFFU) {
        kernel_panic("critical overflow");
    }
}

static inline void exit_critical(void){
    if (g_critical_nesting_count == 0) {
        kernel_panic("critical underflow");
    }
    g_critical_nesting_count--;
    if (g_critical_nesting_count == 0) {
        __asm__ volatile("" ::: "memory");
        __asm__ volatile("fence rw, rw" ::: "memory");
        __asm__ volatile("csrsi mstatus, 8" ::: "memory");
    }
}
```

### 5.10. bsp/clint.c (Core Local Interruptor 하드웨어 드라이버)

**상태**
**심볼 및 함수 원형**
**입출력 규약**
**담당 역할 및 제어 흐름**

**[구현 완료]**
`void clint_software_interrupt_init(void)`
입력: 없음

출력: 없음 | 소프트웨어 인터럽트(MSIP) 비트 초기화 및 CSR `mie.MSIE` 비트 활성화. |

| **[구현 완료]** | `void clint_send_software_interrupt(void)` | 입력: 없음

출력: 없음 | 코어 외부 MMIO `0x0200_0000`(MSIP0)에 1을 기록하여 Cause 3 트랩 유발. |

| **[구현 완료]** | `void clint_clear_software_interrupt(void)` | 입력: 없음

출력: 없음 | `0x0200_0000`에 0을 기록하여 트랩 pending 비트 해제. |

| **[구현 완료]** | `void clint_timer_init(void)` | 입력: 없음

출력: 없음 | 첫 타이머 비교값(`mtime + TIMER_INTERVAL`) 장전 및 CSR `mie.MTIE` 비트 활성화. |

| **[구현 완료]** | `void clint_set_next_timer(void)` | 입력: 없음

출력: 없음 | `mtimecmp` 레지스터를 다음 주기 시각(`+ 10,000`)으로 재예약. |

| **[구현 완료]** | `uint64_t clint_get_mtime(void)` | 입력: 없음

출력: 64비트 현재 시간 | `0x0200_BFF8` 하드웨어 실시간 타이머 레지스터를 원자적으로 읽어 반환. |

| **[구현 완료]** | `void clint_set_mtimecmp(uint64_t compare_value)` | 입력: 64비트 비교값

출력: 없음 | `0x0200_4000`(`mtimecmp`)에 64비트 비교 시각을 원자적 시퀀스로 기록. |

### 5.11. bsp/uart.c (16550 호환 UART 시리얼 드라이버)

**상태**
**심볼 및 함수 원형**
**입출력 규약**
**담당 역할 및 제어 흐름**

**[구현 완료]**
`void uart_init(void)`
입력: 없음

출력: 없음 | UART 16550 통신 속도(Baudrate), 데이터 비트, 패리티, FIFO 활성화. |

| **[구현 완료]** | `void uart_putc(char c)` | 입력: c (출력할 문자)

출력: 없음 | LSR(Line Status Register) TX 빈 공간을 폴링 확인 후 바이트 전송. |

| **[구현 완료]** | `void uart_puts(const char *str)` | 입력: str (널 종료 문자열)

출력: 없음 | 문자열 끝(`\0`)까지 순회하며 `uart_putc()`를 호출하여 시리얼 송신. |

### 5.12. app/main.c (어플리케이션 진입점)

**상태**
**심볼 및 함수 원형**
**입출력 규약**
**담당 역할 및 제어 흐름**

**[구현 완료]**
`int main(void)`
입력: 없음

출력: int | UART 초기화 $\rightarrow$ `trap_init()` $\rightarrow$ CLINT 인터럽트 활성화 $\rightarrow$ `tasks_init()` $\rightarrow$ `priortos_start()` 순차 실행. |

### 5.13. app/tasks.c (사용자 응용 태스크 본체 및 초기화)

**상태**
**심볼 및 함수 원형**
**입출력 규약**
**담당 역할 및 제어 흐름**

**[구현 완료]**
`TaskCreateResult_t tasks_init(void)`
입력: 없음

출력: 성공/실패 결과 | 세마포어 및 응용 태스크 A/B에 스택을 바인딩하고 `task_create()`로 등록. |

| **[구현 완료]** | `void task_a(void)` | 입력: 없음

출력: 없음 (무한 루프) | 세마포어/뮤텍스 획득 후 문자 'A'를 출력하는 사용자 태스크. |

| **[구현 완료]** | `void task_b(void)` | 입력: 없음

출력: 없음 (무한 루프) | 세마포어/뮤텍스 획득 후 문자 'B'를 출력하는 사용자 태스크. |

| **[구현 완료]** | `static void mock_delay(volatile uint32_t count)` | 입력: 루프 반복 횟수

출력: 없음 | 지연 시뮬레이션을 위한 단순 카운트 다운 루프. |

### 5.14. 파일별 미구현 누락 항목 총괄 요약표

**대상 소스 파일**
**미구현 누락 함수 및 전역 심볼**
**추적 SRS 요구사항**
**구현 난이도 및 중요도**

**kernel/include/kernel_internal.h**
`enter_critical(void)`

`exit_critical(void)` (배리어 포함)

`g_critical_nesting_count` | REQ-CRI-001

REQ-CRI-002

INV-CRI-011 | **최상** (커널 원자성의 근간) |

| **kernel/sched.c** | `task_sleep_ticks(ticks)`

`timer_tick_step(void)`

`get_tick_count(void)`

`g_prio_to_slot[32]`

`g_sleep_bitmap`

`g_sleep_remaining_ticks[]`

`g_system_ticks` | REQ-SCH-001

REQ-TSK-003

REQ-TMR-002

REQ-TMR-001 | **최상** ($O(1)$ 간접 매핑 및 시간 서브시스템 완결) |

| **kernel/context.S** | `prio_context_switch`

`task_return_guard` | SRS 3.1

INV-LFC-006 | **상** (트랩 기반 문맥 교환) |

| **kernel/mutex.c** | `mutex_init`

`mutex_lock`

`mutex_unlock` | REQ-SYN-001

REQ-SYN-002

REQ-SYN-003 | **상** (순수 무경합 IPCP 완성) |

| **kernel/semaphore.c** | `INV-SYN-012` 검증 로직 반영 | REQ-SYN-004

INV-SYN-012 | **상** (동기화 프리미티브 간 격리 완성) |

## 6. 정밀 메모리 레이아웃 및 자료구조 명세 (Data Structures & Memory Layout)

본 섹션은 현재 저장소의 빌드 환경(RV32IMC / ILP32 ABI) 및 실제 소스 코드(`linker.ld`, `stack_guard.h`, `kernel_internal.h`, `prio_rtos.h`, `sched.c`, `ipcp.c`)를 기준으로 실측된 물리 메모리 배치와 자료구조 바이트 레이아웃을 규정한다.

### 6.1. 링커 스크립트 메모리 파티셔닝 (linker.ld)

타깃 하드웨어 플랫폼의 단일 물리 RAM 주소 공간(`0x8000_0000`, 128 MiB)을 기반으로 선형 배치되며, VMA와 LMA는 동일하다.

- **메모리 영역 정의 (Memory Region)**:

- `RAM (rwx)`: `ORIGIN = 0x80000000`, `LENGTH = 128M` (물리 주소 `0x8000_0000` ~ `0x87FF_FFFF`)
- **출력 섹션 배치 및 링커 심볼 명세**:

**출력 세그먼트**
**VMA 배치 기준**
**권한**
**Export 링커 심볼**
**내용 및 설계 제약 사항**

**.text**
0x8000_0000
RX
없음
`*(.text.boot)`가 첫 머리에 위치하여 `boot/startup.S`의 `_start`가 최우선 배치됨. 일반 실행 코드 및 `.rodata`가 단일 섹션으로 통합 상주.

**.data**
.text 종료 직후
RW
없음
초기화된 전역/정적 데이터 상주. RAM 상주 상태로 구동되므로 부팅 시 별도 LMA $\rightarrow$ VMA 복사를 거치지 않음.

**.bss**
.data 종료 직후
RW
`__bss_start`

`__bss_end` | 미초기화 및 0 초기화 전역 변수(`g_tcbs`, `mutexes` 등)와 정적 태스크 스택 영역. `_start`에 의해 전 영역이 0으로 클리어됨. |

| **System Stack** | .bss 종료 직후 | RW | `_stack_bottom`

`_stack_top` | `.bss` 종료 지점에서 16바이트 정렬된 후 4 KiB 크기로 할당되는 부트/인터럽트 전용 시스템 스택. |

> **설계 주의 사항**: 현재 구현체에는 독립된 `.rodata` 세그먼트, `.heap` 세그먼트 및 힙 크기 차단용 `ASSERT` 문이 존재하지 않는다. 동적 힙 할당 API를 배제함으로써 제로 힙(Zero-heap, `INV-MEM-001`) 환경을 유지한다.

### 6.2. 태스크 스택 가드 및 문맥 프레임 레이아웃 (kernel/context.S, kernel/include/stack_guard.h)

각 태스크 스택은 높은 주소에서 낮은 주소로 하향 성장하며, 상단 128바이트에 트랩 보존 프레임이, 하단 16바이트에 런타임 오버플로 검증용 카나리 가드 워드가 배치된다.

최소 유효 스택 크기는 `STACK_GUARD_SIZE` (16B) + `STACK_TRAP_FRAME_SIZE` (128B) = **144 Bytes** (`task_create` 호출 시 검증)이다.

```
[High Address: 태스크 스택 버퍼 끝단]
  sp + 0x7C : mstatus (CSR 0x300, 초기값 0x0000_1880 -> MPP=M, MPIE=1)
  sp + 0x78 : mepc    (CSR 0x341, 초기값 task_func 진입 주소)
  sp + 0x74 : x31 (t6)
  sp + 0x70 : x30 (t5)
  ...
  sp + 0x20 : x10 (a0)
  sp + 0x1C : x9  (s1)
  sp + 0x18 : x8  (s0/fp)
  sp + 0x14 : x7  (t2)
  sp + 0x10 : x6  (t1)
  sp + 0x0C : x5  (t0)
  sp + 0x08 : x4  (tp)
  sp + 0x04 : x3  (gp)
  sp + 0x00 : x1  (ra, 초기값 task_exit_trap 루틴 주소)
[Current SP: TCB->sp가 가리키는 지점]
  ... (가용 스택 런타임 성장 영역) ...
[stack_base: 스택 버퍼 시작 주소 (16바이트 카나리 가드)]
  base + 0x0C : 0x5A5A5A5A (Guard Word 3)
  base + 0x08 : 0x5A5A5A5A (Guard Word 2)
  base + 0x04 : 0x5A5A5A5A (Guard Word 1)
  base + 0x00 : 0x5A5A5A5A (Guard Word 0)
[Low Address]
```

- **128바이트 문맥 보존 프레임 상세 규약 (TaskContext_t)**:

**스택 오프셋**
**대상 레지스터**
**ABI 명칭**
**초기 세팅값 (task_create)**
**역할 및 복원 규약**

**0x7C (124)**
mstatus
CSR 0x300
0x0000_1880
mret 실행 시 머신 모드 유지 및 MIE 활성화

**0x78 (120)**
mepc
CSR 0x341
task_func 주소
태스크 최초 진입점 또는 인터럽트 복귀 주소

**0x74 ~ 0x68**
x31 ~ x28
t6 ~ t3
0x0000_0000
임시 레지스터 (4 word)

**0x64 ~ 0x40**
x27 ~ x18
s11 ~ s2
0x0000_0000
Callee-saved 보존 레지스터 (10 word)

**0x3C ~ 0x20**
x17 ~ x10
a7 ~ a0
0x0000_0000
함수 인수 및 반환 레지스터 (8 word)

**0x1C ~ 0x18**
x9 ~ x8
s1, s0(fp)
0x0000_0000
Callee-saved 보존 레지스터 (2 word)

**0x14 ~ 0x0C**
x7 ~ x5
t2 ~ t0
0x0000_0000
임시 레지스터 (3 word)

**0x08 (8)**
x4
tp
0x0000_0000
스레드 포인터 레지스터 (1 word)

**0x04 (4)**
x3
gp
0x0000_0000
전역 포인터 레지스터 (1 word)

**0x00 (0)**
x1
ra
task_exit_trap 주소
함수 반환 시 무한 wfi 격리 루틴 주소

> **제약 검증**: `x0(zero)`는 하드웨어 고정 0이므로 제외되며, `x2(sp)`는 프레임 내에 포함되지 않고 태스크 제어 블록 오프셋 0x00(`TCB_t.sp`)에 직접 저장된다. 프레임 크기는 `_Static_assert(sizeof(TaskContext_t) == 128)`를 통해 검증된다.

### 6.3. 커널 핵심 자료구조 및 사이징 명세 (Architectural Sizing Formulas)

커널의 모든 자료구조는 컴파일 타임에 정적 바운딩되며, 총 할당 크기는 시스템 설정 매크로(`OS_MAX_TASKS`, `OS_MAX_MUTEXES`)에 비례하여 산출된다.

#### ① 태스크 제어 블록 (TCB_t, 총 24 Bytes, kernel_internal.h)

- **단위 구조체 크기**: `sizeof(TCB_t) == 24 Bytes`
- **총 메모리 산출식**:

$\text{Size of } g\_tcbs = \text{OS\_MAX\_TASKS} \times 24\text{ Bytes}$

C

```
typedef enum {
    TASK_STATE_UNUSED  = 0,
    TASK_STATE_READY   = 1,
    TASK_STATE_RUNNING = 2,
    TASK_STATE_BLOCKED = 3
} TaskState_t;

typedef struct {
    uint32_t     *sp;               /* 0x00 (4B): 저장된 스택 포인터 (ASM 오프셋 고정) */
    uint32_t      priority;         /* 0x04 (4B): 현재 동적 유효 우선순위 (0~31) */
    uint32_t      base_priority;    /* 0x08 (4B): 태스크 고유 기본 우선순위 (0~31) */
    TaskState_t   state;            /* 0x0C (4B): 태스크 수명주기 상태 */
    uint8_t      *stack_base;       /* 0x10 (4B): 스택 버퍼 최하단 주소 (카나리 검증점) */
    uint32_t      stack_size_bytes; /* 0x14 (4B): 할당된 스택 버퍼 총 바이트 수 */
} TCB_t;
```

- **오프셋 불변성 검증**:

- `_Static_assert(offsetof(TCB_t, sp) == 0, "TCB sp offset mismatch")`: 어셈블리(`context.S`)에서 0번 오프셋 역참조 보장.
- `_Static_assert(offsetof(TCB_t, stack_base) == 16, "TCB stack_base offset mismatch")`: 트랩 복귀 시 카나리 주소 로드 오프셋 보장.

#### ② 카운팅 세마포어 (Semaphore_t, 총 8 Bytes, prio_rtos.h)

- **단위 구조체 크기**: `sizeof(Semaphore_t) == 8 Bytes`

C

```
typedef struct {
    uint32_t count;   /* 0x00 (4B): 잔여 가용 자원 수 (>= 0) */
    uint32_t waiters; /* 0x04 (4B): 대기 중인 태스크 슬롯 비트맵 (Bit i == 1 -> 슬롯 i 대기) */
} Semaphore_t;
```

- **동작 제약**: `waiters`는 정적 태스크 배열 인덱스(슬롯 0 ~ `OS_MAX_TASKS-1`)를 마스킹한다. 자원이 0일 때 대기 태스크의 해당 슬롯 비트가 세트되며 태스크는 `TASK_STATE_BLOCKED`로 전이된다.

#### ③ IPCP 뮤텍스 런타임 제어 블록 (IpcpMutex_t, 총 20 Bytes, kernel/ipcp.c)

- **단위 구조체 크기**: `sizeof(IpcpMutex_t) == 20 Bytes`
- **총 메모리 산출식**:

$\text{Size of } mutexes = \text{OS\_MAX\_MUTEXES} \times 20\text{ Bytes}$

C

```
typedef struct {
    bool     configured;   /* 0x00 (1B): 초기화 시 사용자와 ceiling이 등록되었는가 */
    bool     locked;       /* 0x01 (1B): 현재 소유자가 있는가 */
                           /* [참고]: 0x02~0x03 (2B)은 4B 워드 정렬을 위해 컴파일러가 자동 패딩 삽입 */
    uint32_t owner_slot;   /* 0x04 (4B): locked일 때 소유 태스크의 고정 슬롯 (0~31) */
    uint32_t user_slots;   /* 0x08 (4B): 이 mutex를 사용할 수 있도록 등록된 슬롯 비트마스크 */
    uint32_t waiters;      /* 0x0C (4B): 천장 장벽에 의해 대기 중인 태스크 슬롯 비트마스크 */
    uint32_t ceiling;      /* 0x10 (4B): 사용자들의 base_priority 최댓값 */
} IpcpMutex_t;
```

### 6.4. 커널 전역 및 정적 상태 변수 명세 (Static Variable Registry)

커널 구동에 사용되는 모든 전역/정적 인스턴스는 BSS 영역에 정적 할당되며 부팅 시 0으로 초기화된다. 배열의 첨자는 빌드 설정 매크로(`OS_MAX_MUTEXES`, `OS_MAX_TASKS`)를 통해 가변 바운딩된다.

#### ① 기구현 정적 변수 (As-Is Baseline Registry)

**변수 식별자**
**자료형**
**정의 위치**
**크기 산출식 (Sizing Formula)**
**역할 및 설계 목적**

**mutexes[OS_MAX_MUTEXES]**
`IpcpMutex_t[]`
`kernel/ipcp.c`
$\text{OS\_MAX\_MUTEXES} \times 20\text{ B}$
정적 등록된 시스템 전역 IPCP 뮤텍스 풀

**idle_stack[1024]**
`uint8_t[1024]`
`kernel/sched.c`
$1,024\text{ B}$ 고정
우선순위 0 유휴(Idle) 태스크 전용 정적 스택

**g_tcbs[OS_MAX_TASKS]**
`TCB_t[]`
`kernel/sched.c`
$\text{OS\_MAX\_TASKS} \times 24\text{ B}$
시스템 태스크 제어 블록 테이블 (인덱스 = 슬롯 ID)

**stack_task_a[1024]**
`uint8_t[1024]`
`app/tasks.c`
$1,024\text{ B}$ 고정
응용 태스크 A 전용 정적 스택

**stack_task_b[1024]**
`uint8_t[1024]`
`app/tasks.c`
$1,024\text{ B}$ 고정
응용 태스크 B 전용 정적 스택

**g_ready_bitmap**
`uint32_t`
`kernel/sched.c`
$4\text{ B}$ 고정
준비(READY) 상태 우선순위 비트마스크

**g_current_tcb**
`TCB_t *`
`kernel/sched.c`
$4\text{ B}$ 고정
현재 CPU를 점유하여 실행 중인 태스크 TCB 포인터

**sem_a_turn**
`Semaphore_t`
`app/tasks.c`
$8\text{ B}$ 고정
응용 태스크 A 실행 순서 제어용 세마포어

**sem_b_turn**
`Semaphore_t`
`app/tasks.c`
$8\text{ B}$ 고정
응용 태스크 B 실행 순서 제어용 세마포어

**configuration_frozen**
`bool`
`kernel/ipcp.c`
$1\text{ B}$ 고정
스케줄러 기동 후 뮤텍스 설정 변경 차단 플래그

#### ② SRS 완결을 위한 추가 정적 변수 (To-Be Extension Registry)

`TCB_t`의 24바이트 크기 불변성을 유지하면서 SRS 3.1/3.3/3.5/3.6절을 충족하기 위해 BSS에 추가 배치되는 정적 변수군이다.

**변수 식별자**
**자료형**
**정의 위치**
**크기 산출식 (Sizing Formula)**
**역할 및 설계 목적**

**g_prio_to_slot[32]**
`uint8_t[32]`
`kernel/sched.c`
$32\text{ B}$ 고정
우선순위($0 \sim 31$)를 실제 TCB 슬롯 번호로 변환하는 $O(1)$ 간접 매핑 테이블 (REQ-SCH-001)

**g_sleep_remaining_ticks[OS_MAX_TASKS]**
`uint32_t[]`
`kernel/sched.c`
$\text{OS\_MAX\_TASKS} \times 4\text{ B}$
`TCB_t`(24B) 크기 변경 없이 슬롯별 잔여 슬립 틱을 관리하는 배열 (REQ-TSK-003)

**g_sleep_bitmap**
`uint32_t`
`kernel/sched.c`
$4\text{ B}$ 고정
현재 슬립 대기 중인 태스크 슬롯 비트마스크 (REQ-TMR-002)

**g_system_ticks**
`uint64_t`
`kernel/sched.c`
$8\text{ B}$ 고정
부팅 후 누적된 64비트 시스템 틱 카운터 (REQ-TMR-001)

**g_critical_nesting_count**
`uint32_t`
`kernel/sched.c`
$4\text{ B}$ 고정
중첩 임계 구역 진입 깊이 카운터 (REQ-CRI-001)

> **[설정 가능성 및 용량 제약 주석]**
> 
> 
> 
> 1. `g_tcbs[]` 및 `g_sleep_remaining_ticks[]`의 배열 크기는 동시 운용 가능한 최대 태스크 수(`OS_MAX_TASKS`)에 의해 결정되며, `mutexes[]`의 배열 크기는 보호 대상 공유 자원의 최대 개수(`OS_MAX_MUTEXES`)에 의해 결정된다.
> 2. 스케줄러 알고리즘은 최대 32개까지 아키텍처 확장을 보장하나, 실제 할당 크기는 컴파일 타임 매크로 정의를 통해 결정론적으로 바운딩된다.

### 6.5. 기준 빌드 메모리 실측 스냅샷 (Reference Build Snapshot - Baseline v2.0)

본 절은 기구현 코드 기준 **`OS_MAX_TASKS = 8`**, **`OS_MAX_MUTEXES = 8`** 조건으로 컴파일 및 링크된 공식 레퍼런스 바이너리(`build/kernel.map`)의 실제 물리 메모리 배치 스냅샷을 기록한다.

- **기준 빌드 심볼별 실측 메모리 맵**:

**객체 / 심볼 식별자**
**실측 시작 주소**
**실측 바이트 크기**
**세부 계산 내역 및 비고**

**.text (.text.boot 포함)**
0x8000_0000
0x255E Bytes (약 9.34 KiB)
실행 코드 및 인라인 문자열 상수 통합

**__bss_start**
0x8000_2560
—
BSS 클리어 시작 경계

**mutexes[8]**
0x8000_2560
160 Bytes ($0\text{xA0}$)
$8 \times 20\text{ B} = 160\text{ B}$ (끝 주소: 0x8000_2600)

**idle_stack**
0x8000_2600
1,024 Bytes ($0\text{x400}$)
유휴 태스크 전용 정적 스택 (끝 주소: 0x8000_2A00)

**g_tcbs[8]**
0x8000_2A00
192 Bytes ($0\text{xC0}$)
$8 \times 24\text{ B} = 192\text{ B}$ (끝 주소: 0x8000_2AC0)

**stack_task_a**
0x8000_2AC0
1,024 Bytes ($0\text{x400}$)
태스크 A 스택 (끝 주소: 0x8000_2EC0)

**stack_task_b**
0x8000_2EC0
1,024 Bytes ($0\text{x400}$)
태스크 B 스택 (끝 주소: 0x8000_32C0)

**g_ready_bitmap**
0x8000_32C0
4 Bytes
준비 큐 비트마스크

**g_current_tcb**
0x8000_32C4
4 Bytes
실행 중인 TCB 포인터

**sem_a_turn**
0x8000_32C8
8 Bytes
세마포어 A 인스턴스

**sem_b_turn**
0x8000_32D0
8 Bytes
세마포어 B 인스턴스

**configuration_frozen**
0x8000_32D8
1 Byte
IPCP 동결 플래그 (패딩 포함)

**__bss_end / _stack_bottom**
0x8000_32E0
0x0D80 Bytes (3,456 Bytes)
커널 정적 객체 및 태스크 스택 전체 합계

**시스템 스택**
0x8000_32E0
4,096 Bytes ($0\text{x1000}$)
`.bss` 뒤 16바이트 정렬 후 할당

**_stack_top**
0x8000_42E0
—
시스템 부트 스택 최상단 (하향 성장 기준점)

> **스냅샷 검증 결과**:
> 
> 
> 
> - `mutexes` 시작(`0x8000_2560`) + 160B = `idle_stack`(`0x8000_2600`) $\rightarrow$ **오차 0바이트 정합**
> - `idle_stack` 시작(`0x8000_2600`) + 1024B = `g_tcbs`(`0x8000_2A00`) $\rightarrow$ **오차 0바이트 정합**
> - `g_tcbs` 시작(`0x8000_2A00`) + 192B = `stack_task_a`(`0x8000_2AC0`) $\rightarrow$ **오차 0바이트 정합**
> - 6.4절 ②항의 추가 정적 변수군(총 $80\text{ Bytes}$: `32 + 32 + 4 + 8 + 4`)이 링크될 경우 `__bss_end`는 `0x8000_3330`으로 선형 확장된다.

### 6.6. 열거형 및 상태값 명세

`TaskState_t`는 `kernel/include/kernel_internal.h`에, 나머지 결과 열거형은 `kernel/include/prio_rtos.h`에 정의되어 있다. 현재 빌드의 RISC-V ILP32 환경에서 열거형 값은 32비트 부호 있는 정수형으로 평가된다.

- **태스크 상태 (TaskState_t)**:

**이름**
**값**
**의미**

`TASK_STATE_UNUSED`
0
태스크 슬롯 미사용

`TASK_STATE_READY`
1
실행 준비 상태

`TASK_STATE_RUNNING`
2
현재 실행 상태

`TASK_STATE_BLOCKED`
3
세마포어 또는 슬립 등을 기다리는 상태

- **태스크 생성 결과 (TaskCreateResult_t)**:

**이름**
**값**
**의미**

`TASK_CREATE_OK`
0
생성 성공

`TASK_CREATE_ERR_INVALID_PRIO`
-1
우선순위/슬롯 범위 오류

`TASK_CREATE_ERR_PRIO_IN_USE`
-2
해당 슬롯에 이미 태스크가 있음

`TASK_CREATE_ERR_NULL_PTR`
-3
태스크 함수 또는 스택 포인터가 NULL

`TASK_CREATE_ERR_STACK_TOO_SMALL`
-4
가드와 초기 문맥을 저장하기에 스택이 작음

`TASK_CREATE_ERR_ADDRESS_OVERFLOW`
-5
스택 끝 주소 계산 중 주소 범위 초과

- **세마포어 결과 (SemaphoreResult_t)**:

**이름**
**값**
**의미**

`SEMAPHORE_OK`
0
성공

`SEMAPHORE_ERR_INVALID_ARGUMENT`
-1
잘못된 인자

`SEMAPHORE_ERR_NO_CURRENT_TASK`
-2
유효한 현재 실행 태스크가 없음

`SEMAPHORE_ERR_INVALID_WAITER`
-3
대기자 비트맵과 태스크 상태가 일치하지 않음

`SEMAPHORE_ERR_COUNT_OVERFLOW`
-4
카운트 증가 시 정수 범위 초과

- **뮤텍스 결과 (MutexResult_t)**:

**이름**
**값**
**의미**

`MUTEX_OK`
0
성공

`MUTEX_ERR_INVALID_ID`
-1
뮤텍스 ID 범위 오류

`MUTEX_ERR_INVALID_ARGUMENT`
-2
잘못된 인자

`MUTEX_ERR_NOT_OWNER`
-3
현재 태스크가 소유자가 아님

`MUTEX_ERR_RECURSIVE`
-4
이미 소유한 뮤텍스를 다시 획득하려 함

`MUTEX_ERR_INVALID_CONTEXT`
-5
유효한 태스크 실행 문맥이 아님

`MUTEX_ERR_INVALID_WAITER`
-6
대기자 상태가 일치하지 않음

`MUTEX_ERR_NOT_CONFIGURED`
-7
뮤텍스가 설정되지 않음

`MUTEX_ERR_INVALID_USERS`
-8
사용자 슬롯 비트맵이 잘못됨

`MUTEX_ERR_CONFIGURATION_FROZEN`
-9
스케줄러 시작 후 설정 변경을 시도함

`MUTEX_ERR_ALREADY_CONFIGURED`
-10
이미 설정된 뮤텍스를 다시 설정하려 함

### 6.7. 태스크 비트맵 및 간접 매핑 운용 명세

**자료구조**
**타입**
**비트 / 인덱스 의미**
**아키텍처 운용 규칙**

**g_ready_bitmap**
`uint32_t`
`bit p` = 우선순위 $p$가 READY
De Bruijn MSB 연산으로 최고 우선순위 $p$를 $O(1)$ 추출

**g_prio_to_slot[32]**
`uint8_t[32]`
`index p` $\rightarrow$`slot i`
우선순위 $p$를 보유한 태스크의 실제 `g_tcbs[slot]` 인덱스 반환

**g_sleep_bitmap**
`uint32_t`
`bit i` = 슬롯 $i$가 SLEEP 대기 중
`timer_tick_step()`에서 슬립 중인 태스크 슬롯 식별

**Semaphore_t.waiters**
`uint32_t`
`bit i` = 슬롯 $i$가 해당 세마포어 대기 중
`semaphore_signal()`에서 우선순위가 높은 대기 태스크 슬롯 선택

**IpcpMutex_t.user_slots**
`uint32_t`
`bit i` = 슬롯 $i$가 해당 뮤텍스 사용 허가됨
뮤텍스 API에서 호출 태스크의 사용 권한 검사

**IpcpMutex_t.waiters**
`uint32_t`
`bit i` = 슬롯 $i$가 해당 뮤텍스 대기 중
unlock 시 획득 가능한 대기 태스크를 찾아 소유권 위임

- 슬롯 0(우선순위 0)은 `idle_task` 전용으로 예약된다. IPCP 뮤텍스의 사용자 마스크에서는 슬롯 0을 허용하지 않는다.
- 기준 빌드의 응용 태스크 출력 뮤텍스 사용자는 슬롯 1과 2이므로, `user_slots` 초기값은 `(1U << 1) | (1U << 2)` = `0x00000006`이다.
- `g_ready_bitmap`은 전역 초기값이 0이며, `tasks_init()`에서 우선순위 1과 2를 등록한 직후 `0x00000006`, `priortos_start()`에서 idle 태스크(우선순위 0)를 등록한 후 `0x00000007`이 된다.

### 6.8. 메모리 매핑 I/O (MMIO) 주소 명세

MMIO 매크로는 `bsp/clint.h`와 `bsp/uart.c`에 정의되어 있다. 이 레지스터들은 일반 RAM에 저장되는 C 구조체가 아니라, 지정된 물리 주소를 `volatile` 포인터로 접근하는 하드웨어 장치 제어용 레지스터이다.

#### CLINT 레지스터

CLINT 베이스 주소는 `0x0200_0000`이다. 32비트 버스 상에서 64비트 시간 값을 원자적으로 조작하기 위해 상/하위 32비트로 분할 접근한다 (`INV-ATM-010`).

**매크로**
**물리 주소**
**접근 폭**
**용도**

**CLINT_MSIP**
0x0200_0000
32 bit
소프트웨어 인터럽트(Cause 3) 요청 및 해제

**CLINT_MTIMECMP_LOW**
0x0200_4000
32 bit
타이머 비교값 하위 32비트

**CLINT_MTIMECMP_HIGH**
0x0200_4004
32 bit
타이머 비교값 상위 32비트

**CLINT_MTIME_LOW**
0x0200_BFF8
32 bit
현재 하드웨어 실시간 타이머 하위 32비트

**CLINT_MTIME_HIGH**
0x0200_BFFC
32 bit
현재 하드웨어 실시간 타이머 상위 32비트

`TIMER_INTERVAL`은 `10,000` tick으로 정의되어 있으며, 10 MHz 하드웨어 타임베이스 기준 정확히 **1ms (**$10,000\text{ cycles} / 10,000,000\text{ Hz} = 0.001\text{s}$**)** 주기 인터럽트를 예약한다.

#### UART 레지스터

UART 베이스 주소는 `0x1000_0000`이며 접근 폭은 8비트이다. LCR.DLAB 비트 설정에 따라 동일 오프셋이 상이한 기능 레지스터로 다중화(Multiplexing)된다.

**매크로**
**물리 주소**
**접근 폭**
**용도 및 DLAB 상태**

**UART_THR**
0x1000_0000
8 bit
송신 데이터 홀딩 레지스터 (DLAB=0, 쓰기 전용)

**UART_DLL**
0x1000_0000
8 bit
보레이트 제수 하위 바이트 (DLAB=1)

**UART_DLM**
0x1000_0001
8 bit
보레이트 제수 상위 바이트 (DLAB=1)

**UART_IER**
0x1000_0001
8 bit
UART 인터럽트 허용 레지스터 (DLAB=0)

**UART_FCR**
0x1000_0002
8 bit
FIFO 제어 레지스터

**UART_LCR**
0x1000_0003
8 bit
라인 제어 및 DLAB 토글 레지스터

**UART_LSR**
0x1000_0005
8 bit
라인 상태 레지스터 (TX 빈 공간 폴링 검사)

### 6.9. As-Is 구현체와 SRS 목표 규격(To-Be) 간 이행 추적 요약

1. **스케줄러 비트맵 및 인덱싱 체계**: 현행 As-Is 구현체는 배열 슬롯 비트맵(`g_ready_bitmap`의 bit $i$ = 슬롯 $i$)과 TCB 선형 순회 비교를 사용하나, 32개 태스크 확장을 위한 SRS 목표 규격(To-Be)은 우선순위 비트맵과 32바이트 간접 매핑 테이블(`g_prio_to_slot[32]`)을 결합한 De Bruijn MSB $O(1)$ 파이프라인으로 이행한다.
2. **동기화 락 관리 방식**: SRS의 경량 포인터 API(`kernel/mutex.c`)와 현행 코드의 정적 디스크립터 배열(`kernel/ipcp.c`의 `mutexes[OS_MAX_MUTEXES]`)을 병행 정의하며, 두 방식 모두 우선순위 승격 시 `g_prio_to_slot` 매핑을 원자적으로 갱신한다.
3. **동기화 프리미티브 간 격리 및 배리어**: IPCP 뮤텍스 점유 상태에서의 세마포어 대기 전이를 금지하는 `INV-SYN-012` 규칙을 확립하고, 임계 구역 탈출 시 `fence rw, rw`를 포함한 원자적 메모리 배리어 시퀀스를 강제하여 동시성 레이스를 차단한다.
4. **카나리 가드 패턴 규격**: 스택 최하단 16바이트 영역에 `0x5A5A5A5A` 4개 워드를 연속 배치하여 단일 워드 대비 오버플로 검출 신뢰도를 보장한다 (`INV-STK-007`).