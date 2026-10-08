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
       └─ ecall
           └─ 기존 trap/context 경로가 다음 READY 태스크 실행

태스크가 semaphore_signal() 호출
  ├─ 기다리는 태스크가 있으면 가장 높은 우선순위 대기자를 READY 처리
  └─ 대기자가 없으면 카운트를 하나 증가
       └─ ecall로 스케줄러에 재선택 요청
```

커널은 호출자가 A인지 B인지 분기하지 않는다. A와 B는 같은 `semaphore_wait()` /
`semaphore_signal()` API를 사용하고, 서로 다른 초기 카운트의 세마포어를 통해 순서를 정한다.

## 파일별 변경

| 파일 | 변경 내용 |
|---|---|
| [prio_rtos.h](../kernel/include/prio_rtos.h) | 공개 `Semaphore_t`, 결과 코드, `semaphore_init()`, `semaphore_wait()`, `semaphore_signal()` API를 추가했다. |
| [kernel_internal.h](../kernel/include/kernel_internal.h) | 스케줄러 내부의 인터럽트 잠금, 현재 태스크 차단, 태스크 깨우기 함수를 선언했다. |
| [sched.c](../kernel/sched.c) | 인터럽트를 보존하며 비활성화/복구하는 함수, 현재 태스크의 `RUNNING -> BLOCKED` 전이, `BLOCKED -> READY` 전이, `task_yield()`의 ecall 요청을 추가했다. |
| [semaphore.c](../kernel/semaphore.c) | 세마포어 카운트와 대기자 bitmap을 관리한다. 대기자는 고정된 태스크 우선순위 bitmap으로 표현하며 동적 메모리를 쓰지 않는다. |
| [trap.c](../kernel/trap.c) | timer interrupt뿐 아니라 machine-mode `ecall`도 처리하고, 두 경우 모두 기존 `sched_schedule()` 경로를 거치게 했다. |
| [tasks.c](../app/tasks.c) | `sem_a_turn` 초기값 1, `sem_b_turn` 초기값 0으로 설정했다. A는 A 차례를 기다린 뒤 A를 출력하고 B를 깨우며, B도 반대로 동작한다. |
| [README.md](../README.md), [implementation-map.md](./implementation-map.md) | 새 세마포어 파일과 태스크 전환 흐름을 문서에 반영했다. |

기존 [mutex.c](../kernel/mutex.c)는 이번 변경에서 구현하지 않았다. IPCP mutex와 세마포어는 별도 기능이다.

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

이 구현은 `ecall`을 소프트웨어 재스케줄 요청으로 사용한다. trap entry가 태스크 문맥을 저장하고
`trap_handler()`가 스케줄러를 부른 뒤, 선택된 태스크 문맥을 복원한다.
`ecall`의 PC는 trap frame에 저장된 `mepc`에서 4를 더해 다음 명령으로 진행시킨다.

## 확인한 결과

- `make -j2`: 빌드 성공
- QEMU 실행: UART에서 `AAAAAAAA...BBBBBBBB...` 형태로 묶음 출력 확인
- `git diff --check`: 공백 오류 없음

QEMU 실행은 기능 확인이지 실기기 timing/WCET 검증은 아니다.

## 아직 구현하지 않은 것과 주의점

1. **Idle task가 없다.** 모든 응용 태스크가 동시에 BLOCKED가 되면 ready bitmap이 0이 되어 현재 스케줄러가 panic한다. 이 A/B 데모에서는 하나의 토큰이 항상 진행 중이므로 정상 흐름에서 두 태스크가 동시에 기다리지는 않는다.
2. **`task_yield()`는 교대 API가 아니다.** 현재 태스크를 계속 READY로 두고 ecall을 요청한다. 고정 우선순위 정책에서는 그 태스크가 다시 선택될 수 있다.
3. **세마포어는 IPCP mutex가 아니다.** 자원 소유권, priority ceiling, 상속/복원 같은 IPCP 규칙은 구현하지 않았다.
4. **자동 테스트 및 정형 검증은 아직 없다.** 현재 검증은 빌드와 QEMU 출력 확인에 한정된다.
5. **A/B 태스크 함수는 데모 코드다.** 제품 사용 전에는 오류 정책, 동시성 경계, idle 동작 및 trap 오류 처리를 별도로 검증해야 한다.

## 다음 작업

1. 슬롯 0에 idle task를 추가하고 ready bitmap이 비었을 때 idle로 전환한다.
2. 호스트 단위 테스트 또는 QEMU 테스트로 semaphore count, block/wakeup, 잘못된 호출을 검증한다.
3. 별도 설계 결정 후 IPCP mutex를 구현한다. 세마포어 데모를 mutex 구현으로 간주하지 않는다.
