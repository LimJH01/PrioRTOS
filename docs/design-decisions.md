# PrioRTOS 설계 결정 기록

이 문서는 요구사항에 영향을 주는 설계 선택과 미결정 사항을 기록한다. 선택을 확정할 때는 근거, 영향받는 요구사항, 검증 방법을 함께 적는다.

상태:

- `제안`: 토론을 위한 초기 권고이며 아직 확정되지 않음
- `확정`: 프로젝트 책임자가 선택을 승인함
- `대체됨`: 후속 결정으로 교체됨

## 미결정 항목

| ID | 결정할 항목 | 현재 코드에서 확인한 사실 | 제안 / 선택지 | 영향 |
|---|---|---|---|---|
| `DEC-001` | 초기 대상 플랫폼 | Makefile은 RV32I + Zicsr, `ilp32`, QEMU `virt` 실행 타깃을 지정한다. | 제안: 우선 QEMU `virt`, RV32 단일 hart, M-mode만 프로토타입 기준으로 고정한다. 실제 보드는 별도 단계에서 추가한다. | `SYS-001`, `SYS-002`, `TRAP-002`, 시간 측정 |
| `DEC-002` | 최대 태스크 수 | `MAX_TASKS`는 8이다. | 사용자 의도: 8개 슬롯을 유지한다. 슬롯 0은 idle, 슬롯 1..7은 응용 태스크에 배정한다. | `TASK-001`, `SCHED-002`, `SCHED-004` |
| `DEC-003` | 우선순위 방향과 동률 | 큰 숫자가 높은 우선순위이며, 현재 태스크 생성은 우선순위 중복을 거부한다. | 사용자 의도: 기본 우선순위 중복 금지, round-robin 금지. 구체화 제안: IPCP 유효 우선순위 동률은 slot 번호가 작은 태스크 우선. 승인 전 제안 상태다. | `TASK-003`, `SCHED-001`, `SCHED-002`, `SCHED-005` |
| `DEC-004` | 태스크 구성 시점 | 스택은 정적 배열이지만 `task_create()`가 부팅 중 호출된다. | 구체화 제안: boot 초기화 중 등록하고 `priortos_start()`에서 설정을 동결한다. 시작 뒤 생성/삭제는 오류로 거부한다. | `TASK-002`, `TASK-003` |
| `DEC-005` | 타이머 주기/timebase | 헤더 주석은 10 MHz와 10,000,000 tick을 기재(산술상 1초)하지만 일부 주석은 1 ms를 말한다. | 미결정: 실제 QEMU/대상 timebase를 확인하고 tick 주기 및 단위를 수치로 결정해야 한다. 현재 값을 기준 요구사항으로 간주하지 않는다. | `TIME-001`, `TIME-002`, WCET 및 응답시간 분석 |
| `DEC-006` | 태스크 period/deadline 정책 | 현재 태스크 예제에는 주기·deadline 메타데이터가 없다. | 구체화 제안: 각 task 설정에 period 또는 minimum inter-arrival, relative deadline, WCET, release jitter, 자원 목록을 요구한다. 초기 response-time 분석은 constrained deadline(`D<=T`) 및 jitter 0으로 제한하고 다른 경우 별도 분석법을 승인한다. 각 수치는 workload와 대상 플랫폼으로 정한다. | `TIME-003`, `TIME-DET-002`, `ERR-003` |
| `DEC-007` | 동기화 프로토콜의 정확한 변형 | 뮤텍스 API만 선언되어 있고 구현은 없다. | 구체화 제안: 자원 ceiling은 사용 task 기본 우선순위 최댓값, 새 lock은 자원이 free이고 caller effective priority가 다른 task 보유 자원의 system ceiling보다 높을 때 허용. 재귀 lock/비소유자 unlock은 오류, 중첩은 같은 ceiling 규칙 적용. 세부 규칙은 [requirements.md](./requirements.md) 3.4절 참조. | `IPC-001`–`IPC-004` |
| `DEC-008` | 뮤텍스 획득 시 차단 동작 | 태스크 상태에 BLOCKED가 있지만 뮤텍스 대기 동작은 없다. | 구체화 제안: lock 불가 시 요청 mutex ID를 TCB에 기록하고 BLOCKED 처리한다. unlock 시 유효 우선순위 내림차순/slot 오름차순으로 재평가하고 소유권 이전 후 READY로 만든다. 승인 전 제안 상태다. | `TASK-004`, `TIME-002`, `IPC-004` |
| `DEC-009` | 오류/안전 상태 정책 | panic은 인터럽트를 끄고 UART 메시지 후 WFI 루프에 진입한다. | 구체화 제안: prototype panic은 non-returning safe halt, deadline miss 정책은 별도 필수 설정으로 두며 설정 없이는 deadline 감시/제품 수용을 금지한다. 제품의 reset/외부 안전 신호 정책은 위험 분석 후 승인해야 한다. | `ERR-001`–`ERR-003` |
| `DEC-010` | O(1), CLZ ISA와 시간 상한 | 선택 코드는 bitmap과 `__builtin_clz`를 사용하지만 빌드는 `-O0`이고 생성 코드에서 `__clzsi2` 헬퍼 호출이 확인된다. ready bitmap 실제 저장형은 `uint32_t`다. | 사용자 선택: bitmap은 당분간 `uint32_t`로 유지해 향후 확장 여지를 둔다. 현재 `MAX_TASKS=8`, 확장 상한은 32 slot이며 미사용 상위 bit는 0으로 유지한다. task-count 비례 loop 금지, 최대 32-bit 범위 검사 제안. 단일 CLZ 명령 및 cycle 상한은 대상 ISA/CPU 측정 후 별도 승인한다. | `TASK-001`, `TASK-004`, `SCHED-004`, `TIME-DET-001`–`TIME-DET-003` |
| `DEC-011` | 검증/테스트 수준 | 현재 저장소에는 자동 테스트 및 정형검증 구현이 확인되지 않는다. | 구체화 제안: HOST는 순수 로직, QEMU는 RV32 기능/트랩, TARGET은 보드별 timing 검증으로 분리한다. 우선 단위·QEMU 테스트와 trace matrix를 만들고 IPCP 불변식 CBMC는 IPCP 구현 이후 추가한다. | `TEST-001`–`TEST-003` |

## 결정 기록 템플릿

새 결정을 확정할 때 다음 형식을 사용한다.

```text
ID:
상태: 확정
결정:
근거:
영향받는 요구사항:
검증 방법:
결정자:
날짜:
```
