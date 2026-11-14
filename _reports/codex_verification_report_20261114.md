환님, **ASIL-D 준비도 판정은 Fail입니다.** 안전 장치의 기반은 구현되어 있지만, E-STOP 리셋 우회, 제한 전환 시 slew 위반, 손상된 SHM의 정상 상태 재초기화, 오프라인 검증기와 게스트 FSM의 불일치가 확인됐습니다. 현재는 **개발·검증 단계**로 판단합니다.

감사 대상은 `feat/demo-hardening`, HEAD `013fefc04e9c30911c40ff0cb5301ecfa6f7f9f6`으로 요청과 일치했습니다. 소스는 변경하지 않았습니다. 아래에서 **실행 확인**과 **정적 분석**을 구분했습니다.

**1. Kernel secpol Type-ID 격리**

확인된 강점:

- `ConnectClientInfoExtraArgs(ctp->info.scoid, &ext, 0, NULL, 0, &tid)`는 현재 메시지의 서버 연결 ID를 사용합니다. 로컬 QNX 8 SDK의 `sys/neutrino.h:738` 선언과 인수 형식이 일치합니다. `ConnectClientInfoExt()`의 이중 포인터 형식과 혼동한 호출은 아닙니다.
- 조회 실패 시 Type-ID를 invalid로 설정하므로, secure 모드의 권한 비교는 거부 방향으로 동작합니다.
- steering/braking/power Type-ID를 각각 STEERING/BRAKE/THROTTLE 채널과 대조합니다. bridge에는 모든 채널을 쓰는 명시적 예외가 있습니다.
- secure 모드에서 정책 로드 실패, Type-ID 누락, 자신의 Type-ID 불일치 시 프로세스가 종료됩니다.
- watcher는 `spawn_envp`에 기본적으로 `SAFEPULSE_SECURE=1`을 넣고, 자식 UID 및 secpol 타입도 지정합니다.

근거: [Type-ID 조회 및 권한 검사](C:/AI/projects/qnx-safepulse-lab/guest/resmgr/safepulse_resmgr.c:113), [spawn 설정](C:/AI/projects/qnx-safepulse-lab/guest/safepulse_watcher.c:138). QNX 공식 문서도 리소스 매니저의 클라이언트 조회 기준을 `ctp->info.scoid`로 설명합니다. [QNX iofunc_client_info_ext](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/i/iofunc_client_info_ext.html)

**Major — production/test 빌드 분리가 기본 빌드 경로에 없습니다.**

크래시 명령 본체는 `SAFEPULSE_FAULT_INJECTION`으로 감쌌지만, **Makefile과 build.bat가 모두 해당 매크로를 항상 정의**합니다. 따라서 기본 산출물에는 의도적 SIGSEGV/FPE/ABRT/ILL 경로가 포함됩니다.

또한 watcher는 부모 환경의 `SAFEPULSE_SECURE=0`을 허용하고, resmgr를 직접 실행하면 환경변수가 정확히 `1`일 때만 보안 검사를 켭니다. production 구성이 컴파일 또는 검증된 실행 정책으로 강제되지 않습니다.

권고: production 기본 타깃에서 fault injection을 제외하고, 테스트 전용 타깃으로 분리하십시오. production에서는 secure 해제 경로도 차단해야 합니다.

근거: [Makefile](C:/AI/projects/qnx-safepulse-lab/guest/Makefile:37), [보안 모드 결정](C:/AI/projects/qnx-safepulse-lab/guest/resmgr/safepulse_resmgr.c:1068).

실제 커널 정책 아래의 교차 ECU 쓰기 차단은 이번 감사에서 재실행하지 않았습니다. 호출 형식과 코드 경로의 정적 확인입니다.

**2. SHM seqlock·프로파일 복원**

확인된 강점:

- 홀수 sequence를 쓰기 진행 상태로, 짝수를 커밋 상태로 사용합니다.
- writer의 release store 및 fence, reader의 acquire load/fence와 sequence 재검사가 있습니다.
- CRC에는 FSM 상태, latch, 시간, 위반 코드, profile ID 및 custom rules의 CRC가 반영됩니다.
- 홀수 sequence, sequence 불일치, CRC 오류, 잘못된 상태/profile 값은 E-STOP으로 복구합니다.
- 재연결 전에 `fstat`으로 최소 크기를 확인합니다.
- head/tail은 누적 카운터이므로 각각이 capacity보다 큰 것은 정상입니다. 저장 슬롯은 마스크로 제한하고, 재시작 시 unsigned `head-tail > capacity`를 검사하는 방식은 타당합니다.

근거: [seqlock 커밋](C:/AI/projects/qnx-safepulse-lab/guest/resmgr/safepulse_resmgr.c:267), [복원 검증](C:/AI/projects/qnx-safepulse-lab/guest/resmgr/safepulse_resmgr.c:1141).

**Major — truncated SHM 복구가 E-STOP을 지우고 NORMAL로 시작합니다.**

기존 SHM 크기가 작으면 `ftruncate` 후 `is_new_shm=1`로 처리합니다. 이후 `ringbuffer_init()`과 `safe_state_fsm_init()`을 호출하므로, 기존 E-STOP latch를 보존할 수 없는 손상 상황이 정상 초기화로 연결됩니다. CRC 손상 경로와 안전 정책이 다릅니다.

권고: 최초 생성과 기존 SHM 손상을 구분하고, 손상 복구는 E-STOP 및 진단 코드를 남겨야 합니다.

근거: [크기 검사와 재초기화](C:/AI/projects/qnx-safepulse-lab/guest/resmgr/safepulse_resmgr.c:1099).

**Major — 프로파일 복원 실패 시 더 느슨한 규칙으로 전환될 수 있습니다.**

복원한 TRUCK/CUSTOM 프로파일을 적용하지 못하면 PASSENGER로 fallback합니다. 예를 들어 TRUCK 제한을 적용해야 하는 상황에서 규칙 파일이 없어지면 다른 제한으로 실행될 수 있습니다.

권고: 검증된 동일 프로파일을 복원하지 못하면 안전 상태로 진입하고, 프로파일 변경을 별도의 승인된 동작으로 처리하십시오.

근거: [프로파일 fallback](C:/AI/projects/qnx-safepulse-lab/guest/resmgr/safepulse_resmgr.c:1206).

**Major — seqlock 보호 범위가 전체 crash transaction을 포함하지 않습니다.**

FSM 메타데이터에는 일관성 검사가 있지만, ring enqueue/dequeue, 통계, 응답은 하나의 트랜잭션이 아닙니다.

- dequeue에서 tail을 올린 뒤 `MsgReply()` 전에 죽으면 수신자가 받지 못한 패킷이 소비될 수 있습니다.
- ring이 가득 차 `ringbuffer_push()`가 실패해도 write는 성공 바이트 수를 반환합니다.
- 정상 상태에서 예기치 않게 죽으면 유효한 이전 NORMAL 상태를 복원할 수 있습니다. 시험용 크래시의 사전 E-STOP 커밋은 모든 실제 크래시를 대표하지 않습니다.

따라서 **seqlock 기반 상태 복원은 구현됐지만, crash 경계의 무손실 전달은 입증되지 않았습니다.**

근거: [pop 이후 응답](C:/AI/projects/qnx-safepulse-lab/guest/resmgr/safepulse_resmgr.c:395), [push 결과 처리](C:/AI/projects/qnx-safepulse-lab/guest/resmgr/safepulse_resmgr.c:606).

**Minor — `fstat()` 실패 시 초기화되지 않은 `st_shm.st_size`를 로그에 사용합니다.** 시스템 호출 실패와 크기 부족 분기를 나눠야 합니다. SHM 구조의 magic/version 검증도 추가하는 편이 좋습니다.

**3. Watcher 스케줄링·복구**

설정값은 watcher FIFO 55, resmgr SPORADIC 50/15, budget 3ms/10ms입니다. watcher가 `waitpid()`로 대기하므로 높은 우선순위를 계속 소비하는 busy loop는 아닙니다.

**Major — `<50ms` 복구 상한은 보장되지 않습니다.**

- resmgr budget 소진 후 우선순위 15는 ECU 19–22보다 낮습니다. CPU 부하 아래에서는 서비스 지연 가능성이 있습니다. SPORADIC은 실행 예산 소진 시 낮은 우선순위로 전환되는 정책입니다. [QNX Sporadic scheduling](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.sys_arch/topic/kernel_Sporadic_scheduling.html)
- watcher의 FIFO 설정 실패, resmgr의 실시간 스케줄링 설정 실패 후에도 실행을 계속합니다.
- watcher가 기록하는 “crash timestamp”는 실제 장애 시점이 아니라 `waitpid()` 반환 직후입니다. 장애부터 관측까지의 시간이 빠집니다.
- readiness는 `open()` 성공만 확인하며, 해당 서비스가 새 자식 PID인지 확인하지 않습니다. 반복 `open()` 자체의 블로킹 시간도 누적 timeout 계산에 포함되지 않습니다.
- readiness 200ms 실패 후에도 `waitpid()`로 들어가므로, 살아 있으나 응답하지 않는 자식을 복구하지 못합니다.
- circuit breaker는 최대 20ms backoff이며, 재시작을 중단하거나 독립 안전 상태를 확보하는 회로 차단 동작은 아닙니다.

근거: [watcher readiness](C:/AI/projects/qnx-safepulse-lab/guest/safepulse_watcher.c:78), [관측 시간 및 backoff](C:/AI/projects/qnx-safepulse-lab/guest/safepulse_watcher.c:190).

저장된 [secpol chaos 보고서](C:/AI/projects/qnx-safepulse-lab/_reports/secpol_chaos_report.md)는 10/10 복구, 최악 52.4ms를 기록합니다. 이 과거 결과도 `<50ms` 상한을 충족하지 않으며, 현재 커밋에 대한 새 실측은 아닙니다.

**4. C SDK slew clamp·정지 인터록**

강점은 SDK actuator 적용에서 `CLOCK_MONOTONIC`을 사용하고, 채널별 시계를 분리하며, E-STOP 감속을 `25 km/h/s`로 계산한다는 점입니다.

**Major — 공개 reset 함수로 인터록을 우회할 수 있습니다.**

`safepulse_try_reset_fsm()`은 속도를 검사하지만, 공개된 `safepulse_reset_fsm()`은 동일한 내부 reset을 검사 없이 호출합니다.

또한 검사 대상은 실제 차량 속도가 아니라 내부 `last_clamped_speed`입니다. 초기화·재시작 직후에는 0이어서, 유효한 속도를 한 번도 받기 전에 reset을 허용할 수 있습니다. 정확히 `0.05`도 허용하므로 요청하신 엄격한 `<0.05` 조건과 다릅니다.

권고: 모든 공개 reset 경로에 동일한 검증을 적용하고, 유효하고 최신인 독립 속도 측정값을 요구해야 합니다.

근거: [SDK reset 구현](C:/AI/projects/qnx-safepulse-lab/guest/safepulse_api.c:178), [공개 API](C:/AI/projects/qnx-safepulse-lab/include/safepulse_api.h:107).

**Major — 제한 전환 시 최종 hard clamp가 slew 계산을 무효화합니다.**

정적 계산 예시:

- 이전 속도 100km/h, 새 제한 50km/h, 경과 20ms라면 램프 결과는 99.5km/h입니다.
- 하지만 마지막 `max_speed_kmh` 검사에서 곧바로 50km/h로 바뀝니다.
- steering도 E-STOP의 `max_angle=0`을 최종 강제하므로, 계산된 복귀 속도와 무관하게 즉시 0도로 바뀔 수 있습니다.

즉, 일반 상태 전환에서 “연속적인 감속·조향 복귀”와 “즉시 hard ceiling” 요구가 충돌합니다. 안전 요구사항에서 우선순위와 실제 actuator 동작을 명확히 정해야 합니다.

근거: [속도 clamp](C:/AI/projects/qnx-safepulse-lab/guest/resmgr/safe_state_fsm.c:365), [조향 최종 clamp](C:/AI/projects/qnx-safepulse-lab/guest/resmgr/safe_state_fsm.c:293).

추가로 resmgr는 SDK와 달리 actuator clamp에 `pkt.timestamp_ns`를 사용합니다. SDK의 monotonic 보강이 게스트 경로 전체에 적용된 것은 아닙니다.

**5. Offline CLI·trace 검증**

이번에 **실행 확인한 결과**입니다. 보고서 파일을 쓰는 CLI 명령 대신 reader/evaluator를 메모리에서 직접 호출했습니다.

| 입력·검사 | 결과 |
|---|---|
| `specs/test_protocol_and_logic.py` | 5개 검사 통과 |
| `examples/sample_trace.json` | 15프레임, 위반 0, NORMAL |
| `examples/fault_trace.json` | 6프레임, `0x1003/0x1001/0x1002/0x1004` 탐지 |
| 글로벌 60ms 공백 | `0x1008`, E-STOP |
| 활성화된 단일 ECU 채널 소실 | `0x1008`, E-STOP |
| 동일 timestamp에서 조향 0→500 | **위반 없음, NORMAL** |
| NaN timestamp | **위반 없음, NORMAL** |
| 1ms 간격 조향 0→500 | **위반 없음, NORMAL** |
| 부팅부터 brake만 지속 수신 | **위반 없음, NORMAL** |

**Major — CLI 판정이 게스트 FSM과 일치하지 않습니다.**

CLI는 반복 위반 5회 또는 첫 위반 후 10ms가 지난 추가 위반으로 E-STOP에 진입합니다. 현재 게스트는 일반 위반에서 DEGRADED에 진입하고 3초 후 LIMP_HOME으로 전환합니다. 실제 CLI 입력에서 20ms 간격의 bounds 오류 두 번으로 E-STOP이 나왔습니다.

따라서 CLI의 최종 FSM 상태를 게스트 동작 검증 근거로 사용할 수 없습니다.

근거: [CLI FSM](C:/AI/projects/qnx-safepulse-lab/host/tools/safepulse_cli.py:118), [게스트 FSM](C:/AI/projects/qnx-safepulse-lab/guest/resmgr/safe_state_fsm.c:162).

**Major — 시간·jerk·starvation 검증에 탐지 공백이 있습니다.**

- rollback은 CLI에서 탐지하지만 동일 timestamp의 같은 채널 갱신과 비유한 timestamp는 거부하지 않습니다.
- CLI와 게스트 모두 5ms 미만의 jerk 검사를 생략하고 이력을 갱신하므로, 빠른 반복 입력으로 검사를 회피할 수 있습니다.
- 채널 watchdog은 두 번 수신한 뒤 활성화됩니다. 처음부터 오지 않거나 한 번만 온 필수 ECU는 다른 채널이 계속 살아 있으면 누락될 수 있습니다.
- 게스트 linter는 timestamp가 이전 값 이하일 때 `0x1003`을 설정하지 않습니다.

근거: [CLI timing 검사](C:/AI/projects/qnx-safepulse-lab/host/tools/safepulse_cli.py:146), [게스트 timing 검사](C:/AI/projects/qnx-safepulse-lab/guest/linter/linter_engine.c:118), [watchdog 활성화 조건](C:/AI/projects/qnx-safepulse-lab/guest/resmgr/safe_state_fsm.c:95).

**Major — offline trace 통과와 실제 replay의 동일성은 보장되지 않습니다.**

오프라인 CRC 검사는 실제 패킷 CRC를 재계산하지 않고 `crc_valid=False` 표시를 읽습니다. replay는 trace timestamp를 `time.time()`으로 교체하고, 프레임의 `crc_valid`와 `alive_counter` 대신 새 패킷을 생성합니다. 따라서 fault trace의 rollback·CRC 오류가 그대로 게스트에 전달되지 않습니다.

근거: [offline CRC 검사](C:/AI/projects/qnx-safepulse-lab/host/tools/safepulse_cli.py:194), [replay 재생성](C:/AI/projects/qnx-safepulse-lab/host/replay/can_replay.py:420).

**판정의 범위**

이번 감사에서 QNX 빌드 경고 0개, 게스트 단위 테스트, 현재 커밋의 QEMU 결합 테스트, 50회 장애 복구 및 무손실 검증은 재확인하지 않았습니다. 확인된 Major 결함만으로도 현재 ASIL-D 준비도는 **Fail**입니다. 단위 테스트 통과나 BIST의 ASIL-D 표시는 이 판정을 대체하지 않습니다.

우선 수정 대상은 **reset 인터록 단일화, actuator 전환 요구사항과 구현 정합성, SHM 손상 시 E-STOP 유지, production 빌드 격리**입니다. 그 후 동일 입력을 C 엔진·CLI·QNX 게스트에 적용하는 비교 검증과 실제 최악 부하 복구 측정이 필요합니다.

[임시 정리 확인]: 이번 감사에서 임시파일을 생성하지 않았으며, 기존 미추적 파일 2개는 그대로 보존했습니다.

