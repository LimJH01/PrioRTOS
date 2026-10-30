# 함수별 Final Master Baseline v2.2.4 적합성 대조표

> 이 문서는 [구현 계약 및 적합성 추적 기준선 v2.2.4](./implementation-contract-v2.2.4.md)의 현재 소스 판정표다. Master 문서에서 제거된 기존 Part 5의 함수별 구현 정보는 이 문서에서만 관리한다. 표에 남아 있는 `§5.x` 표기는 과거 함수 계약에 대한 추적용 역사 참조이며, 현재 기준은 Master의 `REQ-*`/`INV-*`와 구현 계약 문서다.

이 문서는 현재 구현의 모든 함수 및 어셈블리 진입점을
[Final Master Baseline v2.2.4](./master-spec-v2.2.4.md)와 대조한다.
명세 위치는 해당 문서의 절 번호 및 REQ/INV ID로 표시한다.

## 판정 기준

| 판정 | 의미 |
|---|---|
| ✅ 충족 | 코드에서 명세에 정의된 동작을 확인함 |
| ◐ 부분 | 일부 동작은 있으나 명세의 일부 조건·규칙과 다르거나 검증되지 않음 |
| ❌ 미충족 | 구현 동작이 명세와 다르거나 필수 동작이 구현되지 않음 |
| ❌ 미충족 | 명세가 요구하는 함수 또는 동작이 없거나 현재 구현과 다름 |

판정은 소스 정적 검토 결과다. QEMU 또는 실제 하드웨어에서의 동작, 사이클 수,
WCET, 스택 사용량 및 테스트 통과 여부를 뜻하지 않는다.

## 부팅 및 애플리케이션

| 함수 | 현재 판정 | 명세 위치 | 명세와 맞지 않는 점 / 판단 근거 |
|---|---|---|---|
| [`_start`](../boot/startup.S) | ✅ 충족 | §3.3 Step 1, §5.1 | 부트 스택 설정, `.bss` 초기화 후 `main()`을 호출한다. |
| [`main`](../app/main.c) | ◐ 부분 | §3.3 Step 2–3, §5.11 | 초기화 순서는 명세와 맞지만 `TIMER_INTERVAL`이 현재 10,000,000으로 설정되어 명세의 10MHz·1ms 기준 10,000 count와 다르다. |
| `uart_puts_checked` ([`main.c`](../app/main.c)) | ◐ 부분 | §5.10, §3.3 | UART 오류를 `kernel_panic`으로 전파하는 보조 함수다. 명세에 별도 함수로 정의되어 있지 않으며 부트 출력은 명세의 핵심 초기화 단계에 추가된 동작이다. |
| [`tasks_init`](../app/tasks.c) | ◐ 부분 | §5.12, §6.4 | 정적 스택·세마포어·응용 태스크·mutex 사용자를 등록한다. 다만 명세의 정적 `mutex_t` 초기화가 아니라 폐기 예정인 동적 ID 기반 `mutex_configure()`를 사용한다. |
| [`task_a`](../app/tasks.c) | ◐ 부분 | §3.4 REQ-SYN-004–005, §5.12 | semaphore/mutex 순서는 적용되어 semaphore 대기는 mutex 보유 중 일어나지 않는다. 그러나 mutex 보유 중 UART 출력과 지연 루프를 수행하며 실행시간 상한이나 계측은 없다. 실험용 임시 태스크다. |
| [`task_b`](../app/tasks.c) | ◐ 부분 | §3.4 REQ-SYN-004–005, §5.12 | `task_a`와 동일하게 mutex 보유 중 UART 출력과 지연 루프를 수행하며 실행시간 상한은 입증되지 않았다. 실험용 임시 태스크다. |
| [`mock_delay`](../app/tasks.c) | ◐ 부분 | §2 INV-TIM-004, §5.12 | 유한 횟수의 카운트다운 루프지만 실제 실행시간 상한은 제시되거나 검증되지 않았다. 터미널 확인용 임시 함수이며 삭제 예정으로 기록되어 있다. |

## BSP 및 하드웨어 드라이버

| 함수 | 현재 판정 | 명세 위치 | 명세와 맞지 않는 점 / 판단 근거 |
|---|---|---|---|
| [`clint_software_interrupt_init`](../bsp/clint.c) | ✅ 충족 | §3.3 Step 3, §5.9 | MSIP를 초기화하고 `mie.MSIE`를 설정한다. `mstatus.MIE`는 전역 인터럽트 허용 비트이므로 이 함수의 개별 인터럽트 활성화와 구분된다. |
| [`clint_send_software_interrupt`](../bsp/clint.c) | ✅ 충족 | §3.1 Cause 3, §5.9, §2 INV-CRI-011 | 배리어 후 MSIP를 설정하여 Cause 3 소프트웨어 인터럽트를 요청한다. |
| [`clint_clear_software_interrupt`](../bsp/clint.c) | ✅ 충족 | §3.3 트랩 처리, §5.9 | MSIP를 0으로 기록해 pending 상태를 해제한다. |
| [`clint_get_mtime`](../bsp/clint.c) | ◐ 부분 | §3.6 REQ-TMR-001, §2 INV-ATM-010, INV-ERR-008, §5.9 | High-Low-High 읽기를 최대 `CLINT_MTIME_READ_ATTEMPTS`회 시도하고 실패하면 `kernel_panic()`으로 정지한다. 반복 상한은 있으나 MMIO 접근의 WCET는 타깃 버스 응답시간에 따라 별도 입증해야 한다. 인터럽트 지연은 함수 자체의 알고리즘 실행시간과 분리하여 계측해야 한다. |
| [`clint_set_mtimecmp`](../bsp/clint.c) | ✅ 충족 | §2 INV-ATM-010, §5.9, §6.8 | RV32에서 LOW를 최댓값으로 임시 설정한 후 HIGH, LOW를 기록한다. |
| [`clint_timer_init`](../bsp/clint.c) | ❌ 미충족 | §1.1, §3.3 Step 3, §5.9, §6.8 | 명세는 10MHz에서 1ms에 해당하는 10,000 count를 요구하지만 현재 `TIMER_INTERVAL`은 10,000,000이다. 현재 값은 실험용이며 수정 예정이다. |
| [`clint_set_next_timer`](../bsp/clint.c) | ❌ 미충족 | §1.1, §3.1 타이머 ISR, §5.9, §6.8 | 다음 비교값을 예약하는 로직은 있으나 잘못된 `TIMER_INTERVAL`을 사용하므로 명세의 1ms 주기와 다르다. |
| [`uart_init`](../bsp/uart.c) | ✅ 충족 | §1.1 UART0, §5.10, §6.8 | 16550 UART의 divisor, 8-N-1, FIFO 설정을 수행한다. |
| [`uart_putc`](../bsp/uart.c) | ✅ 충족 | §2 INV-ERR-008, §5.10 | TX 준비 비트를 `UART_TX_POLL_LIMIT`회까지만 확인하고 상한 초과 시 오류를 반환한다. |
| [`uart_puts`](../bsp/uart.c) | ✅ 충족 | §2 INV-ERR-008, §5.10 | `uart_putc()`를 사용해 송신하며 CRLF 변환을 포함한 출력량을 `UART_PUTS_MAX_BYTES`(128)바이트로 제한한다. NULL, 문자열 초과, 문자 전송 타임아웃을 구분한다. |

## 트랩, 패닉 및 스케줄러

| 함수 | 현재 판정 | 명세 위치 | 명세와 맞지 않는 점 / 판단 근거 |
|---|---|---|---|
| [`trap_init`](../kernel/trap.c) | ✅ 충족 | §1.1 인터럽트 벡터, §3.3 Step 2, §5.3 | `trap_entry` 주소를 `mtvec`에 기록해 Direct Mode를 설정한다. |
| [`trap_handler`](../kernel/trap.c) | ◐ 부분 | §3.1 Top-Half/Bottom-Half, §3.3 트랩 처리, §5.3 | Cause 3에서는 MSIP를 지우고 스케줄러를 호출한다. Cause 7에서는 다음 타이머만 예약하며 `timer_tick_step()`을 호출하지 않는다. UART 출력과 직접 스케줄링은 현재 주석 처리되어 있어 Top-Half 분리는 부분적으로만 맞는다. |
| [`kernel_panic`](../kernel/panic.c) | ◐ 부분 | §3.3 패닉, §5.7, §2 INV-ERR-008 | MIE 차단·UART 메시지·`wfi` 정지는 구현한다. UART 출력 함수의 bounded polling은 적용되지만 반환 오류를 검사하지 않는 best-effort 출력이다. |
| [`sched_highest_priority_slot`](../kernel/sched.c) | ◐ 부분 | §3.1 REQ-SCH-001, §5.4 | 최하위 set bit를 De Bruijn LUT로 O(1)에 추출하도록 변경했다. 다만 명세의 함수명 `sched_peek_highest_prio()`와 다르고, 현재 IPCP는 `TCB.priority`를 동적으로 변경하므로 bitmap bit index가 effective priority와 항상 일치하는지는 추가 정합성 검증이 필요하다. |
| [`task_exit_trap`](../kernel/sched.c) | ◐ 부분 | §2 INV-LFC-006, §3.4 REQ-TSK-001, §5.2·§5.4 | 현재 함수는 `kernel_panic("task returned")`를 호출하므로 반환 태스크를 정지시키는 동작은 충족한다. 다만 명세가 요구하는 별도 `task_return_guard` 심볼이 아니라 C static 함수로 구현되어 있다. |
| [`stack_overflow_panic`](../kernel/sched.c) | ✅ 충족 | §2 INV-STK-007, §5.4 | 스택 오류를 `kernel_panic("task stack overflow")`로 전달한다. |
| [`task_create`](../kernel/sched.c) | ◐ 부분 | §3.4 REQ-TSK-001, §5.4, §6.2 | 슬롯과 priority를 `slot = prio`로 직접 매핑하고 카나리·128B 초기 문맥을 구성한다. 중복 등록 시 `kernel_panic("duplicate priority")`는 구현하지만, 명세의 TCB layout(`held_mutex`, `task_id`, 1바이트 priority/state)과 현재 TCB layout이 다르다. |
| [`idle_task`](../kernel/sched.c) | ✅ 충족 | §3.4 REQ-TSK-004, §5.4 | 우선순위 31 슬롯에서 무한 `wfi` 대기로 동작한다. |
| [`priortos_start`](../kernel/sched.c) | ◐ 부분 | §3.4 REQ-TSK-005, §5.4 | Idle 등록, LSB De Bruijn 선택, 최초 태스크의 ready bit 제거, RUNNING 전환 및 최초 문맥 복원은 구현한다. 다만 명세의 Guard Bit 합성 방식과 `ipcp_scheduler_started()` 호출은 없다. |
| [`sched_schedule`](../kernel/sched.c) | ◐ 부분 | §3.1 REQ-SCH-001, §5.4, §6.4 | De Bruijn helper로 next를 선택하고 next ready bit를 제거한다. 그러나 현재 Running 태스크를 READY로 바꿀 때 ready bitmap에 다시 세우지 않으며, 명세의 `sched_select_next_task()` API와 Guard Bit 방식도 없다. |
| [`sched_irq_save`](../kernel/sched.c) | ◐ 부분 | §3.5 REQ-CRI-001–002, §5.8, §2 INV-CRI-011 | MIE를 저장하고 끄지만 명세의 중첩 카운터 기반 `enter_critical()` API와 배리어 규약을 구현하지 않는다. |
| [`sched_irq_restore`](../kernel/sched.c) | ◐ 부분 | §3.5 REQ-CRI-002, §5.8, §2 INV-CRI-011 | 저장된 MIE 복원은 가능하나 nesting count, underflow 검사 및 마지막 탈출 시 `fence rw, rw` 규약이 없다. |
| [`sched_request_reschedule`](../kernel/sched.c) | ✅ 충족 | §3.1 Cause 3, §5.4 | CLINT MSIP 기반으로 지연된 문맥 교환을 요청한다. |
| [`sched_block_current`](../kernel/sched.c) | ◐ 부분 | §3.4 REQ-SYN-004, §5.4 | 현재 태스크를 BLOCKED로 만들고 슬롯 ready bit를 제거한 뒤 MSIP를 요청한다. 명세의 임계 구역 API와 sleep bitmap 연계는 구현하지 않는다. |
| [`sched_wake_task`](../kernel/sched.c) | ◐ 부분 | §3.1, §3.4 REQ-SYN-005, §5.4 | 슬롯을 직접 기준으로 BLOCKED 태스크를 READY로 복귀시키고 ready bit를 세운다. 다만 명세의 LSB O(1) 선택 및 sleep expiration 경로와 통합되지 않는다. |
| [`task_yield`](../kernel/sched.c) | ◐ 부분 | §3.4 REQ-TSK-002, §5.4 | MSIP 요청은 수행하지만 명세의 `enter_critical()` → MSIP → `exit_critical()` 순서가 없다. |

## IPCP 및 세마포어

| 함수 | 현재 판정 | 명세 위치 | 명세와 맞지 않는 점 / 판단 근거 |
|---|---|---|---|
| [`current_task_slot`](../kernel/ipcp.c) | ◐ 부분 | §5.5 | 현재 TCB 포인터와 RUNNING 상태로 슬롯을 확인하지만, 명세의 직접 매핑 TCB 필드와 고정 구조가 아니라 최대 32회 선형 검색이다. |
| [`configured_user_mask`](../kernel/ipcp.c) | ❌ 미충족 | §5.5, §6.3 | Idle 슬롯 제외 마스크와 32비트 경계 처리는 있으나, 명세는 런타임 사용자 슬롯 bitmap과 `ipcp.c` 동적 설정 자체를 폐기하고 정적 `mutex_t`를 요구한다. |
| [`system_ceiling`](../kernel/ipcp.c) | ❌ 미충족 | §3.5 REQ-SYN-002, §5.5 | 현재는 다른 mutex의 ceiling 최솟값을 반복 검색한다. 명세의 고정 ceiling과 TCB 스택 shadowing/LIFO 모델을 구현하지 않는다. |
| [`refresh_effective_priority`](../kernel/ipcp.c) | ❌ 미충족 | §2 INV-MUT-009, §3.5 REQ-SYN-002–003, §5.5 | 보유 mutex 전체를 재검색해 priority를 계산한다. 명세의 mutex별 `saved_prio`와 `prev_mutex` 기반 LIFO shadowing, 비트맵 미조작 승격/복원을 구현하지 않는다. |
| [`can_acquire`](../kernel/ipcp.c) | ❌ 미충족 | §3.5 REQ-SYN-002, §5.5 | 사용자·소유 여부와 system ceiling을 검사하고 실패 시 BLOCKED 대기를 허용한다. 명세는 고정 ceiling 유효성 검사와 경합 시 패닉 정책을 요구한다. |
| [`dispatch_eligible_waiters`](../kernel/ipcp.c) | ❌ 미충족 | §3.5, §5.5 | waiter bitmap을 반복 검색하고 소유권 handoff를 수행한다. 명세의 무대기큐·스택 shadowing IPCP에는 이 런타임 waiter handoff 경로가 없다. |
| [`mutex_configure`](../kernel/ipcp.c) | ❌ 미충족 | §3.5 REQ-SYN-001, §5.5 | 현재 동적 ID와 사용자 슬롯으로 ceiling을 계산한다. 명세는 `mutex_init(mutex_t *, ceiling_prio)`로 정적 ceiling과 LIFO 필드를 초기화한다. |
| [`mutex_lock`](../kernel/ipcp.c) | ❌ 미충족 | §2 INV-MUT-009, §3.5 REQ-SYN-002, §5.5 | 획득 불가 시 현재 태스크를 Blocked로 만들고 대기한다. 명세의 `mutex_lock(mutex_t *)`, critical section, saved priority 백업, LIFO push 및 경합 패닉과 다르다. |
| [`mutex_unlock`](../kernel/ipcp.c) | ❌ 미충족 | §2 INV-MUT-009, §3.5 REQ-SYN-003, §5.5 | 소유권 해제와 waiter handoff를 수행하지만, 명세의 LIFO 해제 순서 검증·stack pop·saved priority 복원·out-of-order panic을 구현하지 않는다. |
| [`ipcp_validate_configuration`](../kernel/ipcp.c) | ❌ 미충족 | §3.4 REQ-TSK-005, §3.5 REQ-SYN-001, §5.5 | 동적 IPCP 설정을 시작 전에 검증한다. v2.2에서는 동적 설정 모듈과 사용자 bitmap이 제거되어 해당 명세 경로가 존재하지 않는다. |
| [`ipcp_scheduler_started`](../kernel/ipcp.c) | ❌ 미충족 | §3.4 REQ-TSK-005, §5.5 | 동적 mutex 설정을 동결한다. 명세의 정적 mutex 초기화에는 설정 동결 단계가 없다. |
| [`semaphore_init`](../kernel/semaphore.c) | ✅ 충족 | §3.4 REQ-SYN-004–005, §5.6 | count와 waiter bitmap을 초기화한다. |
| [`semaphore_wait`](../kernel/semaphore.c) | ❌ 미충족 | §2 INV-SYN-012, §3.4 REQ-SYN-004, §5.6 | 현재 태스크가 mutex를 보유해 priority가 승격된 상태인지 검사하지 않는다. 또한 명세의 `held_mutex` 기반 즉시 패닉 규칙을 구현하지 않는다. |
| [`semaphore_signal`](../kernel/semaphore.c) | ◐ 부분 | §3.4 REQ-SYN-005, §5.6 | 대기자가 없으면 count를 증가시키고, 있으면 대기 태스크를 깨운다. 다만 대기자 선택은 선형 슬롯/priority 검색이며 명세의 LSB De Bruijn O(1) 선택과 다르다. |

## 어셈블리 문맥 처리

| 함수/진입점 | 현재 판정 | 명세 위치 | 명세와 맞지 않는 점 / 판단 근거 |
|---|---|---|---|
| [`prio_context_first_switch`](../kernel/context.S) | ✅ 충족 | §3.3 Step 5, §5.2, §6.1 | 초기 문맥과 CSR을 복원하고 `mret`으로 첫 태스크에 진입한다. |
| [`trap_entry`](../kernel/context.S) | ◐ 부분 | §3.1 Cause 3/7, §5.2, §6.1 | 128-byte 문맥 저장·복원과 카나리/headroom 검사는 구현한다. 그러나 타이머 ISR의 `timer_tick_step()` 및 명세의 완전한 Top-Half/Bottom-Half 흐름은 연결되지 않았다. |
| `prio_context_switch` | ❌ 미충족 | §5.2 | 명세에 별도 어셈블리 심볼이 기재되어 있으나 현재 `kernel/context.S`에는 없다. 현재 공용 `trap_entry`가 트랩 문맥을 처리한다. |
| `task_return_guard` | ❌ 미충족 | §2 INV-LFC-006, §5.2 | 명세의 별도 반환 guard 심볼이 없다. 현재 `task_create()`는 static `task_exit_trap()`을 return address로 사용하며 이 함수는 panic을 호출한다. 동작은 부분적으로 맞지만 명세 심볼/API와 다르다. |

## 명세상 필수지만 현재 구현에서 확인되지 않은 함수

| 함수 | 판정 | 명세 위치 | 미구현 내용 |
|---|---|---|---|
| `sched_peek_highest_prio(uint32_t task_bitmap)` | ❌ 미충족 | §3.1 REQ-SCH-001, §5.4 | `bitmap & -bitmap` 및 De Bruijn LUT로 LSB 최고 priority를 O(1)에 추출하는 함수가 없다. |
| `sched_select_next_task(void)` | ❌ 미충족 | §3.1 REQ-SCH-001, §5.4 | 최고 priority 슬롯을 직접 TCB 포인터로 변환하고 ready bitmap에서 제거하는 함수가 없다. |
| `task_sleep_ticks(uint32_t ticks)` | ❌ 미충족 | §3.4 REQ-TSK-003, §5.4, §5.13 | mutex 점유 검사, sleep remaining ticks 기록, sleep bitmap 등록 및 MSIP 디스패치가 없다. |
| `timer_tick_step(void)` | ❌ 미충족 | §3.1 타이머 ISR, §3.6 REQ-TMR-002, §5.4, §5.13 | 시스템 tick 증가, sleep 카운터 감산, 만료 태스크 복귀 및 선점 판정이 없다. |
| `get_tick_count(void)` | ❌ 미충족 | §3.6 REQ-TMR-001, §5.4, §5.13 | 원자적 64비트 시스템 tick 조회 API가 없다. |
| `enter_critical(void)` | ❌ 미충족 | §3.5 REQ-CRI-001, §5.8, §5.13 | MIE 비활성화, 배리어, nesting count 증가 및 overflow 검사가 없다. |
| `exit_critical(void)` | ❌ 미충족 | §3.5 REQ-CRI-002, §5.8, §5.13 | underflow 검사, 마지막 탈출 시 배리어 및 MIE 복원이 없다. |
| `task_return_guard(void)` | ❌ 미충족 | §2 INV-LFC-006, §5.2 | 태스크 반환 시 `kernel_panic("task returned")`를 호출하는 별도 guard가 없다. |
| `unhandled_trap_freeze(void)` | ❌ 미충족 | §5.3 | 미인가 trap을 별도 심볼로 동결하는 함수가 없다. 현재 `trap_handler()`의 `else` 블록에 `wfi` 루프가 직접 들어 있다. |
| `mutex_init(mutex_t *m, uint8_t ceiling_prio)` | ❌ 미충족 | §3.5 REQ-SYN-001, §5.5 | 정적 mutex의 ceiling, `saved_prio`, `prev_mutex`를 초기화하는 API가 없다. |

## 구현 전반에서 명세와 다른 점

- v2.2 명세는 우선순위 0이 최고, 31이 최저이며 `Priority == Task Slot == Bit Index` 직접 매핑을 요구한다. 현재 구현은 이 우선순위 방향과 `slot = prio` 매핑을 따르고, 최하위 set bit를 De Bruijn LUT로 선택한다.
- v2.2.1 명세의 `g_ready_bitmap`은 Running 태스크를 제외한 Ready 태스크 비트맵이어야 한다. 현재 `priortos_start()`와 `sched_schedule()`은 선택된 next bit를 제거하지만, 선점되는 현재 태스크를 READY로 바꿀 때 그 태스크의 bit를 다시 세우지 않는다.
- v2.2 명세는 `g_prio_to_slot[32]`를 폐기했으므로, 기존 v2.1 대조표의 “priority-to-slot 매핑 미갱신”은 더 이상 핵심 불일치가 아니다. 현재 남은 문제는 IPCP 동적 priority와 직접 매핑 bitmap의 정합성, 그리고 Running 비트 처리다.
- v2.2 명세는 `kernel/ipcp.c`의 동적 mutex 설정·waiter handoff 대신 `kernel/mutex.c`의 정적 `mutex_t`와 LIFO priority shadowing을 요구한다. 현재 저장소에는 `kernel/mutex.c`가 없고 `ipcp.c`의 다른 IPCP 변형이 구현되어 있다.
- v2.2 명세는 1ms 타이머, `timer_tick_step()`, sleep bitmap, 64비트 system tick, 중첩 critical-section API를 요구하지만 현재 구현에는 해당 경로가 연결되어 있지 않다.
- 명세의 `TCB_t`, `mutex_t`, 함수 원형 및 어셈블리 심볼 목록과 현재 헤더·소스의 ABI가 다르므로, 명세 적합성을 높이려면 자료구조와 API를 먼저 동기화해야 한다.
