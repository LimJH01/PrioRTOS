# 함수별 Final Master Baseline v2.1 적합성 대조표

이 문서는 현재 구현의 함수 및 어셈블리 진입점 48개를
[Final Master Baseline v2.1](./master-spec-v2.1.md)과 대조한다.
명세 위치는 해당 문서의 절 번호 및 REQ/INV ID로 표시한다.

## 판정 기준

| 판정 | 의미 |
|---|---|
| ✅ 충족 | 코드에서 명세에 정의된 동작을 확인함 |
| ◐ 부분 | 일부 동작은 있으나 명세의 일부 조건·규칙과 다르거나 검증되지 않음 |
| ❌ 미충족 | 구현 동작이 명세와 다르거나 필수 동작이 구현되지 않음 |
| — 미구현 | 명세가 요구하지만 현재 별도 함수로 구현되지 않음 |

판정은 소스 정적 검토 결과다. QEMU 또는 실제 하드웨어에서의 동작, 사이클 수,
WCET, 스택 사용량 및 테스트 통과 여부를 뜻하지 않는다.

## 부팅 및 애플리케이션

| 함수 | 현재 판정 | 명세 위치 | 명세와 맞지 않는 점 / 판단 근거 |
|---|---|---|---|
| [`_start`](../boot/startup.S) | ✅ 충족 | §3.2 Step 1, §5.1 | 시스템 스택 설정, `.bss` 초기화 후 `main()`을 호출한다. |
| [`main`](../app/main.c) | ◐ 부분 | §3.2 Step 2–3, §5.12 | 초기화 순서는 명세와 맞지만 타이머 주기는 `TIMER_INTERVAL` 값에 종속되며 첨부 기준의 1ms와 다르다. |
| [`tasks_init`](../app/tasks.c) | ✅ 충족 | §5.13, §6.7 | 정적 스택·세마포어·응용 태스크·mutex 사용자를 등록한다. 커널 용량은 idle 포함 최대 32개(우선순위 0–31)로 확장했으며, 현재 실제 등록은 idle과 응용 태스크 2개다. |
| [`task_a`](../app/tasks.c) | ◐ 부분 | §3.4 REQ-SYN-004–005, §5.13 | semaphore/mutex 순서는 적용되어 semaphore 대기는 mutex 보유 중 일어나지 않는다. mutex 보유 중 UART 출력과 지연 루프를 실행하지만 실행시간 상한이나 계측은 없다. |
| [`task_b`](../app/tasks.c) | ◐ 부분 | §3.4 REQ-SYN-004–005, §5.13 | `task_a`와 동일하게 mutex 보유 중 UART 출력과 지연 루프를 수행하며 실행시간 상한은 입증되지 않았다. |
| [`mock_delay`](../app/tasks.c) | ◐ 부분 | §2 INV-TIM-004, §5.13 | 유한 횟수의 루프지만 실제 실행시간 상한은 제시되거나 검증되지 않았다. |

## BSP 및 하드웨어 드라이버

| 함수 | 현재 판정 | 명세 위치 | 명세와 맞지 않는 점 / 판단 근거 |
|---|---|---|---|
| [`clint_software_interrupt_init`](../bsp/clint.c) | ✅ 충족 | §3.2 Step 3, §5.10 | MSIP 초기화와 `mie.MSIE` 설정을 수행한다. |
| [`clint_send_software_interrupt`](../bsp/clint.c) | ✅ 충족 | §3.1 Cause 3, §5.10, §2 INV-CRI-011 | 메모리/IO 배리어 후 MSIP를 설정한다. |
| [`clint_clear_software_interrupt`](../bsp/clint.c) | ✅ 충족 | §3.2 트랩 처리, §5.10 | MSIP를 0으로 기록해 pending 상태를 해제한다. |
| [`clint_get_mtime`](../bsp/clint.c) | ◐ 부분 | §3.6 REQ-TMR-001, §2 INV-ATM-010, INV-ERR-008, §5.10 | High-Low-High를 최대 `CLINT_MTIME_READ_ATTEMPTS`회(현재 3회) 시도한다. 상한 내 안정된 값을 얻지 못하면 함수에서 `kernel_panic()`으로 안전 정지한다. 반복 횟수는 제한했지만 MMIO 접근의 WCET는 타깃 버스 응답시간 상한에 따라 별도 계측/입증해야 한다. |
| [`clint_set_mtimecmp`](../bsp/clint.c) | ✅ 충족 | §2 INV-ATM-010, §5.10, §6.8 | RV32에서 임시로 LOW를 최댓값으로 설정한 후 HIGH, LOW를 기록하는 갱신 순서를 사용한다. |
| [`clint_timer_init`](../bsp/clint.c) | ❌ 미충족 | §1.1, §3.2 Step 3, §5.10, §6.8 | 명세는 10 MHz에서 1ms에 해당하는 10,000 count를 요구하지만 현재 `TIMER_INTERVAL`은 10,000,000이다. |
| [`clint_set_next_timer`](../bsp/clint.c) | ❌ 미충족 | §1.1, §3.1 타이머 ISR, §5.10, §6.8 | 재예약 로직은 있으나 같은 잘못된 `TIMER_INTERVAL`을 사용하므로 주기가 명세와 다르다. |
| [`uart_init`](../bsp/uart.c) | ✅ 충족 | §1.1 UART0, §5.11, §6.8 | 16550 UART의 divisor, 8-N-1, FIFO 설정을 수행한다. |
| [`uart_putc`](../bsp/uart.c) | ✅ 충족 | §2 INV-ERR-008, §5.11 | TX 준비 비트를 `UART_TX_POLL_LIMIT` 횟수만 확인하고, 상한 초과 시 `UART_ERR_TX_TIMEOUT`을 반환한다. |
| [`uart_puts`](../bsp/uart.c) | ✅ 충족 | §2 INV-ERR-008, §5.11 | CRLF 변환 후 송신량을 최대 `UART_PUTS_MAX_BYTES`(128)바이트로 제한한다. 상한 초과, NULL 입력 및 문자 전송 타임아웃을 구분된 오류로 반환한다. |

## 트랩, 패닉 및 스케줄러

| 함수 | 현재 판정 | 명세 위치 | 명세와 맞지 않는 점 / 판단 근거 |
|---|---|---|---|
| [`trap_init`](../kernel/trap.c) | ✅ 충족 | §1.1 인터럽트 벡터, §3.2 Step 2, §5.3 | `trap_entry`를 `mtvec`에 기록해 Direct Mode를 설정한다. |
| [`trap_handler`](../kernel/trap.c) | ❌ 미충족 | §3.1 Top-Half/Bottom-Half, §3.2 트랩 처리, §5.3 | Cause 7에서 UART 문자를 출력하고 `sched_schedule()`을 직접 호출한다. 명세는 Top-Half의 UART 출력을 금지하고 틱 처리 후 필요한 경우 MSIP로 Bottom-Half에 디스패치를 위임한다. |
| [`kernel_panic`](../kernel/panic.c) | ◐ 부분 | §3.2 명시적 패닉, §5.8, §2 INV-ERR-008 | MIE 차단·UART 메시지·`wfi` 정지는 구현했다. UART 폴링은 제한되지만, 패닉 메시지 전송 오류를 호출자에게 전달하지 않는 best-effort 출력이다. |
| [`sched_highest_priority_slot`](../kernel/sched.c) | ❌ 미충족 | §3.1 REQ-SCH-001, §5.4, §6.7 | `MAX_TASKS`를 선형 순회해 최고 우선순위를 찾는다. De Bruijn MSB, 우선순위 비트맵, `g_prio_to_slot` 기반 branchless O(1) 선택이 아니다. |
| [`task_exit_trap`](../kernel/sched.c) | ❌ 미충족 | §2 INV-LFC-006, §3.3 REQ-TSK-001, §5.2·§5.4 | 태스크가 반환하면 `kernel_panic("task returned")`를 호출하는 대신 조용히 `wfi` 무한 루프에 진입한다. |
| [`stack_overflow_panic`](../kernel/sched.c) | ✅ 충족 | §2 INV-STK-007, §5.4 | 스택 오류를 `kernel_panic("task stack overflow")`로 전달한다. |
| [`task_create`](../kernel/sched.c) | ◐ 부분 | §3.3 REQ-TSK-001, §5.4, §6.7 | 유효성 검사, 카나리, 초기 문맥 구성은 구현했다. 그러나 슬롯과 우선순위를 동일시하고 `g_prio_to_slot`을 사용하지 않아 0–31 우선순위/독립 슬롯 규칙과 다르다. 중복 등록 시 명세의 패닉 대신 오류 코드를 반환한다. |
| [`idle_task`](../kernel/sched.c) | ✅ 충족 | §3.3 REQ-TSK-004, §5.4 | 무한 `wfi` 대기로 동작한다. |
| [`priortos_start`](../kernel/sched.c) | ◐ 부분 | §3.3 REQ-TSK-005, §5.4 | Idle 등록, IPCP 구성 검사, 첫 태스크 문맥 복원은 구현했다. 최고 우선순위 선택은 O(1) De Bruijn이 아니라 선형 탐색에 의존한다. |
| [`sched_schedule`](../kernel/sched.c) | ❌ 미충족 | §3.1 REQ-SCH-001, §5.4 | 현재는 슬롯 비트맵에서 선형 탐색한다. 명세의 우선순위 비트맵과 De Bruijn/간접 매핑 구조가 아니다. |
| [`sched_irq_save`](../kernel/sched.c) | ◐ 부분 | §3.5 REQ-CRI-001–002, §5.9, §2 INV-CRI-011 | MIE를 저장하고 끄지만 중첩 카운터 기반 `enter_critical()` API는 구현하지 않았다. |
| [`sched_irq_restore`](../kernel/sched.c) | ◐ 부분 | §3.5 REQ-CRI-002, §5.9, §2 INV-CRI-011 | MIE 복원은 가능하나 nesting count, underflow 검사 및 마지막 탈출 시 `fence rw, rw` 규약이 없다. |
| [`sched_request_reschedule`](../kernel/sched.c) | ✅ 충족 | §3.1 Cause 3, §5.4 | CLINT MSIP 기반으로 재스케줄을 요청한다. |
| [`sched_block_current`](../kernel/sched.c) | ◐ 부분 | §3.4 REQ-SYN-004, §5.4 | 현재 태스크를 Blocked로 전환하고 MSIP를 요청한다. 다만 ready bitmap이 우선순위가 아닌 슬롯을 표시한다. |
| [`sched_wake_task`](../kernel/sched.c) | ◐ 부분 | §3.4 REQ-SYN-005, §5.4 | Blocked 태스크를 Ready로 복귀시키지만 슬롯 기준 비트맵을 사용하고 priority-to-slot 매핑을 갱신하지 않는다. |
| [`task_yield`](../kernel/sched.c) | ◐ 부분 | §3.3 REQ-TSK-002, §5.4 | MSIP 요청은 수행하지만 명세의 임계 구역 진입, 현재 우선순위 비트 확인 및 임계 구역 탈출 절차가 없다. |

## IPCP 및 세마포어

| 함수 | 현재 판정 | 명세 위치 | 명세와 맞지 않는 점 / 판단 근거 |
|---|---|---|---|
| [`current_task_slot`](../kernel/ipcp.c) | ✅ 충족 | §5.5 | 현재 TCB 포인터 및 RUNNING 상태로 슬롯을 식별한다. 최대 태스크 수에 제한된 선형 검색이다. |
| [`configured_user_mask`](../kernel/ipcp.c) | ✅ 충족 | §5.5, §6.7 | 유효 슬롯 마스크를 만들고 Idle 슬롯을 제외하며, 32비트 시프트 경계를 별도 처리한다. |
| [`system_ceiling`](../kernel/ipcp.c) | ◐ 부분 | §3.4 REQ-SYN-002, §5.5 | 다른 태스크가 소유한 mutex ceiling의 최솟값 계산은 수행한다. 다만 반복 검색이며 명세의 고정 실행시간/우선순위 매핑 경로와 통합되어 있지 않다. |
| [`refresh_effective_priority`](../kernel/ipcp.c) | ◐ 부분 | §2 INV-MUT-009, §3.4 REQ-SYN-002–003, §5.5 | 보유 mutex 기준 유효 우선순위를 재계산한다. ready bitmap과 `g_prio_to_slot`을 함께 원자적으로 갱신하지 않는다. |
| [`can_acquire`](../kernel/ipcp.c) | ◐ 부분 | §3.4 REQ-SYN-002, §5.5 | 사용자·소유 여부와 system ceiling은 검사한다. 현재의 실패 처리/대기 동작은 SRS에 정의된 경합 시 패닉 규칙과 다르며, 상세 정책 간 차이도 있다. |
| [`dispatch_eligible_waiters`](../kernel/ipcp.c) | ◐ 부분 | §2 INV-MUT-009, §3.4 REQ-SYN-002–003, §5.5 | 대기자 중 획득 가능한 태스크를 선택해 handoff한다. SRS의 무경합 IPCP 경합 처리 및 O(1) 관련 제약과 다르게 정적 상한 반복 탐색을 수행한다. |
| [`mutex_configure`](../kernel/ipcp.c) | ✅ 충족 | §3.4 REQ-SYN-001, §5.5 | 고정 ID/사용자 등록, 사용자 기본 우선순위로 ceiling 계산, 중복 설정 거부 및 시작 후 설정 동결을 수행한다. |
| [`mutex_lock`](../kernel/ipcp.c) | ❌ 미충족 | §2 INV-MUT-009, §3.4 REQ-SYN-002, §5.5 | 획득 불가 시 현재 태스크를 Blocked로 만들고 대기한다. SRS의 mutex contention 패닉 규칙과 다르며, 우선순위 승격에 따른 ready bitmap/간접 매핑 갱신도 없다. LLD §5.5에는 waiter/handoff 동작이 별도로 기재되어 있어 명세 내부에도 정책 차이가 있다. |
| [`mutex_unlock`](../kernel/ipcp.c) | ◐ 부분 | §2 INV-MUT-009, §3.4 REQ-SYN-003, §5.5 | 소유권 확인, 우선순위 재계산, 대기자 handoff 및 MSIP 요청은 수행한다. 비소유자 호출은 패닉이 아니라 오류 반환이며, ready bitmap/매핑 복원은 없다. |
| [`ipcp_validate_configuration`](../kernel/ipcp.c) | ◐ 부분 | §3.3 REQ-TSK-005, §3.4 REQ-SYN-001, §5.5 | 사용자 마스크와 사용 태스크 등록 여부를 확인한다. 설정된 ceiling이 사용자들의 base priority 최솟값과 일치하는지는 시작 시 재검증하지 않는다. |
| [`ipcp_scheduler_started`](../kernel/ipcp.c) | ✅ 충족 | §3.3 REQ-TSK-005, §5.5 | 스케줄러 시작 후 mutex 설정을 동결한다. |
| [`semaphore_init`](../kernel/semaphore.c) | ✅ 충족 | §3.4 REQ-SYN-004–005, §5.7 | count와 waiter bitmap을 초기화한다. |
| [`semaphore_wait`](../kernel/semaphore.c) | ❌ 미충족 | §2 INV-SYN-012, §3.4 REQ-SYN-004, §5.7 | 현재 태스크가 mutex를 보유해 우선순위가 승격된 상태인지 검사하지 않는다. mutex 보유 상태의 semaphore wait를 차단하거나 패닉하지 않아 INV-SYN-012를 보장하지 못한다. |
| [`semaphore_signal`](../kernel/semaphore.c) | ◐ 부분 | §3.4 REQ-SYN-005, §5.7 | 대기자가 없으면 count 증가, 있으면 최고 우선순위 waiter를 깨우는 동작은 있다. ready bitmap은 슬롯 기준이며 `g_prio_to_slot` 갱신 및 명세의 우선순위 비트 설정이 없다. |

## 어셈블리 문맥 처리

| 함수/진입점 | 현재 판정 | 명세 위치 | 명세와 맞지 않는 점 / 판단 근거 |
|---|---|---|---|
| [`prio_context_first_switch`](../kernel/context.S) | ✅ 충족 | §3.2 Step 5, §5.2, §6.2 | 초기 문맥과 CSR을 복원하고 `mret`으로 첫 태스크에 진입한다. |
| [`trap_entry`](../kernel/context.S) | ◐ 부분 | §3.1 Cause 3/7, §5.2, §6.2 | 128-byte 문맥 저장·복원과 카나리/headroom 검사는 구현했다. 그러나 타이머 ISR이 요구하는 Top-Half/Bottom-Half 분리는 `trap_handler`의 동작으로 인해 지켜지지 않는다. |
| `prio_context_switch` | — 미구현 | §5.2, §5.14 | 명세에 별도 함수가 기재되어 있으나 해당 심볼은 없다. 현재 공용 `trap_entry`가 문맥 전환을 담당한다. |

## 명세상 필수지만 현재 구현에서 확인되지 않은 함수

| 함수 | 판정 | 명세 위치 | 미구현 내용 |
|---|---|---|---|
| `task_sleep_ticks(uint32_t ticks)` | — 미구현 | §3.3 REQ-TSK-003, §5.4, §5.14 | mutex 보유 검사, 슬립 상태 등록, ready 상태 제거 및 MSIP 디스패치가 없다. |
| `timer_tick_step(void)` | — 미구현 | §3.1 타이머 ISR, §3.6 REQ-TMR-002, §5.4, §5.14 | 시스템 tick 증가, 슬립 카운터 감소, 만료 태스크 복귀 및 선점 요청이 없다. |
| `get_tick_count(void)` | — 미구현 | §3.6 REQ-TMR-001, §5.4, §5.14 | 원자적 64비트 시스템 tick 조회 API가 없다. |
| `enter_critical(void)` | — 미구현 | §3.5 REQ-CRI-001, §5.9, §5.14 | MIE 비활성화, 메모리 배리어, 중첩 카운터 증가 및 overflow 검사가 없다. |
| `exit_critical(void)` | — 미구현 | §3.5 REQ-CRI-002, §5.9, §5.14 | nesting underflow 검사, 마지막 탈출 시 컴파일러/하드웨어 배리어와 MIE 복원이 없다. |
| `task_return_guard(void)` | — 미구현 | §2 INV-LFC-006, §5.2 | 태스크 반환 시 `kernel_panic("task returned")`를 호출하는 별도 guard가 없다. |

## 구현 전반에서 명세와 다른 점

- §3.1 및 §6.7은 우선순위 0–31 bitmap과 `g_prio_to_slot[32]` 간접 매핑을 요구하지만,
  현재 구현은 `MAX_TASKS = 32`이며 ready bitmap 비트가 TCB 슬롯을 표시한다.
- §6.9는 As-Is와 To-Be의 차이를 명시한다. 이 문서의 적합성 판정은 To-Be 요구사항
  기준이며, §6.9의 As-Is 설명은 현재 구현을 설명하는 대조 정보로 취급했다.
- §3.4 REQ-SYN-002의 mutex 경합 시 패닉 요구와 §5.5의 waiter/block/handoff 설명은
  동작 정책이 서로 다르다. 위 표에는 SRS 규칙과 현재 구현의 차이를 기록했으며,
  명세 자체의 정책 결정이 필요하다.
- 명세서의 0–31 우선순위 목표와 §6.5/§6.9의 기준 빌드 `OS_MAX_TASKS = 8` 정보는
  서로 다른 기준 시점/범위를 기술한다. 현재 코드의 32개 슬롯 용량은 실제로 32개
  응용 태스크가 등록됐다는 뜻은 아니다.
