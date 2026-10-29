# PrioRTOS 아키텍처 및 상세 설계 통합 명세서 (Final Master Baseline v2.2.1)

- **Project Name**: PrioRTOS (Deterministic Hard-RTOS for RISC-V Single-Core)
- **Architecture / ISA**: RV32IMC Base Integer Instruction Set
- **Target Hardware / Board**: Primary: RISC-V QEMU virt / Secondary (Porting): ESP32-C3 (via BSP Abstraction)
- **Interrupt Model**: Standard RISC-V CLINT (Core Local Interruptor)
- **Document Status**: Frozen (Baseline v2.2.1 - Pure Direct-Mapped LSB De Bruijn with Upper Guard Bit Insertion & Priority Shadowing IPCP Architecture)
- **Author / Maintainer**: 임준형

# [EXECUTIVE SUMMARY] 타깃 도메인, 핵심 셀링 포인트 및 아키텍처 차별점

### 1. 목표 타깃 도메인 (Target Domains)

PrioRTOS는 파일 시스템이나 범용 동적 네트워크 스택을 제공하는 범용 OS(FreeRTOS, Zephyr)가 아닌, **ISO 26262 ASIL-D(차량 전장) 및 DO-178C DAL-A(항공/우주)** 수준의 엄격한 타이밍 결정론과 무지터(Zero-Jitter) 실행을 요구하는 고신뢰성 제어기를 정밀 타깃으로 정의한다.

- **자율주행 및 x-by-Wire 스마트 액추에이터**: Steer-by-Wire, Brake-by-Wire 인버터 모터 FOC 정밀 제어, 라이다/레이다 고속 제어 모듈
- **UAM 및 항공우주 FCC (Flight Control Computer)**: 1kHz 주기 IMU Attitude EKF 자세 제어, 쿼드콥터 모터 믹싱, 긴급 착륙 페일세이프 제어기
- **스마트 팩토리 초정밀 액추에이터**: 마이크로초 단위 동기화를 수행하는 다관절 서보 드라이브 및 EtherCAT 슬레이브 통신 루프
- **방위산업 유도 조종 장치**: 초고속 롤/피치 핀 제어기(Fin Actuator) 및 Fail-Stop 안전 통제 시스템

### 2. 핵심 셀링 포인트 (Unique Selling Proposition)

- **완벽한 제로 동적 힙(Zero Dynamic Heap)**: 컴파일 타임 정적 파티셔닝 강제로 런타임 메모리 단편화, 외부 단편화, OOM(Out of Memory) 발생 가능성을 물리적으로 0% 차단.
- **수학적으로 증명 가능한 WCET (최악 실행 시간)**:
    - 분기 없는 LSB 드 브루인 스케줄러: 연산 시점 31번 Guard Bit 합성을 통해 조건 분기문(Branch) 0개 달성, RV32IMC 하드웨어 곱셈기 기준 **6~8 CPU 사이클 고정 (O(1) Bound)**.
    - TCB 스택 기반 우선순위 섀도잉 IPCP: 뮤텍스 획득/해제 지연 **5~8 CPU 사이클 고정**.
- **데드락 및 우선순위 역전 원천 배제**: Baker의 SRP(Stack Resource Policy) 기반 수학적 증명을 통해 상호 대기 데드락 및 연쇄 블로킹 차단. 최악 블로킹 상한은 하위 태스크의 최장 임계 구역 1개로 한정.
- **극소 메모리 풋프린트**: 커널 바이너리 4 KiB 내외, 전역 정적 제어 변수 BSS 소모량 200바이트 이하의 극단적 경량화.

### 3. v2.1 대비 v2.2.1 아키텍처 핵심 혁신 및 차별점

- **간접 매핑 테이블(`g_prio_to_slot[32]`) 완전 폐기**:
    - 과거의 우선순위 간접 역참조 배열을 제거하고, `Priority Level == Task Slot ID == Bit Index`의 1:1 순수 직접 매핑(Pure Direct Mapping) 체계로 전환.
    - 스케줄러 내 메모리 역참조 레이어 1단계 및 BSS 메모리 32바이트 영구 절감.
- **스택 기반 우선순위 섀도잉 (Priority Shadowing IPCP)**:
    - 뮤텍스 락 획득 시 비트맵 조작 및 매핑 테이블 갱신을 전면 금지(0회 조작).
    - 현재 실행(Running) 중인 태스크는 비트맵 밖에서 CPU를 점유하며, TCB 내부의 유효 우선순위(`priority`) 필드만 즉시 천장값으로 승격하고 이전 우선순위는 뮤텍스 객체에 LIFO 체이닝 백업.
    - 동일 우선순위 태스크 깨어남 시 비트맵 충돌(Collision) 및 슬롯 덮어쓰기 파괴 현상을 물리적으로 원천 봉쇄.
- **연산 시점 Guard Bit 주입(Sentinel Bit Masking)을 통한 상태 머신 순수성 확립**:
    - `g_ready_bitmap`은 예외 없이 오직 "현재 대기 중인(Ready) 태스크"만 추적하여, 모든 태스크가 실행/대기 중일 때 유효하게 `0x00000000`을 허용.
    - 스케줄러가 비트맵을 평가하는 순간 레지스터 레벨에서 최하위 31번 비트를 강제 합성(`safe_bm = g_ready_bitmap | (1U << 31)`)하여, 분기문(`if == 0`) 없이 6~8사이클 상수 시간 결정론 보장.

# PART I. 소프트웨어 요구사항 명세서 (SRS)

## 1. 개요 및 시스템 환경 제약 (System Constraints & Environment)

### 1.1. 하드웨어 플랫폼 규격 (HW Baseline - QEMU virt 기준)

- **CPU 및 ISA**:
    - 코어: RISC-V 32-bit Single Hart (Hart 0 강제 고정)
    - 확장 세트: RV32IMC (하드웨어 정수 곱셈기 필수 활용)
    - 엔디안: Little-Endian
    - 특권 모드: Single M-Mode Flat Model (커널 및 태스크 동일 M-Mode 동작, 특권 수준 전환 오버헤드 0)
- **물리 메모리 맵 / 기준 보드 QEMU virt (Physical Memory Map)**:

| **영역 명칭** | **시작 물리 주소** | **크기 (Size)** | **속성** | **설명 및 하드웨어 역할** |
| --- | --- | --- | --- | --- |
| CLINT | 0x0200_0000 | 64 KB | RW (MMIO) | Core Local Interruptor (소프트웨어 및 타이머 인터럽트 제어) |
| PLIC | 0x0C00_0000 | 64 MB | RW (MMIO) | Platform-Level Interrupt Controller (외부 인터럽트 제어) |
| UART0 | 0x1000_0000 | 256 B | RW (MMIO) | NS16550A 호환 디버그 시리얼 포트 |
| DRAM | 0x8000_0000 | 128 MB | RWX | 커널 및 애플리케이션 전체 메모리 (코드, 상수, 전역 데이터, 정적 스택) |
- **CLINT 상세 레지스터 오프셋 및 타임베이스 규격 (Hart 0 기준)**:
    - MSIP (Cause 3 전용): 0x0200_0000 (Bit 0 제어, 1 기록 시 소프트웨어 인터럽트 펜딩)
    - MTIMECMP_LOW: 0x0200_4000 / MTIMECMP_HIGH: 0x0200_4004
    - MTIME_LOW: 0x0200_BFF8 / MTIME_HIGH: 0x0200_BFFC (하드웨어 클럭: 10 MHz 고정)
    - 시스템 틱 주기: 1ms Tick = 10,000,000 Hz * 0.001s = 10,000 cycles
- **인터럽트 벡터 아키텍처 규약**:
    - mtvec 설정: Direct Mode (MODE = 0x00)로 고정하며, 베이스 주소는 trap_entry 레이블에 바인딩되고 RISC-V 특권 규격에 따라 최소 4바이트 정렬을 준수한다.
    - 단일 하드웨어 진입점: 모든 하드웨어 트랩은 원인(mcause)에 관계없이 trap_entry 단일 진입점으로 직행한다.
    - 소프트웨어 트랩 디스패치 (trap_handler 내부 mcause 판독 분기):
        - Cause 7 (0x80000007, MTIP): Top-Half 틱 디스패처로 분기.
        - Cause 3 (0x80000003, MSIP): Bottom-Half 문맥 교환기(DPC)로 분기.
        - 미인가 트랩 및 동기 예외: 진단 출력 없이 Fail-Stop 안전 동결 루틴(wfi 무한 루프)으로 직행.

## 2. 시스템 아키텍처 불변 원칙 (System Invariants)

| **ID** | **카테고리** | **불변식 명칭** | **핵심 제약 내용 (Specification)** | **검증 수단 (Verification)** | **방지 대상 결함 (Hazard)** |
| --- | --- | --- | --- | --- | --- |
| INV-MEM-001 | 메모리 | Zero Dynamic Heap | 런타임 힙 동적 할당 전면 금지, 모든 메모리 컴파일 타임 정적 배치 | 링커 스크립트 .heap=0 강제 및 .map 심볼 검사 | 외부 단편화, OOM, 가변 할당 지연 |
| INV-ALG-002 | 복잡도 | Strict O(1) Complexity | 스케줄러 가변 루프 금지, De Bruijn LSB 직접 매핑 및 Guard Bit 주입 기반 Branchless 인덱싱 강제 | 역어셈블리 내 분기·역방향 점프 정적 검사 | 대기 태스크(N) 비례 WCET 폭증 |
| INV-INT-003 | 인터럽트 | Deferred-Dispatch Trap | 하드웨어 ISR 내 직접 문맥 교환 금지, CLINT MSIP(Cause 3) 소프트웨어 트랩으로 디스패치 위임 | 정적 Call-Graph 검사, ISR 명령어 수 상한 검증 | 인터럽트 지연 폭증, CSR 복귀 상태 오염 |
| INV-TIM-004 | 타이밍 | Bounded Critical Section | 커널 API 내부 전역 인터럽트 차단(MIE=0) 구간을 고정 사이클 상한으로 제한 | Cycle-accurate 시뮬레이터 로그 분석 및 GPIO 실측 | 외부 고속 펄스/센서 유실 (Jitter) |
| INV-CAP-005 | 용량 | Static Task Bounding | 아키텍처 상한 <= 32, 1 <= OS_MAX_TASKS <= 32 정적 설정, 1 Priority = 1 Task = 1 Slot 직접 매핑 강제 | _Static_assert 범위 단언, 정적 비트마스크 중복 검출 | 다단계 FIFO 큐 전락 및 비트맵 붕괴 |
| INV-LFC-006 | 수명주기 | No Runtime Task Deletion | 커널 시동 후 동적 태스크 생성/종료/삭제 금지, 무한 루프 상주 | 공개 삭제 API 배제, 태스크 리턴 시 패닉 트랩 유도 | 자원 고아화, O(N) 자원 회수 지터 |
| INV-STK-007 | 스택 | Static Stack & Canary | 재귀·VLA 금지, 스택 최하단 0x5A5A5A5A (* 4, 16B) 카나리 배치 및 문맥 교환 시 검사 | -fstack-usage 정적 분석, 문맥 교환 시 런타임 무결성 검증 | 스택 오버플로로 인한 인접 메모리 침범 |
| INV-ERR-008 | 에러 처리 | Bounded HW Polling | 레지스터 무한 폴링 루프 금지, 고정 상한 카운터 초과 시 즉시 에러 반환 | 정적 AST 루프 검출, 무응답 하드웨어 결함 주입 테스트 | 외부 페리페럴 고장 시 CPU 프리징(Hang) |
| INV-MUT-009 | 동기화 | Deterministic IPCP Shadowing | 뮤텍스 획득 시 비트맵 조작 없이 TCB 유효 우선순위만 천장값으로 즉시 승격, 직전 우선순위 LIFO 체이닝 백업. 엄격한 LIFO 역순 해제 강제. | 결함 주입 단위 테스트, 획득/해제 사이클 수 카운팅 | 우선순위 역전 현상, 상호 대기 데드락, 스택 오염 |
| INV-ATM-010 | 드라이버 | CLINT 64-bit Atomic | 32비트 버스 상에서 64비트 타이머 레지스터 원자적 접근 시퀀스 준수 | 역어셈블리 명령어 시퀀스 검사, 롤오버 경계값 주입 | 비원자적 갱신으로 인한 가짜 타이머 인터럽트 |
| INV-CRI-011 | 동시성 | Critical Section Nesting | 중첩 진입 카운터를 유지하고 탈출 시 컴파일러/메모리 배리어를 거쳐 카운터가 0일 때만 MIE=1 복원 | 카운터 언더플로 패닉 트랩, 정적 진입/탈출 쌍 검증 | 임계 구역 조기 활성화로 인한 원자성 파괴 |
| INV-SYN-012 | 동기화 | Primitive Isolation | IPCP 뮤텍스를 점유하여 우선순위가 승격된 태스크는 세마포어 대기(semaphore_wait) 상태 전이 불가, 시도 시 즉시 패닉 | 결함 주입 단위 테스트 (mutex_lock 후 semaphore_wait 호출 검증) | 천장 오염, 간접 데드락, 우선순위 역전 복합 결함 |

## 3. 커널 서브시스템 기능 요구사항 (Subsystem Functional Requirements)

### 3.1. 스케줄러 및 인터럽트 동작 정책

- **선점형 고정 우선순위 (Preemptive Fixed-Priority)**:
    - 우선순위 0(최고 우선순위)부터 31(최저 우선순위)까지 총 32개의 고유 레벨을 운용하며, 1 Priority = 1 Task = 1 Slot 직접 매핑 원칙을 적용한다.
    - 숫자가 작을수록 높은 우선순위를 가지며(LSB 우선 체계), 비선점 임계 구역을 제외한 전 구간에서 상위 우선순위 태스크가 준비(Ready) 상태로 전이되면 선점을 실행한다.
    - **Running/Ready 상태 완전 분리 및 간접 매핑 테이블 배제**:
        - CPU를 점유하여 실행 중인 태스크(Running)는 IDLE 태스크를 포함하여 예외 없이 `g_ready_bitmap`에서 자신의 비트를 끄고(0) `g_current_tcb` 포인터로 독점 관리된다.
        - `g_ready_bitmap`의 각 비트 p는 오직 "현재 CPU를 기다리고 있는(Ready) 태스크의 슬롯 번호(p)"만을 나타내며, 실행 중인 태스크만 존재하거나 모든 태스크가 Block/Sleep 상태일 때는 유효하게 `g_ready_bitmap == 0x00000000` 상태를 가질 수 있다.
        - 간접 매핑 테이블(`g_prio_to_slot[32]`)은 영구 폐기되며, 비트 번호 p가 곧 TCB 인덱스가 된다 (`&g_tcbs[p]`).
- **Guard Bit 주입 기반 De Bruijn LSB O(1) 직접 매핑 스케줄러 (REQ-SCH-001)**:
    - 분기문(Branch)과 가변 루프를 일절 배제한 엄격한 상수 시간(O(1)) 산출 시퀀스를 따른다.
    - **1단계 (연산 시점 Guard Bit 주입 / Sentinel Bit Masking)**:
        - `g_ready_bitmap`의 원본 전역 상태는 그대로 유지한 채, CPU 레지스터 연산 시점에 최하위 31번 비트를 강제로 OR 결합한다.
            - safe_bm = g_ready_bitmap | (1U << 31)
        - 이를 통해 준비 큐에 아무도 없어 `g_ready_bitmap == 0`인 상황에서도 `safe_bm`은 수학적으로 최소 `0x80000000`을 가지므로 절대 0이 될 수 없으며, `if (bitmap == 0)` 예외 검사 분기문이 완전히 배제된다.
    - **2단계 (LSB 단일 비트 고립)**: 2의 보수 연산을 통해 대기 비트맵에서 가장 우선순위가 높은(번호가 가장 작은) 단 1비트만 고립시킨다.
        - v = safe_bm & (-safe_bm)
    - **3단계 (De Bruijn 곱셈 해싱 및 고유 인덱스 산출)**: 고립된 비트 v에 32비트 LSB De Bruijn 승수(`0x077CB531U`)를 곱하고 상위 5비트를 시프트 추출한다.
        - index = (v * 0x077CB531U) >> 27
    - **4단계 (직접 TCB 확정)**: 32바이트 룩업 테이블(`DeBruijn_LSB_LUT`)로부터 최고 우선순위 슬롯 번호를 획득하고 TCB 포인터를 즉각 결정한다.
        - highest_prio = DeBruijn_LSB_LUT[index]
        - g_next_tcb = &g_tcbs[highest_prio]
    - **결정론 보장**: 대기 중인 상위 응용 태스크가 존재하면 해당 LSB가 선택되고(31번은 무시됨), 대기 중인 응용 태스크가 전혀 없으면 자연스럽게 31번(IDLE)이 도출된다. 분기문 없이 6 ~ 8 CPU 사이클 이내에 대상 태스크가 확정된다.
- **이원화된 인터럽트 처리 (Top-Half / Bottom-Half)**:
    - **타이머 ISR (Cause 7, Top-Half)**:
        - 64비트 mtimecmp 원자적 갱신, 전역 시스템 틱 카운터 증가(`g_system_ticks++`).
        - 슬립 비트맵(`g_sleep_bitmap`)이 0이 아닐 경우 잔여 틱을 감산하고, 만료 태스크를 `g_ready_bitmap`으로 복귀(`g_ready_bitmap |= (1U << slot)`).
        - **선점 판정**: Guard Bit가 합성된 비트맵으로 추출된 준비 큐 최고 우선순위가 현재 실행 중인 태스크의 동적 유효 우선순위보다 엄격히 높은 경우(`highest_prio < g_current_tcb->priority`), CLINT MSIP에 1을 기록하여 지연된 문맥 교환(DPC)을 예약하고 mret으로 탈출한다 (WCET <= 120 cycles).
        - 타이머 ISR 내부에서의 직접적인 UART 폴링 출력은 지터 방지 및 WCET 보장을 위해 엄격히 금지된다.
    - **소프트웨어 ISR (Cause 3, Bottom-Half DPC)**:
        - CLINT MSIP 비트를 0으로 즉시 클리어한다.
        - 현재 태스크 스택에 128바이트 트랩 문맥을 보존하고 TCB 오프셋 0x00에 sp를 갱신한다.
        - 스택 하단 16바이트 카나리 가드 워드(0x5A5A5A5A * 4)의 훼손 여부를 검증한다 (TCB 오프셋 0x10의 stack_base 참조).
        - 현재 태스크가 여전히 RUNNING 상태였다면 준비 큐로 반환(`g_ready_bitmap |= (1U << g_current_tcb->task_id)`).
        - REQ-SCH-001 알고리즘으로 차기 태스크를 선별하여 `g_ready_bitmap`에서 해당 비트를 클리어하고 `g_current_tcb`를 교체한다.
        - 교체된 태스크의 스택에서 레지스터 32단어(128 Bytes)를 복원한 뒤 mret을 실행하여 제어권을 전환한다.

### 3.2. 32개 우선순위 레벨(0 ~ 31) 영역 분할 설계 (REQ-SCH-002)

```
[Highest: 0] ──────────────────────────────────────────────────────────
              ▲  [도메인 4] Safety & Critical System Tasks (0 ~ 3)
              │    - 하드웨어 긴급 방어, 전원 이상/페일세이프, 워치독 킥
              ├──────────────────────────────────────────────────────────
              │  [도메인 3] Hard Real-Time Periodic Control Tasks (4 ~ 16)
              │    - 모터 제어(FOC), 센서 융합 루프(1kHz 이상 초고속 주기 제어)
              ├──────────────────────────────────────────────────────────
              │  [도메인 2] Soft Real-Time & Communication Tasks (17 ~ 27)
              │    - CAN/UART 통신 패킷 처리, 외부 프로토콜 스택, 로깅 파싱
              ├──────────────────────────────────────────────────────────
              ▼  [도메인 1] Background & Diagnostic Tasks (28 ~ 30)
                   - 시스템 헬스 체크, 플래시 마모도 검사, 유휴 통계
[Lowest: 31] ──────────────────────────────────────────────────────────
                 [도메인 0] IDLE 태스크 고정 (31)
```

| **우선순위 범위** | **도메인 명칭** | **주 실행 주기 / 트리거** | **특성 및 허용 연산 규칙** | **대표 예시 태스크** |
| --- | --- | --- | --- | --- |
| 0 ~ 3 (4레벨) | Safety & Fault Handling | 이벤트성 (Fault 발생 즉시) | • 절대 블록(task_sleep_ticks) 금지

• IPCP 뮤텍스 소유 태스크보다 무조건 상위

• 실행 시간 상한 WCET <= 50 µs | 비상 정지(E-Stop), 과전압 차단, 외부 하드웨어 고장 페일세이프 |
| 4 ~ 16 (13레벨) | Hard Real-Time Control | 1ms ~ 10ms 고정 주기 | • IPCP 뮤텍스 공유 가능

• 지터(Jitter) 허용치 <= 10 µs

• 연산 시간 엄격히 바운딩 | 모터 FOC 정밀 각도 제어, 인버터 제어, IMU/센서 칼만 필터 |
| 17 ~ 27 (11레벨) | Soft Real-Time & Comm | 10ms ~ 100ms 또는 통신 ISR | • 세마포어 기반 대기 빈번

• 1~2틱 지연 허용

• 링 버퍼 통신 패킷 처리 | UART 커맨드 파서, 패킷 CRC 검증, 센서 텔레메트리 패킹 |
| 28 ~ 30 (3레벨) | Background & Health | 500ms 이상 비동기 | • CPU 여유 시간에만 동작

• 언제든 상위 태스크에 의해 선점 허용 | 스택 카나리 무결성 검사, 런타임 CPU 사용률 계산 |
| 31 (1레벨) | System IDLE | 상시 상주 | • task_sleep_ticks 및 mutex_lock 호출 절대 금지

• wfi 저전력 대기 무한 루프 | idle_task |

### 3.3. 부팅 시퀀스 및 예외 처리 정책

- **결정론적 부팅 시퀀스 (Step 0 ~ Step 5)**:
    - Step 0 (엔트리 진입): 링커 스크립트 RAM 시작 주소(0x8000_0000)를 기반으로 _start로 진입.
    - Step 1 (초기 스택 및 BSS 설정): sp를 _stack_top으로 설정하고 .bss 영역을 0으로 클리어 후 main() 호출.
    - Step 2 (플랫폼 및 트랩 벡터 초기화): UART를 초기화하고 trap_entry를 Direct Mode(0x00)로 mtvec에 기록.
    - Step 3 (CLINT 서브시스템 및 응용 태스크 초기화): CLINT MSIE 및 MTIE 활성화, 최초 mtimecmp 설정(mtime + 10,000). tasks_init()으로 사용자 태스크 정적 등록.
    - Step 4 (커널 시동): priortos_start()에서 슬롯 31에 idle_task를 등록(`g_ready_bitmap |= (1U << 31)`), 최고 우선순위 태스크 선정.
    - Step 5 (최초 문맥 복원): prio_context_first_switch()를 호출하여 mstatus(0x1880, MPP=M, MPIE=1) 복원 후 mret으로 최초 태스크 진입.
- **예외 처리 및 패닉 정책**:
    - 미인가 트랩(Cause 3, 7 이외): trap_handler 내부에서 wfi 무한 루프로 진입하여 Fail-Stop 안전 동결.
    - 명시적 커널 패닉(kernel_panic): mstatus.MIE 차단, UART로 [KERNEL PANIC] 출력 후 wfi 무한 루프 진입.

### 3.4. 태스크 제어 서브시스템 (Task Control Subsystem)

- **REQ-TSK-001 (정적 태스크 등록 및 스택 빌드 - task_create)**:
    - 인자 검증(prio < OS_MAX_TASKS, stack != NULL, task_func != NULL, stack_size_bytes >= 144) 수행.
    - 1:1 직접 매핑 원칙에 따라 태스크 슬롯 번호는 우선순위와 완전히 동일(slot = prio)하며, 중복 등록(`g_tcbs[prio].state != TASK_STATE_UNUSED`) 시 `kernel_panic("duplicate priority")`을 격발한다.
    - 스택 최하단 16바이트에 카나리 매직 넘버(`0x5A5A5A5A` * 4) 기록.
    - 최상단 128바이트 프레임 구성: mstatus=0x1880, mepc=task_func, ra=task_exit_trap, 범용 레지스터 0 초기화.
    - TCB 필드 갱신: sp를 프레임 시작점으로 설정, `priority = base_priority = task_id = (uint8_t)prio`, `held_mutex = NULL`, `state = TASK_STATE_READY` 설정.
    - `g_ready_bitmap |= (1U << prio)` 비트 세트.
- **REQ-TSK-002 (자발적 CPU 양보 - task_yield)**:
    - `enter_critical()` -> CLINT_MSIP = 1 -> `exit_critical()` 순서로 선점 트리거.
- **REQ-TSK-003 (지정 틱 슬립 - task_sleep_ticks)**:
    - 호출 태스크가 뮤텍스를 점유 중(`g_current_tcb->held_mutex != NULL`)일 경우 `kernel_panic("sleep with mutex")` 격발 (INV-SYN-012 준수).
    - `enter_critical()` 진입 -> `g_sleep_remaining_ticks[g_current_tcb->task_id] = ticks` 기록 -> `g_sleep_bitmap |= (1U << g_current_tcb->task_id)` 등록.
    - 현재 태스크는 비트맵 밖(Running)에 있으므로 비트 클리어 불필요 -> TCB 상태를 BLOCKED로 변경하고 MSIP = 1 트리거 -> `exit_critical()`.
- **REQ-TSK-004 (유휴 태스크 본체 - idle_task)**:
    - 최하위 우선순위 31(슬롯 31)로 고정 등록. 시스템 시동 시 `g_ready_bitmap |= (1U << 31)`로 등록되나, 실행(Running) 시 일반 태스크와 동일하게 비트맵에서 탈락한다.
    - 본체: `while(1) { __asm__ volatile("wfi"); }`. 슬립 및 뮤텍스 호출 절대 금지.
- **REQ-TSK-005 (커널 시동 - priortos_start)**:
    - idle_task 등록 -> Guard Bit가 합성된 LSB 드 브루인으로 최고 우선순위 태스크 슬롯 선정 -> `g_ready_bitmap`에서 해당 비트 클리어(Running 전환) -> `prio_context_first_switch()` 실행.

### 3.5. 상호 배제 서브시스템 (스택 기반 우선순위 섀도잉 IPCP)

- **REQ-SYN-001 (IPCP 뮤텍스 정적 초기화 - mutex_init)**:
    - 뮤텍스 객체의 `ceiling_prio`를 사전에 정의된 상수로 초기화하고, `saved_prio = 0`, `prev_mutex = NULL`로 설정. 대기 큐는 일절 할당하지 않음.
- **REQ-SYN-002 (뮤텍스 획득 - mutex_lock)**:
    - 실행 시퀀스:
        1. `enter_critical()` 진입.
        2. 호출 태스크의 기본 우선순위가 천장보다 낮으면(base_priority > ceiling_prio) 패닉 격발.
        3. LIFO 섀도잉: 이전 우선순위 백업(`m->saved_prio = g_current_tcb->priority`) 및 뮤텍스 스택 체이닝(`m->prev_mutex = g_current_tcb->held_mutex; g_current_tcb->held_mutex = m;`).
        4. 우선순위 승격: `g_current_tcb->priority = m->ceiling_prio;` (비트맵 조작 0회).
        5. `exit_critical()` 탈출 (WCET <= 7 cycles).
- **REQ-SYN-003 (뮤텍스 해제 - mutex_unlock)**:
    - 실행 시퀀스:
        1. `enter_critical()` 진입.
        2. 엄격한 LIFO 순서 검증: 호출 태스크의 최상단 보유 락이 해제 대상 뮤텍스와 일치하는지 검증(`if (g_current_tcb->held_mutex != m) kernel_panic("out-of-order mutex unlock");`).
        3. 뮤텍스 스택 팝: `g_current_tcb->held_mutex = m->prev_mutex;`.
        4. 우선순위 복원: `g_current_tcb->priority = m->saved_prio;`.
        5. 선점 판정: `sched_peek_highest_prio(g_ready_bitmap) < g_current_tcb->priority` 검사. 참일 경우 CLINT_MSIP = 1 세트.
        6. `exit_critical()` 탈출 (WCET <= 8 cycles).
- **REQ-SYN-004 (세마포어 대기 - semaphore_wait)**:
    - 뮤텍스 점유 상태(`g_current_tcb->held_mutex != NULL`)에서 호출 시 즉시 `kernel_panic("semaphore wait with mutex")` 격발 (INV-SYN-012).
    - count > 0이면 즉시 감소 후 반환. count == 0이면 호출 태스크 슬롯을 sem->waiters에 마킹하고 상태를 BLOCKED로 전이 후 MSIP = 1 요청.
- **REQ-SYN-005 (세마포어 시그널 - semaphore_signal)**:
    - waiters가 비어 있으면 count 증가. waiters가 존재하면 LSB 드 브루인으로 최우선순위 대기자를 선별하여 `g_ready_bitmap`에 등록하고, 필요 시 MSIP = 1 요청.

### 3.6. 임계 구역 및 시간 서브시스템

- **REQ-CRI-001 (enter_critical)**: `csrci mstatus, 8` -> 컴파일러 배리어 -> 중첩 카운터 증가. 오버플로 시 패닉.
- **REQ-CRI-002 (exit_critical)**: 언더플로 검사 -> 카운터 감산 -> 카운터가 0이면 컴파일러 배리어 -> `fence rw, rw` -> `csrsi mstatus, 8` 실행 (INV-CRI-011).
- **REQ-TMR-001 (get_tick_count)**: `enter_critical()` 보호 하에 64비트 `g_system_ticks` 복사 후 반환.
- **REQ-TMR-002 (timer_tick_step)**: 타이머 ISR에서 `g_system_ticks++`, 슬립 카운트 감산 및 만료자 복구 처리.

# PART II. 상세 설계 명세서 (LLD)

## 4. 디렉토리 및 파일 아키텍처

```c
PrioRTOS/
├── Makefile                               # 빌드 자동화 스크립트 (RV32IMC march/mabi, QEMU 구동)
├── README.md                              # 프로젝트 개요 및 빌드 매뉴얼
├── linker.ld                              # RAM(0x8000_0000) 섹션 배치 및 스택 분할 링커 스크립트
│
├── boot/
│   └── startup.S                          # _start 진입점: SP 초기화, .bss 0 클리어 후 main() 호출
│
├── app/
│   ├── main.c                             # 플랫폼 초기화, tasks_init(), priortos_start() 진입점
│   ├── tasks.h                            # 응용 태스크 프로토타입 선언
│   └── tasks.c                            # 응용 태스크 본체 및 task_create() 정적 바인딩
│
├── bsp/
│   ├── clint.h                            # CLINT MMIO 주소 및 타이머 상수
│   ├── clint.c                            # 타이머 인터럽트 장전, MSIP DPC 트리거/클리어
│   ├── uart.h                             # UART 16550 레지스터 오프셋 및 I/O 프로토타입
│   └── uart.c                             # UART 초기화 및 폴링 기반 입출력
│
├── kernel/
│   ├── context.S                          # 최초 문맥 복원 및 128B 레지스터 저장/복원 트랩 핸들러
│   ├── trap.c                             # mtvec 등록(Direct Mode) 및 Cause 3/7 C 디스패처
│   ├── sched.c                            # LSB De Bruijn O(1) 직접 매핑 스케줄러 코어
│   ├── mutex.c                            # 스택 기반 우선순위 섀도잉 IPCP 뮤텍스 (Lock/Unlock)
│   ├── semaphore.c                        # 카운팅 세마포어 모듈 (INV-SYN-012 격리 반영)
│   ├── panic.c                            # UART 에러 출력 및 Fail-Stop wfi 무한 루프
│   └── include/
│       ├── prio_rtos.h                    # 응용 계층 공개 API 헤더
│       ├── kernel_internal.h              # 커널 내부 구조체(TCB_t, mutex_t) 및 배리어 매크로
│       └── stack_guard.h                  # 스택 카나리(0x5A5A5A5A) 및 무결성 검증 매크로
│
└── build/                                 # 빌드 산출물 (ELF, 바이너리, 맵 파일, .o, .d)
```

## 5. 모듈별 함수 원형과 구현 명세 (LLD Synchronized Baseline v2.2.1)

### 5.1. boot/startup.S (부트 및 하드웨어 초기화)

| **상태** | **심볼 및 함수 원형** | **입출력 규약** | **담당 역할 및 제어 흐름** |
| --- | --- | --- | --- |
| [구현 완료] | `void _start(void)` | 입력: 없음

출력: 없음 (Non-returning) | 하드웨어 리셋 벡터 진입점. 부트 스택 포인터(`la sp, _stack_top`) 설정, `.bss` 메모리 0 클리어 후 `tail main` 호출. |

### 5.2. kernel/context.S (저수준 문맥 전환 및 하드웨어 트랩 진입)

| **상태** | **심볼 및 함수 원형** | **입출력 규약** | **담당 역할 및 제어 흐름** |
| --- | --- | --- | --- |
| [구현 완료] | `void trap_entry(void)` | 입력: 없음 (하드웨어 트랩 진입)

출력: 없음 | mtvec에 등록되는 단일 트랩 진입 레이블. 128바이트 프레임을 스택에 확보하여 x1~x31 레지스터 및 mepc, mstatus를 보존하고 C 핸들러(`trap_handler`)로 분기. |
| [구현 완료] | `void prio_context_first_switch(TCB_t *next)` | 입력: a0 (최초 실행 대상 태스크의 TCB 포인터)

출력: 없음 (Non-returning) | 첫 번째 태스크의 스택에서 레지스터와 CSR(`mstatus=0x1880`, mepc)을 복원하고 mret을 실행하여 태스크로 진입 (INV-LFC-006). |
| [구현 완료] | `void prio_context_switch(TCB_t *curr, TCB_t *next)` | 입력: a0 (현재 TCB), a1 (다음 TCB)

출력: 없음 | Cause 3 트랩 경로에서 현재 문맥 128B를 스택에 보존하고 `curr->sp`에 sp를 갱신한 뒤, `next->sp`로부터 레지스터를 복원하여 제어권을 전환하는 어셈블리 루틴. |
| [구현 완료] | `void task_return_guard(void)` | 입력: 없음

출력: 없음 (Non-returning) | 태스크 함수가 while(1)을 탈출하여 정상 반환을 시도할 때 즉시 `kernel_panic("task returned")`을 격발하는 방어 트랩 레이블 (INV-LFC-006). |

### 5.3. kernel/trap.c (트랩 벡터 설정 및 인터럽트 디스패치)

| **상태** | **심볼 및 함수 원형** | **입출력 규약** | **담당 역할 및 제어 흐름** |
| --- | --- | --- | --- |
| [구현 완료] | `void trap_init(void)` | 입력: 없음

출력: 없음 | `trap_entry` 레이블 주소를 Direct Mode(00b)로 CSR mtvec에 기록. |
| [구현 완료] | `void trap_handler(void)` | 입력: 없음 (트랩 진입점에서 호출)

출력: 없음 | CSR mcause 판독 후 Cause 7(타이머) 및 Cause 3(소프트웨어) 인터럽트 분기 처리. |
| [구현 완료] | `void unhandled_trap_freeze(void)` | 입력: 없음

출력: 없음 (Non-returning) | Cause 3, Cause 7 이외의 미인가 예외 발생 시 진단 출력 없이 `for(;;) { __asm__ volatile("wfi"); }`로 직행하는 안전 동결 루틴 (SRS 3.3). |

### 5.4. kernel/sched.c (LSB 직접 매핑 스케줄러 및 태스크 생명주기)

| **상태** | **심볼 및 함수 원형** | **입출력 규약** | **담당 역할 및 제어 흐름** |
| --- | --- | --- | --- |
| [구현 완료] | `TaskCreateResult_t task_create(uint32_t prio, void (*task_func)(void), uint8_t *stack, uint32_t stack_size_bytes)` | 입력: 우선순위, 태스크 함수 포인터, 스택 버퍼, 크기

출력: 성공/실패 열거형 | 스택 최하단 16B 카나리(`0x5A5A5A5A` * 4) 주입, 128B 초기 문맥 프레임 빌드, `slot = prio` 1:1 직접 매핑 바인딩 및 `g_ready_bitmap` 세트 (REQ-TSK-001). |
| [구현 완료] | `void priortos_start(void)` | 입력: 없음

출력: 없음 (Non-returning) | 31번 비트에 Idle 태스크 등록, Guard Bit 합성 LSB 드 브루인으로 최고 우선순위 태스크 선정 후 비트맵에서 탈락(Running 전환)시키고 `prio_context_first_switch` 호출. |
| [구현 완료] | `void task_yield(void)` | 입력: 없음

출력: 없음 | `enter_critical()` -> CLINT_MSIP = 1 -> `exit_critical()` 순서로 안전하게 선점 유발 (REQ-TSK-002). |
| [구현 완료] | `void idle_task(void)` | 입력: 없음

출력: 없음 (무한 루프) | 최하위 우선순위 31(슬롯 31) 유휴 태스크 본체. `while(1) { __asm__ volatile("wfi"); }` 저전력 대기 상주 (REQ-TSK-004). |
| [구현 완료] | `uint8_t sched_peek_highest_prio(uint32_t task_bitmap)` | 입력: 준비 상태 우선순위 비트마스크

출력: 최고 우선순위 슬롯 번호 (0~31) | 31번 Guard Bit 합성(`safe_bm = task_bitmap | (1U << 31)`), 2의 보수 LSB 고립(`v = safe_bm & -safe_bm`), 드 브루인 승수(`0x077CB531U`) 및 32B LUT를 결합하여 **조건 분기문 0개(Branchless)로 대상 슬롯을 O(1) 도출** (REQ-SCH-001). |
| [구현 완료] | `TCB_t* sched_select_next_task(void)` | 입력: 없음

출력: 다음 실행할 TCB 포인터 | `sched_peek_highest_prio`로 최고 대기 슬롯을 선정하고, 준비 비트맵에서 제거한 뒤 `&g_tcbs[top_slot]`으로 직접 역참조하여 반환. |
| [구현 완료] | `void sched_request_reschedule(void)` | 입력: 없음

출력: 없음 | CLINT MSIP = 1을 세트하여 지연된 문맥 교환(Cause 3 Bottom-Half) 요청. |
| [구현 완료] | `void stack_overflow_panic(void)` | 입력: 없음

출력: 없음 (Non-returning) | 스택 카나리 손상 감지 시 `kernel_panic("task stack overflow")` 격발 (INV-STK-007). |
| [구현 완료] | `static void task_exit_trap(void)` | 입력: 없음

출력: 없음 (Non-returning) | 태스크 함수가 반환되었을 때 진입하는 디버그 격리 루프. |
| [구현 완료] | `void task_sleep_ticks(uint32_t ticks)` | 입력: 대기할 틱 수 (`ticks > 0`)

출력: 없음 | 뮤텍스 점유 검증(`held_mutex != NULL` 시 패닉) -> `g_sleep_remaining_ticks[slot]` 기록 -> `g_sleep_bitmap` 등록 -> CLINT_MSIP = 1 선점 유도 (REQ-TSK-003). |
| [구현 완료] | `void timer_tick_step(void)` | 입력: 없음

출력: 없음 | 타이머 ISR(Cause 7)에서 호출되어 `g_system_ticks++`, 잔여 틱 감산, 만료자 `g_ready_bitmap` 복귀 및 선점 판정(`top_ready < current->priority`) 처리 (REQ-TMR-002). |
| [구현 완료] | `uint64_t get_tick_count(void)` | 입력: 없음

출력: 64비트 시스템 누적 틱 | `enter_critical()` 보호 하에 64비트 `g_system_ticks` 값을 원자적으로 읽어 반환하는 공개 API (REQ-TMR-001). |

### 5.5. kernel/mutex.c (스택 기반 우선순위 섀도잉 IPCP 뮤텍스 모듈)

*(참고: 기존 v2.1의 런타임 동적 설정 모듈 `kernel/ipcp.c`는 폐기되었으며, 초경량 결정론적 SRP 스택 섀도잉 로직으로 `kernel/mutex.c`에 통합됨)*

| **상태** | **심볼 및 함수 원형** | **입출력 규약** | **담당 역할 및 제어 흐름** |
| --- | --- | --- | --- |
| [구현 완료] | `void mutex_init(mutex_t *m, uint8_t ceiling_prio)` | 입력: 뮤텍스 포인터, 고정 천장 우선순위

출력: 없음 | 정적 뮤텍스 객체 초기화 (`ceiling_prio` 설정, `saved_prio = 0`, `prev_mutex = NULL`). 대기 큐 미할당 (REQ-SYN-001). |
| [구현 완료] | `void mutex_lock(mutex_t *m)` | 입력: 뮤텍스 포인터

출력: 없음 (위반 시 패닉) | `enter_critical()` -> 천장 유효성 검사(`base_prio > ceiling` 시 패닉) -> 직전 우선순위 백업(`saved_prio`) 및 LIFO 스택 푸시(`prev_mutex`) -> TCB `priority` 승격 -> `exit_critical()` (REQ-SYN-002). |
| [구현 완료] | `void mutex_unlock(mutex_t *m)` | 입력: 뮤텍스 포인터

출력: 없음 (위반 시 패닉) | `enter_critical()` -> **LIFO 해제 순서 검증**(`held_mutex != m` 시 `kernel_panic("out-of-order mutex unlock")`) -> 스택 팝 및 TCB `priority` 복원 -> 선점 재평가 후 MSIP = 1 -> `exit_critical()` (REQ-SYN-003). |

### 5.6. kernel/semaphore.c (카운팅 세마포어 모듈)

| **상태** | **심볼 및 함수 원형** | **입출력 규약** | **담당 역할 및 제어 흐름** |
| --- | --- | --- | --- |
| [구현 완료] | `SemaphoreResult_t semaphore_init(Semaphore_t *sem, uint32_t initial_count)` | 입력: 세마포어 포인터, 초기 카운트

출력: 결과 코드 | 가용 자원 수(`count`) 초기화 및 대기 슬롯 비트맵(`waiters = 0`) 설정. |
| [구현 완료] | `SemaphoreResult_t semaphore_wait(Semaphore_t *sem)` | 입력: 세마포어 포인터

출력: 결과 코드 | **INV-SYN-012 뮤텍스 점유 검사**(`held_mutex != NULL` 시 패닉) -> 자원 부족 시 태스크 슬롯 비트를 waiters에 등록하고 Blocked 전이 후 MSIP = 1 디스패치 유도 (REQ-SYN-004). |
| [구현 완료] | `SemaphoreResult_t semaphore_signal(Semaphore_t *sem)` | 입력: 세마포어 포인터

출력: 결과 코드 | count 가산. 대기자가 존재할 경우 LSB 드 브루인으로 최고 우선순위 대기자를 선별하여 `g_ready_bitmap`에 직접 복귀시키고 필요 시 선점 유발 (REQ-SYN-005). |

### 5.7. kernel/panic.c (커널 패닉 및 안전 정지)

| **상태** | **심볼 및 함수 원형** | **입출력 규약** | **담당 역할 및 제어 흐름** |
| --- | --- | --- | --- |
| [구현 완료] | `__attribute__((noreturn)) void kernel_panic(const char *msg)` | 입력: msg (출력할 에러 문자열)

출력: 없음 (Non-returning) | `csrci mstatus, 8`로 전역 인터럽트 영구 차단, UART로 `[KERNEL PANIC]`과 문자열 송신 후 wfi 무한 루프 진입 (SRS 3.3). |

### 5.8. kernel/include/kernel_internal.h (중첩 임계 구역 인라인 API 및 배리어 규약)

| **상태** | **심볼 및 함수 원형** | **입출력 규약** | **담당 역할 및 제어 흐름** |
| --- | --- | --- | --- |
| [구현 완료] | `static inline void enter_critical(void)` | 입력: 없음

출력: 없음 | `csrci mstatus, 8` -> 컴파일러 메모리 배리어 -> `g_critical_nesting_count++` -> 카운터 오버플로 시 `kernel_panic("critical overflow")` (REQ-CRI-001). |
| [구현 완료] | `static inline void exit_critical(void)` | 입력: 없음

출력: 없음 | 언더플로 검사 -> `g_critical_nesting_count--` -> 0 도달 시 컴파일러 배리어 및 `fence rw, rw` 실행 후 `csrsi mstatus, 8`로 인터럽트 원자적 복원 (REQ-CRI-002, INV-CRI-011). |

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

### 5.9. bsp/clint.c (Core Local Interruptor 하드웨어 드라이버)

| **상태** | **심볼 및 함수 원형** | **입출력 규약** | **담당 역할 및 제어 흐름** |
| --- | --- | --- | --- |
| [구현 완료] | `void clint_software_interrupt_init(void)` | 입력: 없음

출력: 없음 | 소프트웨어 인터럽트(MSIP) 비트 초기화 및 CSR `mie.MSIE` 비트 활성화. |
| [구현 완료] | `void clint_send_software_interrupt(void)` | 입력: 없음

출력: 없음 | MMIO `0x0200_0000`(MSIP0)에 1을 기록하여 Cause 3 소프트웨어 트랩 유발. |
| [구현 완료] | `void clint_clear_software_interrupt(void)` | 입력: 없음

출력: 없음 | `0x0200_0000`에 0을 기록하여 트랩 pending 비트 해제. |
| [구현 완료] | `void clint_timer_init(void)` | 입력: 없음

출력: 없음 | 첫 타이머 비교값(`mtime + TIMER_INTERVAL`) 장전 및 CSR `mie.MTIE` 비트 활성화. |
| [구현 완료] | `void clint_set_next_timer(void)` | 입력: 없음

출력: 없음 | mtimecmp 레지스터를 다음 주기 시각(+10,000)으로 재예약. |
| [구현 완료] | `uint64_t clint_get_mtime(void)` | 입력: 없음

출력: 64비트 현재 시간 | `0x0200_BFF8` 하드웨어 실시간 타이머 레지스터를 원자적으로 읽어 반환 (INV-ATM-010). |
| [구현 완료] | `void clint_set_mtimecmp(uint64_t compare_value)` | 입력: 64비트 비교값

출력: 없음 | `0x0200_4000`(mtimecmp)에 64비트 비교 시각을 원자적 시퀀스로 기록 (INV-ATM-010). |

### 5.10. bsp/uart.c (16550 호환 UART 시리얼 드라이버)

| **상태** | **심볼 및 함수 원형** | **입출력 규약** | **담당 역할 및 제어 흐름** |
| --- | --- | --- | --- |
| [구현 완료] | `void uart_init(void)` | 입력: 없음

출력: 없음 | UART 16550 보레이트(115200), 데이터 8비트, 1 스톱 비트, FIFO 활성화. |
| [구현 완료] | `void uart_putc(char c)` | 입력: c (출력할 문자)

출력: 없음 | LSR(Line Status Register) TX 빈 공간을 바운디드 폴링 확인 후 바이트 전송 (INV-ERR-008). |
| [구현 완료] | `void uart_puts(const char *str)` | 입력: str (널 종료 문자열)

출력: 없음 | 문자열 끝(`\0`)까지 순회하며 `uart_putc()`를 호출하여 시리얼 송신. |

### 5.11. app/main.c (어플리케이션 진입점)

| **상태** | **심볼 및 함수 원형** | **입출력 규약** | **담당 역할 및 제어 흐름** |
| --- | --- | --- | --- |
| [구현 완료] | `int main(void)` | 입력: 없음

출력: int | UART 초기화 -> `trap_init()` -> CLINT 인터럽트 활성화 -> `tasks_init()` -> `priortos_start()` 순차 실행. |

### 5.12. app/tasks.c (사용자 응용 태스크 본체 및 초기화)

| **상태** | **심볼 및 함수 원형** | **입출력 규약** | **담당 역할 및 제어 흐름** |
| --- | --- | --- | --- |
| [구현 완료] | `TaskCreateResult_t tasks_init(void)` | 입력: 없음

출력: 성공/실패 결과 | 세마포어/뮤텍스 정적 초기화 및 응용 태스크 A/B에 정적 스택을 바인딩하여 `task_create()`로 등록. |
| [구현 완료] | `void task_a(void)` | 입력: 없음

출력: 없음 (무한 루프) | 정적 우선순위 1번. 뮤텍스 획득 후 문자 'A'를 출력하는 고정 주기 제어 태스크. |
| [구현 완료] | `void task_b(void)` | 입력: 없음

출력: 없음 (무한 루프) | 정적 우선순위 2번. 뮤텍스 획득 후 문자 'B'를 출력하는 주기 제어 태스크. |
| [구현 완료] | `static void mock_delay(volatile uint32_t count)` | 입력: 루프 반복 횟수

출력: 없음 | 지연 시뮬레이션을 위한 고정 상한 카운트 다운 루프. |

### 5.13. 파일별 미구현 누락 항목 총괄 요약표 (구현 체크리스트)

| **대상 소스 파일** | **미구현 누락 함수 및 전역 심볼** | **추적 SRS 요구사항** | **구현 난이도 및 중요도** |
| --- | --- | --- | --- |
| `kernel/include/kernel_internal.h` | `enter_critical(void)`

`exit_critical(void)` (배리어 포함)

`g_critical_nesting_count` | REQ-CRI-001

REQ-CRI-002

INV-CRI-011 | 최상 (커널 원자성의 근간) |
| `kernel/sched.c` | `sched_peek_highest_prio(bitmap)`

`sched_select_next_task(void)`

`task_sleep_ticks(ticks)`

`timer_tick_step(void)`

`get_tick_count(void)`

`g_sleep_bitmap`

`g_sleep_remaining_ticks[]`

`g_system_ticks` | REQ-SCH-001

REQ-TSK-003

REQ-TMR-002

REQ-TMR-001 | 최상 (LSB 드 브루인 및 시간 서브시스템 완결) |
| `kernel/context.S` | `prio_context_switch`

`task_return_guard` | SRS 3.1

INV-LFC-006 | 상 (트랩 기반 문맥 교환) |
| `kernel/mutex.c` | `mutex_init`

`mutex_lock`

`mutex_unlock` (LIFO 역순 검증 포함) | REQ-SYN-001

REQ-SYN-002

REQ-SYN-003

INV-MUT-009 | 상 (스택 섀도잉 무경합 IPCP 완성) |
| `kernel/semaphore.c` | `INV-SYN-012` 뮤텍스 중첩 대기 검증 로직 반영 | REQ-SYN-004

INV-SYN-012 | 상 (동기화 프리미티브 간 격리 완성) |

# 6. 정밀 메모리 레이아웃 및 자료구조 명세

### 6.1. 128바이트 문맥 보존 프레임 레이아웃 (kernel/context.S)

```
[High Address: 태스크 스택 상단]
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
  sp + 0x00 : x1  (ra, 초기값 task_exit_trap)
[Current SP: TCB->sp가 가리키는 지점]
  ... (런타임 가용 스택 영역) ...
[stack_base: 스택 버퍼 최하단 (16바이트 카나리 가드)]
  base + 0x0C : 0x5A5A5A5A (Guard Word 3)
  base + 0x08 : 0x5A5A5A5A (Guard Word 2)
  base + 0x04 : 0x5A5A5A5A (Guard Word 1)
  base + 0x00 : 0x5A5A5A5A (Guard Word 0)
[Low Address]
```

### 6.2. 커널 핵심 자료구조 명세 (`kernel_internal.h`, `prio_rtos.h`)

#### ① IPCP 뮤텍스 구조체 (`mutex_t`, 총 8 Bytes)

C

```
typedef struct mutex {
    uint8_t       ceiling_prio; /* 0x00 (1B): 고정 천장 우선순위 (0~31) */
    uint8_t       saved_prio;   /* 0x01 (1B): 직전 유효 우선순위 LIFO 백업 */
    uint8_t       padding[2];   /* 0x02~0x03 (2B): 4바이트 정렬을 위한 명시적 패딩 */
    struct mutex *prev_mutex;   /* 0x04 (4B): 중첩 락 체이닝 포인터 */
} mutex_t;

_Static_assert(sizeof(mutex_t) == 8, "mutex_t size mismatch");
```

#### ② 태스크 제어 블록 구조체 (`TCB_t`, 총 24 Bytes)

어셈블리 루틴(`context.S`)과의 완벽한 하드웨어 ABI 정합성을 위해 포인터와 4바이트 워드 필드를 전반부(0x00~0x14)에 배치하고, 1바이트 메타데이터를 후반부(0x14~0x17)에 정렬하여 `stack_base`의 오프셋을 16 (0x10)으로 영구 불변 고정한다.

C

```
typedef enum {
    TASK_STATE_UNUSED  = 0,
    TASK_STATE_READY   = 1,
    TASK_STATE_RUNNING = 2,
    TASK_STATE_BLOCKED = 3
} TaskState_t;

typedef struct {
    uint32_t     *sp;               /* 0x00 (4B): 저장된 스택 포인터 (ASM 오프셋 0 고정) */
    mutex_t      *held_mutex;       /* 0x04 (4B): 현재 점유 중인 뮤텍스 스택 헤드 (NULL이면 미소유) */
    uint32_t      stack_size_bytes; /* 0x08 (4B): 할당된 스택 버퍼 총 바이트 수 */
    uint32_t      reserved;         /* 0x0C (4B): 4바이트 정렬 및 24B 크기 유지용 예약 필드 */
    uint8_t      *stack_base;       /* 0x10 (4B): 스택 버퍼 최하단 주소 (ASM 오프셋 16 고정, 카나리 검증점) */
    uint8_t       priority;         /* 0x14 (1B): 현재 동적 유효 우선순위 (0~31) */
    uint8_t       base_priority;    /* 0x15 (1B): 태스크 고유 불변 우선순위 (0~31) */
    uint8_t       task_id;          /* 0x16 (1B): 태스크 고유 슬롯 ID (priority와 영구 일치) */
    uint8_t       state;            /* 0x17 (1B): 태스크 수명주기 상태 (TaskState_t) */
} TCB_t;

_Static_assert(sizeof(TCB_t) == 24, "TCB_t size mismatch");
_Static_assert(offsetof(TCB_t, sp) == 0, "TCB sp offset mismatch (must be 0)");
_Static_assert(offsetof(TCB_t, stack_base) == 16, "TCB stack_base offset mismatch (must be 16)");
```

#### ③ 카운팅 세마포어 구조체 (`Semaphore_t`, 총 8 Bytes)

C

```
typedef struct {
    uint32_t count;   /* 0x00 (4B): 잔여 가용 자원 수 */
    uint32_t waiters; /* 0x04 (4B): 대기 태스크 슬롯 비트맵 (Bit i == 1 -> 슬롯 i 대기) */
} Semaphore_t;

_Static_assert(sizeof(Semaphore_t) == 8, "Semaphore_t size mismatch");
```

### 6.3. 커널 전역 정적 변수 레지스트리 (BSS 할당)

| **변수 식별자** | **자료형** | **정의 위치** | **크기** | **역할 및 설계 목적** |
| --- | --- | --- | --- | --- |
| `g_tcbs[OS_MAX_TASKS]` | TCB_t[] | kernel/sched.c | OS_MAX_TASKS * 24 B | 시스템 태스크 제어 블록 테이블 (인덱스 = 슬롯 ID = 기본 우선순위) |
| `g_ready_bitmap` | uint32_t | kernel/sched.c | 4 Bytes | 준비(READY) 상태 태스크 순수 비트마스크 (Running 태스크 제외) |
| `g_sleep_bitmap` | uint32_t | kernel/sched.c | 4 Bytes | 슬립(SLEEP) 대기 중인 태스크 슬롯 비트마스크 |
| `g_sleep_remaining_ticks[OS_MAX_TASKS]` | uint32_t[] | kernel/sched.c | OS_MAX_TASKS * 4 B | 태스크별 잔여 슬립 틱 카운터 배열 |
| `g_system_ticks` | uint64_t | kernel/sched.c | 8 Bytes | 부팅 후 누적된 64비트 시스템 틱 카운터 |
| `g_current_tcb` | TCB_t* | kernel/sched.c | 4 Bytes | 현재 CPU를 점유 중인 태스크 TCB 포인터 |
| `g_critical_nesting_count` | uint32_t | kernel/sched.c | 4 Bytes | 중첩 임계 구역 진입 깊이 카운터 |
| `idle_stack[1024]` | uint8_t[] | kernel/sched.c | 1,024 Bytes | 유휴 태스크(우선순위 31) 전용 정적 스택 |

*(참고: 기존 v2.1에 존재하던 `g_prio_to_slot[32]`(32 Bytes) 및 동적 뮤텍스 풀 `mutexes[]`는 아키텍처 다이어트를 통해 전면 삭제됨)*

### 6.4. 비트맵 운용 및 생명주기 상태 규약 (Guard Bit 결합 모델)

- `g_ready_bitmap`은 부팅 시 0으로 초기화된다.
- 응용 태스크 1, 2번 등록 후 `g_ready_bitmap = 0x00000006`이 된다.
- `priortos_start()`에서 유휴 태스크(31번) 등록 후 `g_ready_bitmap = 0x80000006`이 된다.
- 최초 태스크(1번)가 Running으로 디스패치되면 해당 비트가 제거되어 `g_ready_bitmap = 0x80000004`가 된다.
- 만약 모든 응용 태스크가 Block/Sleep 상태에 빠지고 IDLE 태스크(31번)가 CPU를 점유하여 Running 상태로 전이되면, 31번 비트마저 제거되어 `g_ready_bitmap = 0x00000000`이 된다.
- 전역 비트맵이 `0x00000000`인 상태에서도 스케줄러 코어(`sched_peek_highest_prio`)는 레지스터 내부에서 Guard Bit를 자동 주입(`safe_bm = g_ready_bitmap | (1U << 31)`)하므로, 분기문(`if == 0`) 없이 100% 안전하게 31번(IDLE) 슬롯을 선택하여 CPU를 연속 구동한다.