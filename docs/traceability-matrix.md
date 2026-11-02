# PrioRTOS 단계별 검증 및 추적성 매트릭스

이 문서는 [요구사항 카탈로그](./requirements.md)의 각 요구사항을 설계 영역,
구현 위치, 검증 사례 및 검증 방식에 연결한다. 행에 구현 파일이 적혀 있다는
사실만으로 요구사항 충족이나 검증 완료를 의미하지 않는다.

## 상태 및 검증 단계

| 상태 | 의미 |
|---|---|
| `구현` | 관련 코드 경로가 확인됨. 요구사항 전체의 충족을 뜻하지 않는다. |
| `부분` | 일부 구현은 있으나 요구사항과 불일치하거나 필요한 범위가 빠져 있다. |
| `미구현` | 현재 저장소에서 요구 동작을 확인할 수 없다. |
| `미검증` | 자동 테스트 또는 대상 환경에서의 확인 결과가 아직 없다. |

검증 사례 ID(`TC-*`)는 이 문서에서 제안하는 추적 식별자이며, 자동화된 테스트
파일이나 통과 결과가 이미 있다는 뜻이 아니다. 현재 저장소에는 테스트 파일 및
`verification/` 스크립트가 없고, `make cbmc`와 `make trace`는 해당 스크립트를
호출하도록 선언되어 있을 뿐이다.

## 요구사항 추적성 매트릭스

| 요구사항 ID | 요구사항 요약 | 서브시스템 / 명세 영역 | 구현 API / 파일 | 검증 사례 ID | 테스트 검증 방식 | 현재 상태 |
|---|---|---|---|---|---|---|
| `SYS-001`, `SYS-002` | RV32IMC / QEMU virt, Hart 0, 10 MHz timebase 기준 플랫폼 | 플랫폼 및 타깃 구성 | `Makefile`; `linker.ld`; `boot/startup.S` | `TC-SYS-001` | 컴파일 플래그, ELF 속성, QEMU machine/hart 설정 및 CLINT timebase를 검사한다. FE310/실제 보드 포팅은 별도 검증 대상이다. | **부분** — 기준 문서와 Makefile은 정렬했으며 ELF/QEMU 실행 검증이 남아 있음. |
| `TIME-001` | CLINT `mtime` / `mtimecmp` 사용 | 타이머 및 시간 기준 | `clint_timer_init()`, `clint_get_mtime()`, `clint_set_mtimecmp()`, `bsp/clint.c`, `bsp/clint.h`, `kernel/trap.c` | `TC-TIME-001` | QEMU 및 실제 타깃에서 `mtime` 증가와 `mtimecmp` 인터럽트 발생/재설정을 확인한다. | **부분** — CLINT 접근 경로는 구현됐으나 목표 보드에서 검증되지 않음. |
| `TIME-002` | 1 kHz, 1 ms 시스템 tick | 타이머 및 시간 기준 | `TIMER_INTERVAL`, `clint_timer_init()`, `clint_set_next_timer()` in `bsp/clint.c`; 타이머 처리 in `kernel/trap.c` | `TC-TIME-002` | 10 MHz `mtime` timebase와 연속 인터럽트 간 10,000 count 간격을 계측한다. | **부분** — `TIMER_INTERVAL=10000ULL`은 정렬됐지만 timer tick 처리와 overrun 검증이 남아 있음. |
| `TIME-003` | 서브밀리초 작업을 시스템 tick과 분리 | 타이머 및 시간 기준 | 현재 대응 API 없음; `bsp/clint.c`, `kernel/trap.c` | `TC-TIME-003` | 요구되는 PWM/하드웨어 직접 인터럽트 경로가 설계·구현되었는지 확인하고, 대상 장치에서 트리거 지터를 계측한다. | **미구현** |
| `TASK-001` | 최대 32개, 동적 생성·소멸 없음 | 태스크 및 정적 등록 | `MAX_TASKS`, `g_tcbs[]`, `task_create()` in `kernel/include/kernel_internal.h`, `kernel/sched.c`; 정적 스택 in `app/tasks.c` | `TC-TASK-001` | 32개 슬롯 경계에서 등록 성공/실패, 슬롯 중복 등록 거부 및 런타임에 태스크 테이블이 추가 변경되지 않는지 검사한다. | **부분** — 32개 슬롯의 고정 크기 TCB 및 부팅 시 등록 구조는 구현. 슬롯 31은 `priortos_start()`가 idle에 사용하므로 응용 태스크는 최대 31개이며 현재 실제 응용 태스크는 2개다. 자동 경계 테스트 없음. |
| `TASK-002` | 우선순위 0–31, 중복 금지 | 태스크 및 정적 등록 | `task_create()` in `kernel/sched.c`; `TaskCreateResult_t` in `kernel/include/prio_rtos.h` | `TC-TASK-002` | 범위 밖 우선순위와 동일 슬롯 재등록을 입력하고 결과 코드 및 TCB/ready bitmap 불변성을 검사한다. | **구현 / 미검증** — 우선순위를 슬롯 인덱스로 사용하며 0–31 범위와 사용 중 슬롯을 검사한다. |
| `TASK-003` | 우선순위 0은 idle 전용 | 태스크 및 정적 등록 | `IDLE_TASK_PRIORITY`, `idle_task()`, `priortos_start()` in `kernel/include/kernel_internal.h`, `kernel/sched.c` | `TC-TASK-003` | 스케줄러 시작 때 idle 슬롯이 등록되고, 모든 응용 태스크가 BLOCKED일 때 idle로 진행하는지 QEMU에서 확인한다. | **부분 / 미검증** — idle 등록은 구현됐으나 모든 응용 태스크가 BLOCKED인 경우의 자동 검증 없음. |
| `SCHED-001` | `uint32_t` 준비 비트맵, 비트가 태스크 슬롯을 표시 | 스케줄러 상태 및 준비 집합 | `g_ready_bitmap`, block/wake 경로 in `kernel/include/kernel_internal.h`, `kernel/sched.c`, `kernel/semaphore.c`, `kernel/ipcp.c` | `TC-SCHED-001` | 태스크 READY/BLOCKED/RUNNING 전이에 따른 비트와 TCB 상태를 함께 검사한다. 타입과 비트 의미가 요구사항 정의와 일치하는지도 검사한다. | **부분** — 구현의 타입과 슬롯 비트 의미는 요구사항과 일치한다. 유효 우선순위가 슬롯과 달라질 수 있는 IPCP 승격 상황은 별도 추적이 필요하다. |
| `SCHED-002` | 최고 우선순위 선택을 O(1)로 수행 | 스케줄러 선택 정책 | `sched_highest_priority_slot()`, `sched_schedule()` in `kernel/sched.c` | `TC-SCHED-002` | 비트맵/준비 집합 여러 조합에서 선택 결과를 확인하고, 생성 어셈블리 및 타깃 사이클 계측으로 최악 실행시간을 확인한다. | **부분** — 선택 함수는 최하위 set bit를 De Bruijn LUT로 O(1)에 추출한다. 다만 현재 IPCP의 동적 effective priority와 bitmap bit index의 정합성 및 Running bit 제거는 추가 검증이 필요하다. |
| `SCHED-003` | 선점형 고정 우선순위, 동순위 RR 없음 | 스케줄러 선택 정책 | `sched_schedule()`, `trap_handler()`, `task_yield()` in `kernel/sched.c`, `kernel/trap.c`; `trap_entry` in `kernel/context.S` | `TC-SCHED-003` | 높은 우선순위 태스크의 READY 전환 시 선점, 낮은 우선순위 유지, 동순위일 때 현재 태스크 유지 및 timer/MSIP 문맥 복귀를 검사한다. | **부분 / 미검증** — 정책 경로는 구현됐으나 자동 상태 전이 및 타깃 테스트 없음. |
| `IPC-001` | 정적 Mutex 및 사용자 기준 ceiling | IPCP 및 정적 자원 설정 | `mutex_configure()` 및 mutex 테이블 in `kernel/ipcp.c`; 공개 타입/API in `kernel/include/prio_rtos.h`; 호출 예 `app/tasks.c` | `TC-IPC-001` | 사용자 슬롯 유효성, ceiling 계산, 중복 설정 및 `priortos_start()` 이후 설정 동결을 확인한다. 설정된 ceiling이 사용자 태스크 기본 우선순위 최솟값과 일치하는지 검사한다. | **부분 / 미검증** — 고정 테이블과 설정 API가 있으나 ceiling은 초기화 시 사용자 비트맵에서 계산된다. 자동 테스트 없음. |
| `IPC-002` | Mutex 획득 시 ceiling으로 즉시 승격 | IPCP 우선순위 관리 | `mutex_lock()`, `can_acquire()`, `refresh_effective_priority()` in `kernel/ipcp.c` | `TC-IPC-002` | lock 성공 직후 effective priority 및 스케줄러 선택을 확인한다. 잠금이 불가능한 경우는 block/handoff 정책대로 동작하는지 별도 검사한다. | **부분 / 미검증** — 성공적으로 획득한 경우 승격하며, 획득이 불가능하면 block될 수 있다. 요구사항의 “블로킹 없이”와 동작 전제가 맞는지 정합성 결정이 필요하다. |
| `IPC-003` | unlock 시 base/다른 보유 mutex 기준 우선순위 복원 | IPCP 우선순위 관리 | `mutex_unlock()`, `refresh_effective_priority()` in `kernel/ipcp.c` | `TC-IPC-003` | 단일·다중 mutex 획득 및 서로 다른 순서의 해제 후 effective priority가 base 또는 남은 ceiling 최솟값으로 복원되는지 검사한다. | **부분 / 미검증** — 보유 mutex를 재계산하는 경로는 구현됐으나 자동 테스트 없음. |
| `IPC-004` | Mutex 중첩 최대 4단계 | IPCP 중첩 정책 | 최종 `mutex_init()`, `mutex_lock()`, `mutex_unlock()`; `OS_MAX_MUTEX_NESTING` in `kernel/include/kernel_internal.h` | `TC-IPC-004` | 서로 다른 mutex 1~4단계 획득은 허용되고 5단계는 즉시 오류 처리되는지 확인한다. | **미구현** — 상수 `OS_MAX_MUTEX_NESTING=4`는 존재하지만 최종 정적 Mutex API와 제한 검사가 아직 연결되지 않음. |
| `IPC-005` | 낮은 우선순위 임계 구역 하나로 blocking bound 제한 | IPCP 블로킹 및 시간 분석 | `mutex_lock()`, `mutex_unlock()`, waiter handoff in `kernel/ipcp.c` | `TC-IPC-005` | 정의된 경쟁 태스크/자원 시나리오에서 최악 blocking time을 측정하고 분석 상한과 비교한다. 임계 구역 WCET 및 대상 클럭 가정을 기록한다. | **미검증** — 코드만으로 시간 상한을 입증할 수 없고 WCET/대상 계측 결과도 없음. |
| `MEM-001` | TCB와 태스크 스택의 정적 BSS 배치 | 메모리 맵 및 정적 메모리 | `g_tcbs[]` in `kernel/sched.c`; 스택 배열 in `app/tasks.c`; idle stack in `kernel/sched.c`; BSS in `linker.ld` | `TC-MEM-001` | ELF/map의 심볼 주소와 섹션을 검사해 TCB 및 태스크 스택이 BSS에 있고 동적 할당이 아닌지 확인한다. | **부분 / 미검증** — 정적 배열과 BSS 배치는 코드상 확인되며 산출물 검사 결과는 기록되지 않음. |
| `MEM-002` | 부팅 후 `malloc()`/`free()` 및 힙 의존 금지 | 메모리 맵 및 정적 메모리 | `kernel/`, `app/`, `bsp/` 전체; 링크 산출물 `build/kernel.elf`, `build/kernel.map` | `TC-MEM-002` | 소스 및 링크 심볼/재배치에서 할당 함수 참조를 검색하고, 링크 설정에 힙 영역/의존성이 없는지 확인한다. 검사 범위와 생성 산출물 해시를 기록한다. | **미검증** — 현재 자동 검사 없음. “정적 배열 사용”만으로 전체 링크의 힙 비의존성이 입증되지는 않음. |
| `DET-001` | 자원 상한과 결정론적 실행 경로 | 스케줄러 및 IPCP 상한 | `MAX_TASKS`, `MAX_MUTEXES`, `g_ready_bitmap`, `mutexes[]`; `kernel/include/kernel_internal.h`, `kernel/include/prio_rtos.h`, `kernel/sched.c`, `kernel/ipcp.c` | `TC-DET-001` | 주요 루프가 태스크/뮤텍스 고정 상한 내에서 종료하는지 검토하고, 반복 횟수 및 타깃 최악 실행시간을 경계 입력에서 측정한다. | **부분 / 미검증** — 고정 크기 자료구조는 사용하지만 스케줄러 선택은 슬롯 순회이며 실행시간 상한 계측은 없음. |

## 검증 단계별 실행 계획

| 단계 | 사례 | 실행 환경 및 산출물 | 완료 조건 |
|---|---|---|---|
| 정적/호스트 검증 | `TC-TASK-*`, `TC-SCHED-*`, `TC-IPC-*` | 테스트 하네스에서 API 결과 코드, TCB, waiter/owner 상태, ready bitmap 및 effective priority 검사 | 경계값과 정상/오류 경로에 대한 자동 테스트가 재현 가능하게 통과 |
| 링크 산출물 검증 | `TC-SYS-001`, `TC-MEM-*` | RV32 ELF와 `build/kernel.map`; 심볼/섹션/미정의 참조 검사 | 목표 아키텍처 및 BSS 배치가 산출물로 확인되고 힙 참조가 없음 |
| QEMU 기능 검증 | `TC-TIME-001`, `TC-TASK-003`, `TC-SCHED-003`, `TC-IPC-*` | QEMU `virt` 실행 로그와 테스트별 결과 | 트랩·문맥 전환·동기화 동작 확인. QEMU 결과를 실기기 WCET 증거로 사용하지 않음 |
| 대상 시간 검증 | `TC-SYS-001`, `TC-TIME-002`, `TC-IPC-005`, `TC-DET-001` | 실제 보드, 클럭/timebase 설정, 사이클 계측 및 workload | tick, 인터럽트 지연, 임계 구역 WCET 및 blocking bound를 명시된 하드웨어 조건에서 측정 |

## 현재 확인된 주요 간극

- 1차 기준 플랫폼은 QEMU `virt`/RV32IMC/Hart 0이며, FE310 포팅은 별도 범위다.
- 1차 기준 tick은 10 MHz에서 10,000 count인 1 ms이며, 현재 코드는 interval 값만 정렬되고 timer 처리 경로는 추가 구현이 필요하다.
- ready bitmap의 요구 타입/비트 의미와 구현(`uint32_t`, task slot 기준)이 다르며 최고 우선순위 선택도 상수 시간 CLZ가 아니라 슬롯 순회다.
- 최종 기준은 4단계 mutex 중첩이지만 정적 Mutex 전환과 중첩 검사, IPCP 동작 및 blocking bound 자동 검증은 아직 남아 있다.
- 현재 `verification/` 검사 파일과 자동 테스트가 없으므로 위 `TC-*`는 모두 추후 구현할 검증 사례다.
