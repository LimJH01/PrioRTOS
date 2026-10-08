# PrioRTOS 요구사항 초안

## 문서 상태

- 상태: 초안
- 최상위 목표: 사용자 합의
- 목적: 구현 및 테스트의 기준을 합의하기 위한 요구사항 목록
- 주의: 목표는 합의되었지만, 이 문서는 인증 완료나 Hard RTOS 달성·정형 검증 완료를 주장하지 않는다.
- 미결정 설계 선택은 [design-decisions.md](./design-decisions.md)에 기록한다.

요구사항 상태:

- `확정(코드 관찰)`: 현재 소스에서 값이나 구조를 확인했으나, 그 자체로 검증 완료를 뜻하지 않는다.
- `제안`: 초기 구현을 위한 권고안이며, 사용자 확인 후 확정한다.
- `미결정`: 구현 전에 결정해야 한다.

## 1. 목표와 범위

### 프로젝트가 지향하는 목표

PrioRTOS는 FreeRTOS처럼 소형 임베디드 시스템을 위한 단순한 우선순위 기반 선점형 RTOS의 사용 철학을 지향한다. 다만 동일 우선순위 태스크 사이의 round-robin 스케줄링은 제공하지 않고, 태스크와 자원을 정적으로 구성하며, 고정 크기 ready bitmap으로 실행 태스크를 결정하는 RISC-V RTOS를 목표로 한다. IPCP를 제공하고 커널 핵심 경로의 실행시간을 유계로 설계하여 대상 하드웨어에서 측정·검증하는 것을 장기 목표로 한다.

이 목표는 설계 방향이며, 현재 구현 또는 Hard RTOS 보장 완료를 뜻하지 않는다.

### 구체화한 커널 정책

| 항목 | 목표 정책 | 상태 |
|---|---|---|
| 태스크 슬롯 | 총 8개 고정 슬롯을 둔다. 슬롯 0은 idle 전용, 슬롯 1–7은 응용 태스크용이다. | 사용자 의도 |
| 우선순위 | 숫자가 클수록 우선순위가 높다. 초기 응용 태스크 슬롯과 기본 우선순위는 1:1이며 1–7을 사용한다. 확장 시 1–31까지 허용한다. | 사용자 의도와 구체화 제안 |
| 선점 정책 | 실행 가능한 태스크 중 가장 높은 우선순위 태스크를 실행한다. 더 높은 우선순위 태스크가 준비되면 현재 태스크를 선점한다. | 목표 정책 |
| 같은 우선순위 | 응용 기본 우선순위는 고유하다. IPCP로 유효 우선순위 동률이 생기면 슬롯 번호가 작은 태스크를 선택한다(제안). | 기본 정책은 사용자 의도; 유효 우선순위 동률은 제안 |
| idle | 응용 태스크가 실행 가능하지 않을 때 슬롯 0이 실행된다. idle은 일반 응용 태스크보다 낮은 우선순위를 유지한다. | 사용자 의도 |
| 준비 bitmap | 저장형은 `uint32_t`이며 비트 i는 슬롯 i를 뜻한다. 현재 `MAX_TASKS=8`이고 이후 최대 32 slot까지 같은 저장형으로 확장할 수 있다. READY/RUNNING이면 켜고, BLOCKED/UNUSED이면 끈다. `i >= MAX_TASKS`인 비트는 항상 0이다. idle 슬롯 0은 fallback으로 항상 실행 가능해야 한다(제안). | 저장형 제안 승인; 상태/idle 규칙 구체화 제안 |
| 스케줄러 탐색 | 준비 bitmap에서 최고 우선순위를 고정 시간으로 선택한다. CLZ 하드웨어 명령은 대상 ISA가 지원하고 생성 코드로 확인된 경우에만 사용한다. | 목표 정책; ISA 선택 미결정 |
| 태스크/자원 구성 | TCB, 스택, 태스크 수 및 IPCP 자원 메타데이터에 동적 힙 할당을 사용하지 않는다. 태스크 등록은 스케줄러 시작 전만 허용하고 시작 시 설정을 동결한다(제안). | 정적 구성 의도; 등록 동결은 제안 |
| 시간 대기 | delay/periodic wait 또는 이벤트 대기로 BLOCKED 상태에 들어간 태스크는 만료/이벤트 시 READY로 복귀한다. 같은 우선순위 순환을 대신하는 기능이 아니라 의도적인 대기 기능이다. | 목표 정책 |
| 동기화 | 공유 자원은 정적으로 정의된 IPCP 자원으로 보호한다. 아래 3.4절에 제시한 ceiling/중첩/차단 규칙을 구현안으로 제안한다. | 목표 정책; 변형 승인은 미결정 |
| 실시간 보장 | O(1) 자료구조 사용만으로 Hard RTOS라고 주장하지 않는다. 인터럽트 지연, 커널 경로 WCET, 태스크 응답시간과 blocking bound를 대상에서 검증한 뒤 범위를 명시한다. | 수용 원칙 |

고정 우선순위 선점 스케줄링에서 높은 우선순위 태스크가 계속 실행 가능하면 낮은 우선순위 태스크가 지연되거나 굶을 수 있다. 이를 없애기 위해 round-robin을 몰래 추가하지 않는다. 각 응용 태스크의 실행시간·주기·deadline을 분석하고 필요하면 태스크 설계나 우선순위 배치를 조정한다.

### 초기 개발 범위 제안

- QEMU `virt`에서 기능 개발 및 반복 테스트
- RV32 단일 hart, Machine Mode 우선 지원
- 총 8개 슬롯(슬롯 0 idle, 응용 태스크 최대 7개)과 정적 스택
- 고정 우선순위 선점 스케줄링
- 동일 우선순위 round-robin 없이 명시적인 대기/깨우기와 IPCP 뮤텍스 지원
- 동적 힙, 멀티코어, 메모리 보호, 실제 항공·방산 안전 인증은 초기 범위에서 제외

실제 보드 지원과 더 강한 안전성 주장은 별도 검증 범위로 다룬다.

### 아직 수치로 확정하지 않은 값

- 타이머 입력 주파수와 tick 주기
- 각 태스크의 period, deadline, WCET 및 허용 응답시간
- 최악 인터럽트 지연과 스케줄러/뮤텍스 경로의 사이클 상한
- 최대 IPCP 자원 수, 각 자원의 ceiling 및 임계 구역 상한
- deadline miss 및 복구 불가 커널 오류 시 동작

이 값들은 근거 없이 임의로 채우지 않는다. 대상 보드와 응용 workload를 정한 뒤 요구사항과 검증 방법에 함께 추가한다.

## 2. 현재 코드에서 확인한 기준선

| 항목 | 현재 확인된 내용 | 요구사항 상태 |
|---|---|---|
| CPU ISA | Makefile이 `rv32i_zicsr`와 `ilp32`를 지정 | 확정(코드 관찰) |
| 실행 환경 | QEMU `virt` 실행 타깃이 있음 | 확정(코드 관찰); 실제 보드 지원 여부는 미결정 |
| hart 수 | 멀티코어 지원 코드가 보이지 않음 | 단일 hart 제안 |
| 태스크 최대 수 | `MAX_TASKS`가 8 | 확정(코드 관찰) |
| 우선순위 범위 | 0..7, 숫자가 큰 값이 더 높은 우선순위로 선택됨 | 확정(코드 관찰) |
| 우선순위 중복 | 현재 `task_create`는 우선순위당 태스크 하나만 허용 | 확정(코드 관찰); 정책 유지 여부 미결정 |
| ready bitmap 저장형 | `g_ready_bitmap`은 `uint32_t`; 현재 슬롯 0..7용 하위 8비트만 사용 | 목표 저장형과 일치; 향후 최대 32 slot 확장 가능 |
| 태스크/스택 할당 | 앱에서 스택 배열을 정적으로 선언하고 부팅 중 `task_create` 호출 | 확정(코드 관찰); 빌드 시 생성으로 바꿀지 미결정 |
| 타이머 | CLINT `mtime`/`mtimecmp` 접근 코드가 있음 | 확정(코드 관찰) |
| 타이머 주기 | 주석의 10 MHz 및 `10,000,000` interval은 1초에 해당하나 주석 일부는 1 ms라 설명이 불일치 | 미결정; 반드시 합의 후 수정 |
| IPCP | 뮤텍스 API 선언은 있으나 `mutex.c` 구현은 비어 있음 | 미구현 |
| 검증 | 저장소에 실제 테스트/정형검증 파일이 확인되지 않음 | 미구현 |

## 3. 기능 요구사항

아래 문장은 구현과 테스트를 위한 구체화 초안이다. `제안`으로 표시된 정책은 사용자가 승인하기 전까지 확정된 제품 요구사항이 아니다. 검증은 명시된 입력을 넣고 기대 상태/반환값/오류 동작을 확인해야 하며, 코드가 존재한다는 것만으로 통과 처리하지 않는다.

### 3.1 시스템 및 태스크

| ID | 구체적인 요구사항 | 완료 판정(테스트/분석) | 현재 상태 |
|---|---|---|---|
| `SYS-001` | 빌드는 저장소의 toolchain version lock과 Makefile 플래그로 수행한다. `make clean all`은 종료 코드 0, compiler warning 0개(`-Werror` 제안)여야 한다. 산출물은 `build/kernel.elf`와 `build/kernel.asm`이다. QEMU serial 출력은 `Hello PrioRTOS Bare-metal Boot!`, `== timer interrupt enabled ==`, 반복 timer marker `.`를 포함해야 한다. 실제 task switch 검사는 별도 test workload의 task별 marker로 확인한다. | CI에서 깨끗한 checkout에 `make clean all` 및 제한 시간 내 QEMU serial 검사를 실행한다. 위의 고정 문자열과 test workload의 각 task marker를 검사한다. toolchain version, 명령, 로그를 보관한다. 빌드만 통과하고 QEMU 검사가 없으면 부분 통과다. | 부분 구현 |
| `SYS-002` | 초기 프로파일은 **RV32, 단일 hart, M-mode, QEMU `virt`**로 제한한다(제안). 부팅 시 hart ID가 지원값이 아니거나 필수 CSR/타이머 설정이 맞지 않으면 태스크를 시작하지 않고 명시적 오류 경로로 정지한다. 다른 hart/privilege mode는 지원한다고 문서화하지 않는다. | QEMU에서 부팅, timer trap, task switch를 각각 확인한다. 지원하지 않는 hart 설정은 초기화 실패를 확인한다. 실제 보드 지원은 해당 보드별 별도 시험 없이는 통과로 세지 않는다. | 제안 |
| `TASK-001` | 초기 프로파일은 `MAX_TASKS=8`이며 슬롯 0은 idle 전용, 슬롯 1–7은 응용 태스크 전용이다. `MAX_TASKS`는 이후 8..32 범위에서 확장할 수 있으며 각 응용 슬롯 번호와 기본 우선순위는 1:1이다. `MAX_TASKS > 32`는 단일 `uint32_t` bitmap 지원 범위 밖이므로 컴파일 실패해야 한다. 성공 등록 후 동일 슬롯 재등록, 슬롯 0 응용 등록, 범위 밖 슬롯 등록은 거부하며 기존 TCB/bitmap은 바뀌지 않는다. | 초기 프로파일에서 슬롯 1과 7 등록 성공, 같은 슬롯 중복·0·8 등록 실패를 검사한다. 확장 설정 검사는 `MAX_TASKS=32` 성공, `MAX_TASKS=33` 컴파일 실패를 확인한다. 활성 범위 밖 bitmap bit는 항상 0이어야 한다. | 부분 구현 |
| `TASK-002` | TCB, 태스크 스택, idle 스택, IPCP 자원 테이블은 정적 저장소를 사용한다. 태스크 등록은 `priortos_start()` 전 초기화 구간에서만 허용하고 시작 후에는 생성/삭제를 거부한다(초기화 후 설정 동결 제안). 커널 및 BSP는 `malloc/calloc/realloc/free`에 의존하지 않는다. 각 stack base는 16바이트 정렬이며 guard와 초기 trap frame 외에 응용 사용 공간이 남아야 한다. | 링크된 ELF의 undefined symbol 및 소스 의존성 검사에서 힙 allocator 참조가 0개인지 확인한다. 시작 전 등록은 성공하고 시작 후 등록은 명시적 오류이며 TCB/bitmap 불변임을 확인한다. 각 스택 주소/크기/오버랩 경계를 검사한다. | 부분 구현 |
| `TASK-003` | 등록은 우선순위/슬롯 범위, 중복, 함수 포인터, 스택 포인터, 16바이트 정렬, 최소 크기, 주소 덧셈 overflow, 다른 등록 스택과의 겹침을 검사한다. 최소 크기는 `STACK_GUARD_SIZE + STACK_TRAP_FRAME_SIZE + TASK_MIN_USABLE_STACK_BYTES` 이상이며 `TASK_MIN_USABLE_STACK_BYTES`는 제품 설정에서 0보다 큰 값으로 지정한다. 각 실패는 `TASK_CREATE_ERR_INVALID_PRIO=-1`, `TASK_CREATE_ERR_PRIO_IN_USE=-2`, `TASK_CREATE_ERR_NULL_PTR=-3`, `TASK_CREATE_ERR_STACK_TOO_SMALL=-4`, `TASK_CREATE_ERR_ADDRESS_OVERFLOW=-5` 중 구체 원인에 맞는 결과를 반환한다. 정렬/겹침/시작 후 등록은 각각 `TASK_CREATE_ERR_STACK_MISALIGNED=-6`, `TASK_CREATE_ERR_STACK_OVERLAP=-7`, `TASK_CREATE_ERR_SCHEDULER_STARTED=-8`로 반환한다. 어느 검사에서 실패해도 TCB, guard, ready bitmap은 변경되지 않는다. | 각 조건을 하나씩 위반한 테스트가 해당 오류 코드를 받고 전역 상태가 전후 동일함을 확인한다. valid 경계 크기는 성공, 1바이트 작은 크기는 실패해야 한다. | 부분 구현 |
| `TASK-004` | 허용 전이는 `UNUSED→READY`(등록), `READY→RUNNING`(선택), `RUNNING→READY`(선점/yield), `RUNNING→BLOCKED`(wait), `BLOCKED→READY`(이벤트/시간 만료)이다. 다른 전이는 거부한다. idle은 `UNUSED→READY`로 시스템 시작 때 한 번 등록되며 block/삭제할 수 없다. Ready bitmap에서 READY와 RUNNING은 1, BLOCKED와 UNUSED는 0이다. bitmap은 정확히 `uint32_t`이며 `MAX_TASKS<=32`다. 상태와 bitmap 변경은 하나의 원자적 커널 연산이어야 한다(정책 제안). | 모든 허용 전이를 성공시키고, 전이 후 상태와 bitmap을 검사한다. 가능한 각 불허 전이는 오류를 반환하며 상태/bitmap이 바뀌지 않는지 검사한다. 항상 활성 slot에 대해 `bitmap[i] == (state[i] == READY || state[i] == RUNNING)`이고 `i >= MAX_TASKS`인 비트는 0이어야 한다. `_Static_assert(sizeof(g_ready_bitmap) == sizeof(uint32_t))`를 검사한다. | 미구현 |

### 3.2 스케줄러

| ID | 구체적인 요구사항 | 완료 판정(테스트/분석) | 현재 상태 |
|---|---|---|---|
| `SCHED-001` | 선택 대상은 상태가 READY 또는 RUNNING인 태스크다. 선택 키는 IPCP 적용 전에는 기본 우선순위, 적용 후에는 유효 우선순위다. 숫자가 클수록 우선순위가 높다. 가장 높은 유효 우선순위 태스크를 선택하며, 동률이면 슬롯 번호가 작은 태스크를 선택한다(동률 정책 제안). 선택 후 이전 RUNNING 태스크는 계속 선택되면 RUNNING, 아니면 READY가 되고 선택 대상은 RUNNING이 된다. | idle만 runnable, 응용 1개 runnable, 낮은 우선순위가 여러 개, 최고 우선순위 교체, 동률 조합을 테스트한다. 선택된 slot과 모든 상태/bitmap이 기대값과 정확히 일치해야 한다. | 부분 구현 |
| `SCHED-002` | idle 태스크는 슬롯 0이며 항상 실행 가능한 fallback이다. READY 응용 태스크가 있으면 idle보다 우선순위가 높아 실행되지 않는다. READY 응용 태스크가 없으면 idle이 RUNNING이 되고 `wfi`를 실행한다. timer interrupt가 오면 만료된 sleep 태스크를 READY로 바꾼 후 idle 또는 해당 응용 태스크를 선택한다. | 모든 응용 태스크를 BLOCKED 상태로 만든 뒤 idle 실행을 확인한다. timer interrupt 후 깨울 태스크가 없으면 다시 idle, sleep 만료 태스크가 있으면 해당 태스크로 전환되는지 확인한다. | 미구현 |
| `SCHED-003` | 상태, ready bitmap, 현재 TCB, 유효 우선순위 변경은 관찰 가능한 중간 불일치 없이 원자적으로 수행한다. MIE를 잠시 끄는 구현은 진입 시 기존 MIE를 저장하고 종료 시 기존 값으로 복구해야 한다(무조건 enable 금지). trap 진입 중 이미 MIE가 꺼져 있다면 해당 상태를 그대로 유지한다. | 각 변경 직전/직후 인터럽트를 주입하는 테스트 또는 모델 테스트를 한다. 모든 관찰 지점에서 상태/bitmap invariant가 유지되고, MIE의 이전 값이 정확히 복구되는지 검사한다. | 미구현 |
| `SCHED-004` | 태스크 선택은 `uint32_t` ready map을 사용하고, task count에 비례해 반복 횟수가 늘어나는 loop/재귀/전체 TCB scan을 금지한다. 지원 한계는 최대 32 slot이며 bit 검사 횟수는 32 이하로 고정한다. 이 알고리즘 복잡도 요구는 CPU cycle 상한 요구와 별개다. | source/CFG 및 최적화별 disassembly에서 task-count loop/재귀/TCB scan이 없고, 최악 경로가 최대 32개 bit 검사 이하인지 확인한다. cycle 상한을 주장하려면 별도 승인된 `SCHED_SELECT_MAX_CYCLES`를 실제 대상 CPU에서 측정한다. | 설계 의도, 미검증 |
| `SCHED-005` | 응용 기본 우선순위는 중복될 수 없다. 같은 유효 우선순위가 생기는 경우 슬롯 번호가 작은 태스크를 선택하고 같은 우선순위 태스크 사이를 교대로 순환하지 않는다. 새로 READY가 된 태스크가 현재 태스크보다 높은 우선순위면 다음 스케줄 지점에서 선점한다. 같은/낮은 우선순위면 현재 태스크를 선점하지 않는다. | 중복 등록 거부, effective-priority 동률 반복 실행, READY 전환에 따른 선점/비선점 케이스를 검사한다. 동일 입력 상태에서 반복 호출 결과가 동일해야 한다. | 부분 구현/미검증 |

### 3.3 시간 및 태스크 대기

| ID | 구체적인 요구사항 | 완료 판정(테스트/분석) | 현재 상태 |
|---|---|---|---|
| `TIME-001` | 각 플랫폼은 `TIMEBASE_HZ`, `TICK_HZ`, 단위, compare 값 계산/반올림 정책을 설정 파일에 명시한다. `TIMEBASE_HZ > 0`, `TICK_HZ > 0`, 한 tick의 compare 증가량이 0이 아니어야 한다. 나눠 떨어지지 않는 경우 허용 오차/누적 보정 방식을 명시하며 임의 상수를 쓰지 않는다. 현재 10 MHz/10,000,000 조합은 1초 간격이지 1ms가 아니다. | 설정값에서 계산한 tick 기간과 대상의 `mtime` 증가량을 비교한다. 허용 오차 `TIMER_TOLERANCE_TICKS` 이내인지 확인한다. 설정값, 산식, 측정 도구, 결과를 기록한다. 값이 정해지기 전에는 timer period 요구를 승인/통과 처리하지 않는다. | 미결정/불일치 |
| `TIME-002` | `task_sleep_until(deadline)`은 task context에서만 호출할 수 있으며 현재 RUNNING 태스크를 deadline까지 BLOCKED로 만들고 ready bit를 끈다. deadline은 `mtime` tick 기준이다. deadline이 현재 시각 이하이면 block 없이 성공한다. 동일 시각 deadline을 가진 태스크는 한 번의 timer 처리에서 모두 READY가 된다. `task_wake(slot)`은 task context에서만 호출할 수 있으며 명시적으로 지정한 BLOCKED 응용 태스크만 READY로 바꾼다. 반환값은 `TASK_WAKE_OK=0`, `TASK_WAKE_ALREADY_RUNNABLE=1`, `TASK_WAKE_ERR_INVALID_SLOT=-1`, `TASK_WAKE_ERR_IDLE=-2`, `TASK_WAKE_ERR_INVALID_CONTEXT=-3`으로 한다(제안). READY/RUNNING 대상의 중복 wake는 상태 변경 없이 `TASK_WAKE_ALREADY_RUNNABLE`을 반환한다. timer tick은 sleep 대기만 해제하고 별도의 event wait를 깨우지 않는다. 모든 deadline은 비교 가능한 최대 미래 거리 이내여야 하며 범위 밖 입력은 거부한다. ISR에서 깨우기가 필요하면 별도 ISR-safe API를 정의하기 전까지 허용하지 않는다. | deadline 전/동일/후, 동시 만료, 중복 wake, 잘못된 slot, idle wake, ISR에서 호출, counter wrap 경계 입력을 테스트한다. 각 테스트에서 반환값, 상태, bitmap, wake 시각을 확인한다. | 미구현 |
| `TIME-003` | 제품 태스크 구성은 각 응용 태스크에 대해 `task_id/slot`, 기본 priority, stack bytes, 실행 유형(periodic 또는 sporadic), period 또는 최소 inter-arrival, relative deadline, WCET, 초기 프로파일의 release jitter(0), 사용 자원 목록을 명시한다. 모든 시간값은 tick 단위로 변환 가능해야 하며 0/누락/단위 혼용은 거부한다. period와 deadline 간 관계는 시스템 정책으로 명시하고 임의 가정하지 않는다. | 구성 검증기가 누락/0/범위 오류/중복 slot/불가능한 자원 참조/지원하지 않는 nonzero jitter를 시작 전에 거부한다. 유효한 구성은 task별 수용 분석 입력으로 출력하고 분석 결과와 구성 해시를 함께 기록한다. | 미결정 |

### 3.4 IPCP 동기화

아래 `IPC-*`의 차단 및 중첩 규칙은 구현 가능한 **IPCP 변형 제안**이다. `DEC-007/008` 승인 전에는 프로토콜 적합성 요구로 확정하지 않는다. 제안 변형의 판정은 다음과 같다. 현재 태스크가 보유하지 않은 mutex들의 ceiling 중 최댓값을 `system_ceiling`으로 둔다(없으면 -1). 새 mutex를 잠그려면 요청 mutex가 비어 있고 현재 태스크의 유효 우선순위가 `system_ceiling`보다 커야 한다. 두 조건을 모두 만족할 때만 획득한다. 요청 자원이 사용 중이거나 ceiling 조건을 만족하지 못하면 호출 태스크는 요청 mutex를 기다리며 BLOCKED가 된다. unlock 때 대기자를 유효 우선순위 내림차순, 동률 시 slot 오름차순으로 재평가한다. 조건을 만족하는 대기자에게는 mutex 소유권을 READY 전환보다 먼저 직접 넘긴다. 대기자 선택은 고정된 slot 1–7만 검사하며 동적 큐를 할당하지 않는다.

| ID | 구체적인 요구사항 | 완료 판정(테스트/분석) | 현재 상태 |
|---|---|---|---|
| `IPC-001` | 모든 mutex는 시작 전에 정적으로 등록되고 고유한 ID를 갖는다. mutex ceiling은 그 mutex를 사용할 수 있도록 선언된 태스크들의 기본 우선순위 최댓값이다. 사용자 제공 ceiling을 허용한다면 계산값과 반드시 일치해야 한다. 사용자 목록에 없는 태스크의 lock은 거부한다. 자원 수 상한 `MAX_MUTEXES`는 설정으로 고정하고 동적 생성은 금지한다. | 정상 자원의 ceiling을 계산해 기대값과 비교한다. 중복 ID, ceiling 불일치, 없는 태스크, 0개 사용자, `MAX_MUTEXES` 초과를 초기화 실패로 검출하고 스케줄러가 시작되지 않는지 검사한다. | 미구현 |
| `IPC-002` | lock 성공 시 자원 소유자를 호출 태스크로 기록하고 태스크 유효 우선순위를 보유 자원 ceiling 중 최댓값(없으면 기본 우선순위)으로 올린다. 재귀 lock은 오류다. 여러 mutex 중첩은 `system_ceiling` 판정을 통과할 때만 허용한다. unlock은 소유자만 가능하며 비소유자 unlock은 오류이고 상태를 바꾸지 않는다. 소유자가 unlock하면 대기자를 slot 1–7 범위에서 재평가하고 자원/우선순위를 원자적으로 갱신한다. lock을 재개한 태스크의 `mutex_lock`은 소유권 이전 완료 후 성공을 반환한다. 잠금 불가 시 태스크는 BLOCKED가 된다(제안). | free lock, busy lock, system-ceiling에 의한 거부, 재귀 lock, 올바른/비소유자 unlock, 중첩/역순 unlock, 대기/소유권 이전 사례에서 소유자·상태·유효 priority·ready bitmap과 API 반환값을 검사한다. 모든 오류 호출은 상태 불변성을 유지해야 한다. | 미구현 |
| `IPC-003` | 태스크의 유효 우선순위는 `max(base_priority, ceiling of every mutex currently owned by task)`로 계산한다. 잠금 획득 시 즉시 재계산하고, 하나를 해제해도 다른 보유 mutex ceiling이 남으면 그 최댓값을 유지한다. 마지막 ceiling lock 해제 시 기본 우선순위로 복원한다. 유효 priority는 0..7 범위이며 다른 태스크의 기본 priority를 덮어쓰지 않는다. | 보유 mutex 0/1/여러 개, 높은 ceiling 해제, 마지막 해제, 대기 mutex 소유권 이전의 각 단계에서 기대 effective priority와 스케줄 선택 결과를 검사한다. | 미구현 |
| `IPC-004` | mutex API 결과는 `MUTEX_OK=0`, `MUTEX_ERR_INVALID_ID=-1`, `MUTEX_ERR_NOT_CONFIGURED=-2`, `MUTEX_ERR_NOT_OWNER=-3`, `MUTEX_ERR_RECURSIVE=-4`, `MUTEX_ERR_INVALID_CONTEXT=-5`로 정의한다(구체화 제안). mutex 획득이 즉시 불가능하면 호출 태스크에 요청 mutex ID를 기록하고 BLOCKED로 만든 뒤 현재 태스크 실행을 양보한다. 재개된 `mutex_lock`은 소유권 이전 후 `MUTEX_OK`를 반환한다. unlock 뒤 대기 태스크를 유효 우선순위 내림차순, 동률 시 slot 오름차순으로 재평가한다. mutex가 비어 있고 system-ceiling 조건을 만족하는 가장 높은 대기 태스크에게 소유권을 먼저 직접 이전한 후 READY로 만든다. 다른 mutex ceiling 때문에 기다리던 태스크도 각 unlock 후 같은 규칙으로 재평가한다. 최대 대기 태스크 수는 7이며 동적 wait queue 할당은 없다. 잘못된 mutex ID/호출 문맥/비소유자 해제는 blocking 없이 오류를 반환한다. | 한 mutex의 단일/복수 대기, ceiling 때문에 다른 mutex에서 대기, unlock 연쇄 후 wake 순서, 각 오류 코드, 재개 후 반환값을 테스트한다. unlock 뒤 소유권이 두 task에 동시에 부여되거나 소유자 없이 예약되는 순간이 없어야 한다. | 미결정 |

### 3.5 트랩, 오류 처리 및 검증

| ID | 구체적인 요구사항 | 완료 판정(테스트/분석) | 현재 상태 |
|---|---|---|---|
| `TRAP-001` | 초기 프로파일에서 지원하는 비동기 원인은 Machine Timer Interrupt(`mcause` interrupt bit=1, cause code=7) 하나로 한정한다. 그 원인은 다음 timer를 예약하고 스케줄링한 뒤 복귀한다. 다른 interrupt 및 동기 exception은 cause 값을 보존해 `kernel_panic` 경로로 보내며 task로 복귀하지 않는다. | timer cause에서 timer 재설정과 스케줄 호출을 확인한다. 대표적인 미지원 interrupt, illegal instruction, load/store fault에서 cause 진단과 non-returning panic을 확인한다. 모든 입력 cause가 처리 또는 panic 중 한 경로로 귀결돼야 한다. | 부분 구현 |
| `TRAP-002` | 트랩 프레임은 x0을 제외한 x1..x31, `mepc`, `mstatus`를 보존한다. x2/sp는 TCB가 보유한 프레임 주소로 복원되며 ABI 16-byte 정렬을 유지한다. 첫 실행 프레임과 인터럽트 복귀 프레임은 같은 오프셋/크기 계약을 쓴다. 태스크 레지스터는 커널 C 핸들러 호출 중 보존되어야 한다. | 각 태스크가 고유 패턴을 레지스터에 두고 반복 timer preemption 후 패턴, `mepc`, `mstatus`, sp 정렬이 유지되는 QEMU/타깃 테스트를 한다. 어셈블리 offset과 C `sizeof/offsetof` 정적 검사를 통과한다. | 구현 존재, 미검증 |
| `ERR-001` | 각 태스크 스택 하단에는 정확히 16-byte(4×32-bit) canary를 둔다. trap 진입 시 프레임 128-byte를 확보하기 전에 `sp`가 guard+frame 경계를 넘지 않는지 검사하고, 프레임 저장 후 canary 네 단어를 모두 검사한다. 하나라도 불일치하거나 headroom이 부족하면 task 실행으로 복귀하지 않고 system stack에서 panic한다. | canary 각 단어별 손상, 경계 직전/경계 초과 sp를 주입한다. 정상 입력은 계속 실행되고 손상 입력은 stack panic으로 귀결되며 손상 태스크로 복귀하지 않는지 확인한다. | 부분 구현 |
| `ERR-002` | `kernel_panic(code, detail)`은 MIE를 비활성화하고 원인 코드를 기록한 뒤 반환하지 않는다. UART 진단은 `UART_PANIC_TIMEOUT_CYCLES` 이내에만 시도하고 timeout이면 출력을 포기해 WFI 정지로 진행한다. 추가 스케줄링과 태스크 복귀는 금지한다. 현재 WFI 정지 정책은 프로토타입 기본값이며 reset/외부 안전 신호 정책은 제품별로 승인한다. | panic 원인별로 `mstatus.MIE=0`, panic 진입 후 task progress가 없음, 반환하지 않음을 시험한다. UART ready와 permanently-not-ready 경우 모두 panic이 timeout 이내에 non-returning halt에 도달하는지 확인한다. | 부분 구현 |
| `ERR-003` | deadline miss 정책은 `DEADLINE_MISS_POLICY` 설정에 명시한다. 설정 선택 전에는 deadline 감시 기능/제품 수용을 활성화하지 않는다. 정책은 최소한 miss 검출 시각, 해당 task 식별, 기록 방식, task 계속 실행 여부, 시스템 정지/reset 여부를 지정해야 한다. 누락 또는 지원하지 않는 정책 값은 빌드/초기화 실패다. | 기한 안 완료와 기한 초과를 각각 주입해 정책에 명시된 기록 및 상태 변화를 검사한다. 설정 누락/잘못된 값이 배포 이미지를 만들지 못하게 하는지 검사한다. | 미결정 |
| `TIME-DET-001` | 측정 경로를 각각 구분한다: interrupt entry부터 첫 handler 명령까지의 latency, trap save/restore, scheduler select, mutex lock/unlock, 최대 IRQ-disabled 구간. 보고서에는 대상 CPU/보드, clock/timebase, compiler/binutils 버전, 최적화 옵션, 입력 상태, 측정 방법/오차, 최댓값을 기록한다. QEMU 결과를 하드웨어 WCET 측정으로 취급하지 않는다. | 각 경로별로 측정 범위와 최악 입력 조합을 지정하고, 정적 분석/실측 결과 및 재현 명령을 보관한다. 측정하지 않은 경로는 `미측정`으로 남긴다. | 미구현 |
| `TIME-DET-002` | 승인된 단일 hart fixed-priority workload의 각 task에 대해 execution time `C_i`, period/minimum inter-arrival `T_i`, relative deadline `D_i`, IPCP blocking bound `B_i`를 포함해 response-time 분석을 한다. 제안 기본 프로파일은 constrained-deadline workload(`0 < D_i <= T_i`), release jitter 0으로 제한한다. 이 프로파일에서 우선순위가 i보다 높은 태스크 집합 `hp(i)`에 대해 `R_i^(n+1) = C_i + B_i + Σ_{j∈hp(i)} ceil(R_i^n/T_j) C_j`를 계산한다. IPCP 차단 bound는 task i를 ceiling 접근 규칙으로 막을 수 있는 lower-priority 임계 구역 시간의 최댓값이며, 그런 구간이 없으면 0이다. 값이 수렴해 `R_i <= D_i`면 수용하고, `R_i > D_i` 또는 분석 설정 상한 초과면 거부한다. 임의 deadline(`D_i > T_i`)과 nonzero release jitter는 별도 분석법 승인 전 지원 범위 밖이다. | 모든 task의 입력, 분석 식/도구/버전, 각 반복 결과, 최종 `R_i`, `D_i`, `B_i`를 출력한다. 입력 누락, 비수렴, 분석 상한 초과 또는 `R_i > D_i`가 하나라도 있으면 실패한다. `C_i`는 승인된 WCET 근거에서만 가져오고 평균시간은 받지 않는다. | 미결정 |
| `TIME-DET-003` | 모든 MIE 비활성 구간은 시작/종료 지점이 코드에서 식별 가능하고 중첩 시 이전 상태를 복구해야 한다. 해당 구간의 최대 사이클은 대상 설정의 `MAX_IRQ_OFF_CYCLES` 이하이어야 한다. panic에서의 비복귀 정지는 정상 critical section 측정에서 제외하되 panic 상태임을 명시한다. | 소스/assembly에서 MIE clear 구간 전부를 목록화하고 대상에서 최대 길이를 계측한다. 누락된 구간 또는 상한 초과가 있으면 실패한다. 상한 수치가 지정되지 않으면 요구사항은 미승인이다. | 미구현 |
| `TEST-001` | 모든 SYS/TASK/SCHED/TIME/IPC/TRAP/ERR/TIME-DET 요구사항 ID는 코드 위치와 하나 이상의 검증 항목으로 추적 가능해야 한다. 정적 분석형 요구사항은 테스트 대신 승인된 분석 산출물을 연결할 수 있다. | 추적표에서 구현 요구사항 ID의 누락/중복/존재하지 않는 test ID가 0개여야 한다. 테스트 변경 시 관련 trace를 함께 갱신한다. | 미구현 |
| `TEST-002` | 각 public API 및 상태 전이에는 정상, 최소/최대 경계, 잘못된 입력, 실패 후 상태 불변성 테스트가 있어야 한다. 하드웨어 의존 경로에는 성공/오류 원인별 통합 테스트를 둔다. | 요구사항별 test case 목록에서 해당 범주가 채워졌는지 자동 검사한다. 오류 테스트는 반환 코드뿐 아니라 TCB, bitmap, owner, priority 등 전역 상태가 보존되는지도 확인한다. | 미구현 |
| `TEST-003` | 검증 결과는 `HOST`, `QEMU`, `TARGET` 중 실행 환경을 표시한다. HOST는 순수 로직 테스트, QEMU는 ISA/부팅/트랩 기능 테스트, TARGET은 보드별 시간/하드웨어 동작 검증으로 분류한다. 한 환경의 통과를 다른 환경의 통과로 대체하지 않는다. | 모든 결과에 환경, 보드/가상 플랫폼, 툴 버전, commit ID, 설정, 실행 명령, pass/fail 및 로그 위치를 기록한다. TARGET 실측 없이 WCET/Hard-RT 보장 표시는 금지한다. | 미구현 |

## 4. 제품별로 값 확정이 필요한 설정

구현 규칙은 위와 같이 테스트 가능하게 정의했지만, 다음 값은 프로젝트 코드만 보고 정할 수 없다. 각 값이 승인되기 전까지 관련 요구사항은 `제안` 또는 `미결정`이지 `확정`이 아니다.

| 설정 | 필요한 값 | 영향을 받는 ID | 미확정 시 처리 |
|---|---|---|---|
| 플랫폼 프로파일 | hart 수, privilege mode, QEMU/보드, 지원 interrupt | `SYS-001`, `SYS-002`, `TRAP-001` | 프로토타입은 RV32/QEMU virt/single-hart/M-mode 제안으로만 검증 |
| 태스크 자원 한계 | 초기 `MAX_TASKS=8`, bitmap 제한 `MAX_TASKS<=32`, 최소 usable stack, stack별 크기, 등록 동결 시점 | `TASK-001`–`TASK-003` | 구성 검증에서 누락을 오류로 처리 |
| 타이머 | timebase Hz, tick Hz, 허용 오차, counter wrap 비교 범위 | `TIME-001`, `TIME-002` | 시간 대기 수용 테스트 미승인 |
| workload | task별 period/inter-arrival, deadline, WCET, 자원 사용표, 분석 프로파일(`D<=T` 제안) | `TIME-003`, `TIME-DET-002` | 실시간 수용성 분석 불가 |
| 빌드/오류 진단 | toolchain version lock, warning 정책, `UART_PANIC_TIMEOUT_CYCLES` | `SYS-001`, `ERR-002` | CI/진단 완료 판정 미승인 |
| IPCP | `MAX_MUTEXES`, 제안한 block/ceiling/tie 규칙 승인 | `IPC-001`–`IPC-004` | mutex 요구사항 미승인, 구현 완료로 간주 금지 |
| 이벤트/대기 API | `task_sleep_until` 및 `task_wake` API의 공개 여부와 호출 문맥 | `TIME-002`, `TASK-004` | API 계약 승인 전 timer/event wait 구현 미승인 |
| 안전 반응 | deadline miss 및 panic의 제품별 정지/reset/알림 정책, `UART_PANIC_TIMEOUT_CYCLES` | `ERR-002`, `ERR-003` | 제품/안전 동작 수용 불가 |
| 시간 상한 | 선택 경로/IRQ-off/WCET 한계와 대상 CPU | `SCHED-004`, `TIME-DET-001`–`TIME-DET-003` | 복잡도 외 시간 보장 주장 금지 |

## 5. 초기 프로토타입 완료 기준

아래를 모두 충족해야 초기 기능 프로토타입 완료로 판정한다. 제품 실시간 적합성이나 안전 인증 완료를 의미하지 않는다.

1. `make clean all` 및 QEMU 부팅/반복 timer preemption이 자동으로 확인된다.
2. 8개 슬롯 중 idle 1개와 응용 최대 7개를 등록하며, 잘못된 구성은 실행 전에 거부된다.
3. 모든 task state transition 뒤 ready bitmap invariant가 유지되고, highest-priority/idle/tie 동작 테스트가 통과한다.
4. timer/event wake 경계 테스트가 통과하고 설정된 tick과 측정값의 차이가 허용 오차 이내다.
5. 승인된 IPCP 변형을 구현한 경우 lock/unlock, ceiling, 중첩, 오류, block/wakeup 테스트가 통과한다.
6. trap register-preservation, stack guard/headroom 손상, unsupported cause, panic 경로 테스트가 통과한다.
7. 요구사항 추적표의 미연결 ID가 0개이며, QEMU 기능 검증과 실제 target 시간 검증 결과가 분리돼 있다.
8. 제품 workload와 대상 하드웨어, timing limits가 확정되기 전에는 Hard-RTOS/WCET 수용 완료로 표시하지 않는다.

## 6. 요구사항 변경 절차

1. 설계 선택을 [design-decisions.md](./design-decisions.md)에 기록한다.
2. 요구사항 표의 `미결정` 항목을 결정하고 담당자/날짜를 남긴다.
3. 요구사항 ID를 구현 코드와 테스트에 연결한다.
4. 구현·테스트가 합의된 기준을 통과한 뒤 상태를 갱신한다.
5. 대상 CPU, 보드, 컴파일러 또는 타이밍 설정이 바뀌면 영향받는 요구사항과 테스트를 재검토한다.
