# IPCP 구현 변경 기록

이 문서는 IPCP 구현 작업에서 기존 코드의 어느 부분이 바뀌었는지, 새 코드가 어떤 책임을 맡는지, 모듈 간 동작이 어떻게 이어지는지 기록한다. 기록은 현재 작업 트리의 변경을 기준으로 하며, 이 문서가 추가된 시점의 구현은 빌드와 QEMU 기본 실행을 확인했지만 IPCP 전용 자동 테스트는 아직 없다.

## 1. 변경 요약

기존에는 `mutex.c`가 동적 할당이 아닌 고정 크기 테이블에서 mutex를 생성하고 기본적인 잠금, 차단, 소유권 인계를 수행했다. mutex마다 ceiling과 사용 태스크 목록은 없었고, 태스크의 `priority`가 ready bitmap 인덱스처럼도 사용되고 있어 유효 우선순위가 바뀌는 IPCP 동작과 직접 연결되지 않았다.

이번 변경에서는 mutex 구현을 `ipcp.c`로 옮기고 정적 ID 및 사전 등록한 사용자 슬롯 비트맵을 사용하도록 했다. 등록 시 사용 태스크들의 기본 우선순위로 ceiling을 계산한다. lock은 자원 가용성과 system ceiling을 확인하고, 현재 태스크가 획득할 수 없는 경우 mutex의 대기자 bitmap에 슬롯을 기록한 뒤 BLOCKED 상태로 만든다. lock 획득/해제 및 소유권 이전에 맞춰 TCB의 유효 우선순위를 갱신한다.

태스크 식별과 스케줄 우선순위는 다음처럼 분리했다.

| 정보 | 저장 위치/표현 | 용도 |
|---|---|---|
| 태스크 슬롯 | `g_tcbs[]` 배열 인덱스 | TCB, ready bitmap, semaphore/mutex waiter bitmap에서 태스크를 계속 식별 |
| 기본 우선순위 | `TCB_t.base_priority` | 태스크가 원래 등록된 우선순위와 IPCP 복귀 기준 |
| 유효 우선순위 | `TCB_t.priority` | 현재 스케줄링 및 대기자 순서에 사용하는 값 |
| 준비 상태 집합 | `g_ready_bitmap`의 슬롯 비트 | READY 또는 RUNNING인 태스크의 존재 여부 |

유효 우선순위가 같아지는 상황을 금지하지 않는다. 스케줄러는 현재 태스크보다 숫자가 작은 유효 우선순위가 있을 때만 선점한다. 현재 태스크가 더 이상 RUNNING이 아니면 숫자가 가장 작은 유효 우선순위를 선택하고, 우선순위 동률에서는 슬롯 번호가 작은 태스크를 결정적인 기준으로 삼는다. 우선순위별 FIFO ready queue는 이번 변경에 포함하지 않았다.

## 2. 파일별 변경 내용

### 2.1 `kernel/ipcp.c` — IPCP 구현 모듈 신규 추가

새 파일은 mutex 설정과 IPCP 실행 상태를 한 곳에서 관리한다. 동적 메모리 할당 없이 `MAX_MUTEXES` 크기의 정적 배열을 사용한다.

#### 내부 mutex 상태

각 mutex 항목은 다음 상태를 가진다.

- `configured`: 해당 고정 ID가 초기화 때 설정되었는지 여부
- `locked`, `owner_slot`: 현재 잠금 상태와 소유 태스크 슬롯
- `user_slots`: lock이 허용된 태스크 슬롯을 나타내는 비트맵
- `waiters`: 해당 mutex를 기다리며 BLOCKED된 태스크 슬롯 비트맵
- `ceiling`: 사용자 태스크의 `base_priority` 최솟값으로 계산한 자원 ceiling(숫자가 작을수록 높은 우선순위)

별도의 `configuration_frozen` 플래그를 두고 스케줄러 시작 이후 mutex 설정 변경을 차단한다.

#### 태스크 및 ceiling 처리

- `current_task_slot()`은 `g_current_tcb`가 TCB 배열의 어느 슬롯을 가리키는지 확인하고, 현재 상태가 RUNNING인지 함께 검사한다.
- `configured_user_mask()`는 idle 슬롯 0과 `MAX_TASKS` 범위 밖 비트를 허용하지 않는 마스크를 만든다. 최대 32 슬롯일 때 shift overflow가 나지 않도록 32 슬롯 경우를 따로 처리한다.
- `system_ceiling(requesting_slot)`은 요청 태스크가 소유하지 않은, 현재 잠긴 mutex의 ceiling 최솟값을 구한다. 잠긴 자원이 하나도 없을 때는 `UINT32_MAX`를 반환해 ceiling 제한이 없음을 표시한다.
- `refresh_effective_priority(slot)`은 태스크의 기본 우선순위와 현재 보유한 모든 mutex의 ceiling 중 최솟값을 다시 계산해 `TCB_t.priority`에 저장한다. mutex 하나를 해제해도 더 높은 ceiling(더 작은 숫자)의 다른 mutex가 남아 있으면 그 ceiling을 유지하며, 더 이상 보유 mutex가 없으면 기본 우선순위로 복귀한다.
- `can_acquire()`는 요청 mutex가 설정되었고, 잠기지 않았으며, 호출 태스크가 사용자 목록에 들어 있는지 확인한다. 이어서 호출 태스크의 유효 우선순위가 다른 태스크들이 보유한 system ceiling보다 엄격히 높은지 검사한다.

#### 정적 설정 API

`mutex_configure(mutex_id, user_slots)`는 공개 mutex 설정 API다.

1. ID가 `0..MAX_MUTEXES-1`인지 확인한다.
2. 사용자 비트맵이 0이 아니고 idle 슬롯/미사용 상위 비트를 포함하지 않는지 확인한다.
3. 인터럽트를 보호한 상태에서 설정이 아직 동결되지 않았는지, 같은 ID를 이미 설정하지 않았는지 검사한다.
4. 각 지정 슬롯에 태스크가 실제 등록되어 있는지 확인한다.
5. 지정된 태스크들의 `base_priority` 최솟값을 ceiling으로 계산하고 mutex 항목을 설정한다.

실패 시 구체적인 `MutexResult_t` 오류 코드를 반환하며 해당 mutex 설정을 성공 상태로 만들지 않는다. 설정은 `priortos_start()`가 호출되기 전에 이루어져야 한다.

#### lock, block, 재개 처리

`mutex_lock(mutex_id)`는 잘못된 ID, 미설정 mutex, 잘못된 호출 문맥, 비사용자 태스크, 재귀 잠금을 검사한다.

- 자원이 비어 있고 system-ceiling 조건도 만족하면 현재 슬롯을 소유자로 설정하고 effective priority를 즉시 재계산한 뒤 `MUTEX_OK`를 반환한다.
- 자원이 사용 중이거나 ceiling 조건을 통과하지 못하면 호출 태스크의 슬롯 비트를 해당 mutex의 waiter bitmap에 넣고 `sched_block_current()`로 현재 태스크를 BLOCKED 처리한다.
- 대기 태스크가 소유권을 이전받아 다시 스케줄되면 `mutex_lock()`의 반복문이 다시 실행된다. mutex가 이미 자기 슬롯 소유로 넘어온 것을 확인하고 `MUTEX_OK`를 반환한다.
- 같은 태스크가 lock 호출을 재개하기 전에 동일 waiter로 중복 등록되는 상태는 `MUTEX_ERR_INVALID_WAITER`로 거부한다.

대기 태스크가 실제로 block된 뒤 재개했을 때 소유권을 확인하는 동작은 mutex handoff가 잠금 반환보다 먼저 완료되는 모델이다.

#### unlock 및 waiter handoff

`mutex_unlock(mutex_id)`는 현재 태스크가 실제 소유자인지 검사한다. 비소유자나 잘못된 문맥에서의 호출은 mutex 상태를 바꾸지 않고 오류를 반환한다. 정상 unlock은 다음 순서로 수행한다.

1. 현재 mutex의 잠금과 소유자를 해제한다.
2. 이전 소유자의 effective priority를 나머지 보유 mutex ceiling을 고려해 재계산한다.
3. `dispatch_eligible_waiters()`로 잠금 가능한 waiter를 다시 평가한다.
4. 선택한 waiter를 새 소유자로 먼저 기록하고, waiter bitmap에서 제거하고, effective priority를 갱신한 뒤 READY로 깨운다.
5. 더 이상 적격 waiter가 없을 때까지 반복한다. 다른 mutex의 ceiling 때문에 아직 대기해야 하는 태스크는 BLOCKED로 남겨 다음 unlock에서 다시 검사한다.
6. MSIP를 요청하고 인터럽트를 복구해 더 높은 우선순위 태스크가 준비되었다면 스케줄러가 실행하도록 한다.

대기자 순위는 effective priority 오름차순(숫자가 작은 순서), 동률이면 슬롯 번호 오름차순이다. 소유권은 wakeup보다 먼저 기록하여 waiter가 READY가 된 뒤 소유자 없는 mutex를 관찰하지 않도록 한다. 내부 waiter 상태가 TCB 상태와 불일치하면 `kernel_panic()`으로 커널 오류를 드러낸다.

#### 부팅 설정 검증과 동결

- `ipcp_validate_configuration()`는 시작 시 등록된 mutex마다 사용자 비트가 유효하고 지정된 슬롯에 태스크가 존재하는지 확인한다.
- `ipcp_scheduler_started()`는 첫 태스크 선택을 마친 뒤 설정을 동결한다.

### 2.2 `kernel/sched.c` — 슬롯 bitmap과 유효 우선순위 스케줄링 연결

#### 최고 우선순위 태스크 선택 변경

기존에는 ready bitmap의 가장 높은 set bit를 CLZ 기반으로 구해 그 인덱스를 곧바로 우선순위/태스크 번호처럼 사용했다. 이제 `sched_highest_priority_slot(task_bitmap)`이 bitmap에서 준비된 각 **슬롯**을 확인하고 각 TCB의 `priority`(유효 우선순위)를 비교한다.

- bitmap은 task slot을 식별하는 용도로만 사용한다.
- 더 높은 유효 우선순위(숫자가 더 작은 priority)가 우선 선택된다.
- 비교 우선순위가 같으면 이미 선택된 더 작은 슬롯을 그대로 유지한다.
- 함수 반환값은 우선순위 값이 아니라 `g_tcbs[]` 인덱스다. 준비 슬롯이 없으면 `MAX_TASKS`를 반환한다.

현재 `MAX_TASKS`는 32이므로 최대 32개 슬롯을 순회한다. 이 선택 방식은 우선순위별 ready queue가 아니며, 실행 비용은 슬롯 수에 비례한다.

#### 태스크 등록

`task_create()`의 중복 판정은 ready bitmap 비트 확인에서 `g_tcbs[prio].state != TASK_STATE_UNUSED` 확인으로 바뀌었다. 이는 ready bit를 제거한 BLOCKED 태스크도 이미 등록된 슬롯으로 취급하기 위함이다. 새 태스크 TCB에는 현재와 기본 우선순위를 모두 등록 시의 `prio`로 초기화한다.

#### 스케줄러 시작

`priortos_start()`는 idle 태스크 등록 후 다음 검증/초기화를 수행한다.

1. IPCP mutex 설정을 검사하고 설정 오류면 `kernel_panic("invalid IPCP configuration")`으로 시작을 거부한다.
2. ready bitmap에서 유효 우선순위 숫자가 가장 작은 슬롯을 찾는다.
3. 선택한 TCB를 RUNNING으로 설정한다.
4. `ipcp_scheduler_started()`를 호출해 설정을 동결한다.
5. 최초 문맥 전환을 수행한다.

#### 선점과 동순위 정책

`sched_schedule()`은 준비 bitmap에서 최고 유효 우선순위 태스크를 찾는다. 현재 태스크가 RUNNING이고, 선택된 태스크의 유효 우선순위 숫자가 현재 태스크보다 크거나 같으면 그대로 반환한다. 따라서 타이머 tick이나 yield에서 같은 우선순위 태스크로 교체되는 round-robin은 발생하지 않는다.

현재 태스크가 BLOCKED된 경우에는 이 유지 조건을 적용하지 않는다. 새로 선택한 태스크를 RUNNING으로 전환한다. 현재 태스크가 여전히 RUNNING인 상태에서 더 높은 우선순위 태스크가 준비된 경우에는 기존 태스크를 READY로 내리고 새 태스크를 RUNNING으로 바꾼다.

#### block/wakeup bitmap 인덱스 변경

`sched_block_current()`는 `TCB_t.priority`가 아니라 `g_current_tcb - g_tcbs`로 슬롯을 계산해 ready bit를 제거한다. `sched_wake_task()` 인수도 우선순위가 아니라 슬롯 번호로 명확히 변경했다. wake 시 BLOCKED인지 확인하고 해당 슬롯을 READY로 바꾼 뒤 ready bit를 복구한다.

### 2.3 `kernel/semaphore.c` — waiter를 우선순위가 아닌 슬롯으로 추적

Semaphore waiter bitmap은 이제 태스크의 effective priority가 아니라 TCB 슬롯을 나타낸다.

- `semaphore_wait()`에서 현재 TCB의 배열 인덱스를 계산해 해당 슬롯의 waiter bit를 설정한다.
- `semaphore_signal()`은 waiter bitmap을 `sched_highest_priority_slot()`에 넘겨 유효 우선순위 숫자가 가장 작은 대기 태스크의 **슬롯**을 구한다.
- 선택한 슬롯을 `sched_wake_task(slot)`으로 깨우고 해당 waiter bit를 제거한다.

이 변경으로 IPCP boost 때문에 태스크 우선순위가 변해도 semaphore가 엉뚱한 TCB 슬롯을 깨우지 않는다. semaphore의 count 및 API 반환 정책은 이 변경에서 바꾸지 않았다.

### 2.4 `kernel/include/kernel_internal.h` — 내부 스케줄러 계약 정리

- `TCB_t.priority` 주석을 현재 유효 우선순위로, `TCB_t.base_priority` 주석을 원래 기본 우선순위로 명시했다. 두 필드 자체와 TCB 배치는 새 필드를 추가하지 않고 그대로 유지했다.
- 기존 최고 set bit를 찾는 `SCHED_GET_HIGHEST_PRIO` 매크로와 ready bit 조작 매크로를 제거했다. 그 매크로는 bitmap 비트 번호를 곧 우선순위라고 간주하는 설계에 의존했다.
- `sched_wake_task()` 매개변수를 `slot`으로 명시했다.
- `sched_highest_priority_slot()` 선언을 추가했다.
- IPCP의 부팅 검증(`ipcp_validate_configuration()`)과 설정 동결(`ipcp_scheduler_started()`)을 위한 내부 선언을 추가했다.

### 2.5 `kernel/include/prio_rtos.h` — 공개 mutex API를 설정 기반으로 변경

- 런타임 풀에서 ID를 할당하던 `mutex_create(uint32_t *mutex_id_out)`를 제거했다.
- 고정 ID와 허용 태스크 슬롯 bitmap을 받는 `mutex_configure(uint32_t mutex_id, uint32_t user_slots)`를 추가했다.
- 기존 `mutex_lock()`과 `mutex_unlock()` 공개 이름은 유지했다. 실제 구현은 이제 `ipcp.c`가 제공한다.
- 설정 오류를 구분하기 위해 `MUTEX_ERR_INVALID_USERS`, `MUTEX_ERR_CONFIGURATION_FROZEN`, `MUTEX_ERR_ALREADY_CONFIGURED`를 추가하고, 미설정 ID 오류를 `MUTEX_ERR_NOT_CONFIGURED`로 명시했다.

API 사용 순서는 다음과 같다.

```c
task_create(...);  // 사용자 태스크를 먼저 모두 등록
mutex_configure(MUTEX_ID, USER_SLOT_BITMAP);
priortos_start();  // IPCP 설정 검증 후 설정 동결
```

### 2.6 `app/tasks.c` — IPCP 설정 및 데모 자원 사용 추가

- 슬롯 상수 `TASK_A_SLOT`, `TASK_B_SLOT`, 고정 자원 ID `OUTPUT_MUTEX_ID`를 정의했다.
- A/B 태스크가 UART 출력 burst를 시작하기 전 mutex를 잠그고, burst 완료 뒤 해제하도록 했다.
- `tasks_init()`은 두 태스크의 생성이 성공한 뒤 슬롯 1과 2를 사용자로 하여 mutex ID 0을 설정한다.
- semaphore 기반 A/B 교대는 유지했다. 따라서 현재 데모는 mutex API 통합과 UART 출력 보호 경로를 실행하지만, semaphore가 한 시점에 한 태스크만 실행하도록 교대시키므로 두 task 간 mutex 경합/대기 handoff를 입증하는 테스트는 아니다.

### 2.7 `kernel/mutex.c` — 기존 basic mutex 파일 삭제

기존 `mutex.c`의 mutex 테이블과 `mutex_create`, `mutex_lock`, `mutex_unlock` 구현은 제거했다. 동일한 이름의 lock/unlock API를 두 모듈이 중복 제공하지 않도록 mutex 책임은 `ipcp.c`로 이동했다.

기존 “사용 중인 슬롯을 찾아 동적으로 mutex ID를 할당”하는 사용법은 더 이상 유효하지 않다. 호출 코드는 고정 ID를 정하고, 초기화 시 사용자 태스크 슬롯 목록을 등록하도록 바뀌어야 한다. `Makefile`은 `kernel/*.c`를 wildcard로 수집하므로 새 `ipcp.c`를 자동으로 빌드하고, 삭제한 `mutex.c`는 더 이상 빌드 대상이 아니다.

### 2.8 `Makefile` — 낡은 주석 제거

`__builtin_clz`의 인라인 치환과 관련된 주석을 제거했다. 스케줄러가 해당 intrinsic/CLZ 매크로 기반 탐색을 사용하지 않고 슬롯 순회로 바뀌었기 때문이다. 컴파일 옵션이나 빌드 타깃은 변경하지 않았다.

## 3. 문서 변경

### `docs/requirements.md`

- 현재 IPCP 상태를 basic mutex 미구현에서 정적 설정, ceiling, system-ceiling 검사, effective priority 갱신 및 handoff가 코드에 연결된 상태로 고쳤다.
- `IPC-001`–`IPC-004`의 동작 설명을 새 설정 API와 현재 구현 의미에 맞췄다.
- 구현 완료와 자동 테스트 완료를 구분해 “구현됨, 자동 검증 미구현”으로 기록했다.
- `IPC-004` API 반환 코드와 scheduler의 동순위 비선점 정책을 명시했다.

### `docs/design-decisions.md`

- `DEC-003`에 동순위 정책을 기록했다: 더 높은 유효 우선순위만 선점하고, 동률이면 현재 태스크를 유지하며, 현재 태스크가 runnable하지 않으면 슬롯 번호로 결정한다.
- `DEC-007`에 구현된 IPCP 변형의 ceiling 및 system-ceiling 조건을 적고 테스트 미완료를 표시했다.
- `DEC-008`에 mutex별 waiter bitmap과 unlock 시 소유권 우선 이전 동작을 기록했다.
- `DEC-010`에 슬롯 bitmap과 유효 우선순위 슬롯 순회 방식, 고정 상한의 비용 측정 필요성을 기록했다.

### `docs/implementation-map.md`

- 공개 API, 스케줄러 선택, mutex 구현 모듈 이름과 책임을 `ipcp.c` 및 설정 기반 IPCP로 갱신했다.
- IPCP 경로를 현재 연결된 기능으로 기술하고 자동 검증은 미완료라고 구분했다.
- idle fallback 및 ready bitmap의 슬롯 의미를 현재 구현과 맞췄다.
- 다음 작업을 IPCP 상태 전이 테스트, scheduler/semaphore 검증, WCET 측정으로 갱신했다.

### `docs/task-sync-progress.md`

- basic mutex 생성 예제(`mutex_create`)를 제거하고 고정 mutex ID 및 사용자 슬롯 bitmap을 등록하는 예제로 교체했다.
- API 호출 순서, waiter bitmap, ceiling 계산, handoff 동작을 요약했다.
- “IPCP 코드 구현, 자동 검증 미완료”라고 현재 상태를 구분했다.

### `README.md`

- IPCP 코드 위치를 `mutex.c`에서 `ipcp.c`로 바꿨다.
- 스케줄러가 CLZ/O(1) 선택을 한다는 표현을 유효 우선순위 순회 방식으로 갱신했다.
- task/mutex 수 상한으로 루프 실행 수가 제한되지만 실제 WCET 검증은 별도라는 점을 명시했다.

## 4. 실행 흐름

### 초기화

1. `tasks_init()`이 semaphore를 초기화한다.
2. `task_create()`가 응용 태스크를 슬롯 1과 2에 등록한다. 각 TCB에서 `priority`와 `base_priority`는 등록 기본값으로 시작한다.
3. `mutex_configure(0, (1U << 1) | (1U << 2))`가 mutex 0의 사용자와 ceiling을 기록한다. 현재 예제에서 ceiling은 두 사용자 기본 우선순위 중 최솟값이다.
4. `priortos_start()`가 idle 태스크를 등록하고 IPCP 설정을 검증한 다음 초기 실행 슬롯을 선택한다.
5. scheduler 시작 상태를 IPCP에 알리고 configuration을 동결한 뒤 최초 문맥 전환을 수행한다.

### 정상 lock/unlock

1. 태스크가 `mutex_lock(id)`를 호출한다.
2. 호출 태스크의 배열 슬롯과 RUNNING 상태, 설정된 사용자 여부를 검사한다.
3. 요청 mutex가 free이고 다른 태스크 소유 자원의 system ceiling보다 caller 유효 priority가 높으면 소유권을 설정한다.
4. 소유 자원 ceiling을 기준으로 caller의 effective priority를 다시 계산한다.
5. `mutex_unlock(id)`는 현재 소유자만 수행할 수 있다.
6. 해당 자원을 해제하고 소유자의 effective priority를 나머지 보유 자원에 따라 낮추거나 기본값으로 복원한다.
7. 잠금 가능한 waiter가 있다면 소유권 이전, priority 재계산, READY 전환 순서로 깨운다.
8. MSIP 경로가 스케줄러를 호출한다. 현재 태스크보다 엄격히 높은 우선순위가 준비되었을 때만 선점한다.

### 차단 및 재개

lock이 즉시 불가하면 mutex 별 waiter bitmap에 슬롯을 기록하고 scheduler가 ready bitmap에서 해당 슬롯을 지운 뒤 현재 태스크를 BLOCKED 처리한다. unlock이 해당 waiter에게 소유권을 직접 이전하고 READY로 바꾸면, 재개된 `mutex_lock()` 호출은 반복 검사를 통해 이미 자신이 소유자임을 확인하고 성공을 반환한다.

## 5. 구현에서 의도적으로 하지 않은 것

- **우선순위별 FIFO ready queue는 추가하지 않았다.** 중복 effective priority를 방지하는 자료구조가 아니며, 현재 정책은 배열 순회, 현재 태스크 유지, 슬롯 오름차순 tie-break다.
- `MAX_TASKS`를 8보다 크게 확장하지 않았다. 기존 slot bitmap 기반 구조와 task 생성 API의 범위는 별도 요구사항 변경이 필요하다.
- IPCP 규칙을 위한 unit test/CBMC harness를 추가하지 않았다.
- 보드 대상 WCET, timer latency 또는 IPCP blocking bound를 측정/증명하지 않았다.
- 기존 basic mutex의 `mutex_create()` API와 동적 ID 배정 호환성을 유지하지 않았다.

## 6. 검증 결과 및 남은 확인

이 변경 기록 작성 시 확인한 항목:

- `make all`: 성공. 링크에서 기존/현행 linker 설정의 RWX LOAD segment 경고가 출력됨.
- RISC-V toolchain `-fsyntax-only` 확인: `sched.c`, `ipcp.c`, `semaphore.c`, `app/tasks.c`에 대해 성공.
- QEMU `virt` 실행: 부팅 문구와 A/B 출력 반복을 확인. 제한 시간 종료로 QEMU 프로세스는 중단됨.
- `git diff --check`: 공백 오류 없음.

아직 필요한 검증:

1. 설정 API 오류 입력과 설정 동결 후 호출의 상태 불변성.
2. 사용자 아닌 태스크의 lock 거부 및 잘못된 호출 문맥 검사.
3. system-ceiling 조건으로 인한 block과 이후 unlock에 따른 재평가.
4. 단일/중첩 mutex의 ceiling 승격, 단계적 복구 및 기본 priority 복귀.
5. 복수 waiter의 effective-priority/slot 순서와 직접 소유권 이전.
6. 재개한 `mutex_lock()`이 성공으로 반환하고, 동시에 두 소유자가 생기지 않는지 확인.
7. 동순위 비선점, block된 현재 태스크 전환, semaphore waiter 우선순위 선택 테스트.
8. 모든 응용 태스크 block 시 idle로 실행되는 경로 확인.

---

이 문서는 변경 내역의 설명서이며, 요구사항 승인이나 IPCP 프로토콜 적합성 인증을 대신하지 않는다. IPCP 세부 변형과 제품 적용 조건은 `docs/requirements.md`와 `docs/design-decisions.md`의 상태를 함께 확인해야 한다.
