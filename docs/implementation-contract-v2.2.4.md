# PrioRTOS 구현 계약 및 적합성 추적 기준선 v2.2.4

이 문서는 [Final Master Baseline v2.2.4](./master-spec-v2.2.4.md)의 구현 동반 문서다.

Master 문서에는 아키텍처·요구사항·불변식·자료구조만 유지하며, 함수별 구현 상태와 소스 적합성 판정은 이 문서와 [함수별 적합성 대조표](./function-conformance-v2.1.md)에서 관리한다.

## 문서 경계

| 문서 | 책임 |
| --- | --- |
| [master-spec-v2.2.4.md](./master-spec-v2.2.4.md) | 동결된 제품 아키텍처 및 요구사항 기준선 |
| [implementation-contract-v2.2.4.md](./implementation-contract-v2.2.4.md) | 함수·심볼·자료구조의 구현 계약과 추적 규칙 |
| [function-conformance-v2.1.md](./function-conformance-v2.1.md) | 현재 소스의 충족·부분·미충족 판정 및 근거 |

## 구현 판정 원칙

- `✅ 충족`은 소스 정적 검토에서 요구 동작을 확인했다는 뜻이며, WCET·하드웨어 동작·인증 완료를 의미하지 않는다.
- `◐ 부분`은 일부 동작은 있으나 API, 자료구조, 경로 연결 또는 검증 조건이 기준선과 다른 상태다.
- `❌ 미충족`은 요구 심볼·동작이 없거나 현재 구현이 기준선과 명확히 다른 상태다.
- Master 문서의 `구현 완료` 표현은 실제 소스 판정의 근거로 사용하지 않는다. 구현 상태는 반드시 적합성 대조표를 기준으로 한다.

## 구현 계약 범위

### 부트 및 애플리케이션

`_start`, `main`, `tasks_init`, `task_a`, `task_b`, `mock_delay`의 초기화 순서, 정적 스택 사용, 태스크 등록 및 실험용 동작을 추적한다.

### BSP

CLINT의 MSIP·MTIME·MTIMECMP 원자 접근, 1 ms 타이머 설정, UART 16550 초기화 및 bounded polling을 추적한다.

### 트랩·패닉·스케줄러

`trap_init`, `trap_handler`, `kernel_panic`, `trap_entry`, `prio_context_first_switch`, `sched_highest_priority_slot`, `sched_select_next_task`, `task_sleep_ticks`, `timer_tick_step`, `get_tick_count`, `enter_critical`, `exit_critical`의 존재 여부와 기준선 정합성을 추적한다.

### IPCP 및 세마포어

정적 `mutex_t`와 `mutex_init`, LIFO priority shadowing, `locked` 이중 락 검증, `semaphore_wait`의 primitive isolation 및 순수 LSB waiter 선택을 추적한다. 기존 동적 `kernel/ipcp.c` 구현은 v2.2.4 목표와의 차이로 별도 기록한다.

### ABI 및 스택 경계

128바이트 문맥 프레임, TCB ABI 오프셋, 16바이트 카나리, `mscratch = stack_base + 144`, 커널 시스템 스택 전환을 추적한다.

## 기존 Part 5 참조 처리

Master v2.2.4에서 제거된 기존 `§5.1~§5.13`은 더 이상 Master의 장 번호로 사용하지 않는다. 기존 적합성 대조표에 남은 `§5.x` 표기는 과거 함수 계약 추적표를 가리키는 역사적 참조이며, 판정의 실질적 기준은 다음 우선순위를 따른다.

1. Master의 `REQ-*` 및 `INV-*` 요구사항
2. Master의 LLD 4·6절 구조·자료구조 명세
3. 이 문서의 구현 계약 범위
4. 적합성 대조표의 현재 소스 근거

따라서 함수가 Master에 직접 정의되어 있지 않더라도, 구현 계약과 적합성 대조표에서 현재 상태를 추적할 수 있다.

## 현재 저장소의 주요 정합성 이슈

- [kernel/ipcp.c](../kernel/ipcp.c)가 존재하며, 정적 `kernel/mutex.c` 기반 v2.2.4 목표와 다르다.
- 현재 구현에는 `timer_tick_step`, `task_sleep_ticks`, `get_tick_count`, 중첩 `enter_critical`/`exit_critical` 경로의 정합성 검토가 필요하다.
- `task_return_guard`와 `unhandled_trap_freeze`는 Master가 요구하는 별도 심볼인지 실제 구현에 존재하는지 분리해서 판정한다.
- Running 태스크의 Ready 비트 복귀 여부와 동적 priority가 직접 매핑 bitmap과 충돌하지 않는지 검증한다.

## 갱신 규칙

구현 변경 시 먼저 소스와 이 문서의 계약을 갱신하고, 이후 적합성 대조표에서 판정을 갱신한다. Master 문서는 실제 구현이 요구사항을 충족하는 경우에만 개정한다.
