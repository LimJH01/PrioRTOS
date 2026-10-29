# 태스크 대기/깨우기 구현 정리

이 문서는 A와 B가 번갈아 실행되도록 추가한 변경사항과 현재 한계를 설명한다.
여기서 구현 완료는 빌드 및 QEMU 동작을 확인했다는 뜻이며, 정형 검증이나 제품 수준 검증이 끝났다는 뜻은 아니다.

## 한눈에 보기

이전에는 스케줄러가 ready bitmap에서 가장 높은 우선순위 태스크를 고르는 기능은 있었지만,
태스크가 기다리거나 다른 태스크를 깨우는 공통 API와 이를 실제 문맥 전환에 연결하는 경로가 없었다.

이번 변경은 그 사이를 연결했다.

```text
태스크가 semaphore_wait() 호출
  ├─ 카운트가 있으면 하나 소비하고 계속 실행
  └─ 카운트가 없으면 현재 태스크를 BLOCKED 처리
       └─ MSIP 소프트웨어 인터럽트
           └─ 기존 trap/context 경로가 다음 READY 태스크 실행

태스크가 semaphore_signal() 호출
  ├─ 기다리는 태스크가 있으면 숫자가 가장 작은(가장 높은) 우선순위 대기자를 READY 처리
  └─ 대기자가 없으면 카운트를 하나 증가
       └─ MSIP로 스케줄러에 재선택 요청
```

커널은 호출자가 A인지 B인지 분기하지 않는다. A와 B는 같은 `semaphore_wait()` /
`semaphore_signal()` API를 사용하고, 서로 다른 초기 카운트의 세마포어를 통해 순서를 정한다.

## 파일별 변경

| 파일 | 변경 내용 |
|---|---|
| [prio_rtos.h](../kernel/include/prio_rtos.h) | 공개 `Semaphore_t`, 결과 코드, `semaphore_init()`, `semaphore_wait()`, `semaphore_signal()` API를 추가했다. |
| [kernel_internal.h](../kernel/include/kernel_internal.h) | 스케줄러 내부의 인터럽트 잠금, 현재 태스크 차단, 태스크 깨우기 함수를 선언했다. |
| [sched.c](../kernel/sched.c) | 인터럽트를 보존하며 비활성화/복구하는 함수, 현재 태스크의 `RUNNING -> BLOCKED` 전이, `BLOCKED -> READY` 전이, `task_yield()`의 MSIP 요청을 추가했다. |
| [semaphore.c](../kernel/semaphore.c) | 세마포어 카운트와 대기자 bitmap을 관리한다. 대기자는 고정된 태스크 우선순위 bitmap으로 표현하며 동적 메모리를 쓰지 않는다. |
| [trap.c](../kernel/trap.c) | machine software interrupt와 timer interrupt를 처리하고, 두 경우 모두 기존 `sched_schedule()` 경로를 거치게 했다. |
| [tasks.c](../app/tasks.c) | `sem_a_turn` 초기값 1, `sem_b_turn` 초기값 0으로 설정했다. A는 A 차례를 기다린 뒤 A를 출력하고 B를 깨우며, B도 반대로 동작한다. |
| [README.md](../README.md), [implementation-map.md](./implementation-map.md) | 새 세마포어 파일과 태스크 전환 흐름을 문서에 반영했다. |

`kernel/ipcp.c`는 고정 ID mutex에 대해 IPCP를 처리한다. 초기화 시 `mutex_configure(id, user_slots)`로 사용 태스크 슬롯을 등록하면 기본 우선순위로 ceiling을 계산한다. ID와 사용자 목록은 스케줄러 시작 때 동결된다. lock은 요청 mutex의 free 여부와 system ceiling을 검사하며, 획득할 수 없는 태스크는 해당 mutex의 고정 waiter bitmap에서 대기한다. unlock은 보유 mutex의 ceiling으로 유효 우선순위를 재계산하고, system-ceiling 조건을 통과한 대기자에게 소유권을 넘긴 후 READY로 만든다. 세마포어는 IPCP와 별도 기능이다.

사용자 비트맵은 `g_tcbs[]`의 슬롯 번호를 사용한다. 현재 `task_create()`는 기본 우선순위와 슬롯 번호를 같은 인수로 지정하므로, 예를 들어 슬롯 1과 2가 자원을 쓰면 초기화 코드에서 다음처럼 등록한다.

```c
enum { UART_MUTEX_ID = 0U };

void resources_init(void) {
    uint32_t uart_users = (1U << 1) | (1U << 2);
    if (mutex_configure(UART_MUTEX_ID, uart_users) != MUTEX_OK) {
        kernel_panic("mutex configuration failed");
    }
}

void write_uart_from_task(void) {
    if (mutex_lock(UART_MUTEX_ID) != MUTEX_OK) {
        kernel_panic("mutex lock failed");
    }
    uart_putc('A');
    if (mutex_unlock(UART_MUTEX_ID) != MUTEX_OK) {
        kernel_panic("mutex unlock failed");
    }
}
```

## A/B 교대가 되는 이유

두 세마포어의 초기값은 다음과 같다.

```text
sem_a_turn = 1  // A는 즉시 통과 가능
sem_b_turn = 0  // B는 A가 신호를 줄 때까지 대기
```

각 태스크의 루프는 같은 패턴을 따른다.

```text
A: wait(A 차례) -> 'A'를 여러 번 연속 출력 -> signal(B 차례)
B: wait(B 차례) -> 'B'를 여러 번 연속 출력 -> signal(A 차례)
```

세마포어 카운트가 0일 때 `wait`는 현재 태스크를 `BLOCKED`로 바꾸고 ready bitmap에서 제외한다.
`signal`은 대기자를 깨워 `READY`로 바꾸고 ready bitmap에 다시 넣는다.
따라서 스케줄러의 기존 고정 우선순위 선택 규칙이 다음 실행 태스크를 고른다.

## 실제 문맥 전환 경로

`sched_schedule()`은 다음 태스크를 선택하고 `g_current_tcb`를 갱신한다.
그 자체가 CPU 레지스터를 전환하는 코드는 아니다. 실제 저장/복원은 기존
[context.S](../kernel/context.S)의 trap entry/return 경로가 담당한다.

이 구현은 CLINT `MSIP`를 소프트웨어 재스케줄 요청으로 사용한다. 요청은 `MSIE`와 전역 `MIE`가
활성화된 시점에 trap entry로 전달되고, trap entry가 태스크 문맥을 저장한 뒤
`trap_handler()`가 pending 비트를 지우고 스케줄러를 호출한다. 비동기 인터럽트이므로
`mepc`를 직접 증가시키지 않고, 인터럽트가 발생한 명령 위치로 복귀한다.

## 확인한 결과

- `make -j2`: 빌드 성공
- QEMU 실행: UART에서 `AAAAAAAA...BBBBBBBB...` 형태로 묶음 출력 확인
- `git diff --check`: 공백 오류 없음

QEMU 실행은 기능 확인이지 실기기 timing/WCET 검증은 아니다.

## 아직 구현하지 않은 것과 주의점

1. **Idle task가 없다.** 모든 응용 태스크가 동시에 BLOCKED가 되면 ready bitmap이 0이 되어 현재 스케줄러가 panic한다. 이 A/B 데모에서는 하나의 토큰이 항상 진행 중이므로 정상 흐름에서 두 태스크가 동시에 기다리지는 않는다.
2. **`task_yield()`는 교대 API가 아니다.** 현재 태스크를 계속 READY로 두고 MSIP를 요청한다. 고정 우선순위 정책에서는 현재 태스크가 다시 선택될 수 있으며, 같은 우선순위 태스크 간 Round-Robin은 수행하지 않는다.
3. **IPCP 코드가 구현되었지만 아직 검증되지 않았다.** 정적 설정, ceiling, 유효 우선순위 조정과 waiter handoff가 연결되어 있다. IPCP 요구사항별 자동 검증은 추가해야 한다.
4. **자동 테스트 및 정형 검증은 아직 없다.** 현재 검증은 빌드와 QEMU 출력 확인에 한정된다.
5. **A/B 태스크 함수는 데모 코드다.** 제품 사용 전에는 오류 정책, 동시성 경계, idle 동작 및 trap 오류 처리를 별도로 검증해야 한다.

## 다음 작업

1. 슬롯 31에 idle task를 추가하고 ready bitmap이 비었을 때 idle로 전환한다.
2. 호스트 단위 테스트 또는 QEMU 테스트로 semaphore count, block/wakeup, 잘못된 호출을 검증한다.
3. IPCP의 설정 경계, system-ceiling 차단, nested lock, boost 복구 및 동순위 비선점 규칙을 자동 테스트한다.
