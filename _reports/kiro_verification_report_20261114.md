먼저 대상 파일을 찾고 커밋 상태를 확인하겠습니다.
# QNX SafePulse Lab Wave 2 (e9df7ca) 정적 코드 감사

환님, 소스 코드를 직접 읽고 감사했습니다. 이 환경에는 `qcc`와 QNX SDK, QEMU 실측이 없습니다. 따라서 빌드 경고 0개, secpol 실동작, 우선순위 동작은 검증하지 못했고, 코드 정적 판독만으로 판단했습니다. `python host/tools/safepulse_cli.py check examples/sample_trace.json --rules rules.json`만 실행했습니다.

## 지시문 전제와 실제 코드가 다른 점

| 지시문 | 실제 코드 |
|---|---|
| `seqlock_begin_read/check_read` 매크로, `seqlock_write_barrier()` | `ringbuffer.h/.c`에 없습니다. seqlock은 `safepulse_resmgr.c`의 `persist_fsm_impl()`(쓰기)과 `main()`(읽기)에 인라인으로 구현돼 있습니다. |
| BIST "부팅 시 및 주기적 점검" | 부팅 시 1회(`safepulse_run_bist()`)만 있습니다. 주기 실행 코드는 없습니다. |
| 조향 슬루율 300 deg/s | `MAX_STEERING_SLEW_RATE_DEG_S = 250` (NORMAL, E-STOP), DEGRADED 80, LIMP_HOME 40입니다. 300은 `max_jerk` 규칙값에만 있습니다. |
| CLI 8대 위반 중 Timestamp, Inconsistent, Deadlock, Dropped Frame | 이 이름의 검사는 없습니다. 구현된 것은 0x1001~0x1008입니다. |
| `sample_trace.json` 15-frame | 파일은 16줄이고 로더는 15 프레임을 읽습니다. 어느 줄이 제외되는지는 확인하지 못했습니다. |

---

## 1. secpol Type-ID 격리

**강점**
- 식별에 `ctp->info.scoid`로 커널에 질의합니다. 메시지 페이로드를 쓰지 않으므로 클라이언트가 값을 위조할 수 없습니다.
- 판정이 fail-closed입니다. `ConnectClientInfoExtraArgs` 실패 시 `SECPOL_INVALID_TYPE`이 되고, 이는 어떤 허용 타입과도 일치하지 않아 `EACCES`가 됩니다.
- 3중 검사입니다. open 시 UID와 type, write 시 센서 채널과 type, devctl 시 UID와 bridge type을 검사합니다.
- 권한 검증이 `INJECT_CHAOS`, `RESET_STATS`, `RESET_LATENCY`, `SYNC_EDGE`, `LOAD_PROFILE`, `LOAD_RULES`와 16바이트 chaos write에 모두 적용돼 있습니다.

**Major**
1. **보안 모드 silent degrade.** `init_secpol_type_enforcement()`가 `secpol_open` 실패, 타입 ID 누락, 자기 타입 불일치 중 하나라도 만나면 `SLOG2_WARNING`만 남기고 UID 검사만으로 계속 동작합니다. `SAFEPULSE_SECURE=1`이면 기동을 거부하거나 안전 상태로 가야 합니다.
2. **watcher가 환경변수를 넘기지 못할 가능성.** `posix_spawn(..., spawn_argv, NULL)`에서 `envp`가 NULL입니다. QNX가 NULL을 부모 `environ` 상속으로 처리하는지 SDK 문서로 확인하지 못했습니다. 상속되지 않으면 watcher가 기동한 resmgr는 `g_secure_mode=0`이 되고 모든 권한 검사가 `EOK`를 반환합니다. 환경변수를 명시적으로 전달하도록 바꾸는 것을 권고합니다.
3. **크래시 주입 경로가 운영 빌드에 상존.** `fault_type` 3, 6, 7, 8은 resmgr를 SIGSEGV, SIGFPE, SIGABRT, SIGILL로 종료시킵니다. bridge 권한만 있으면 안전 서비스를 중단시킬 수 있습니다. `#ifdef SAFEPULSE_FAULT_INJECTION`으로 분리하는 것을 권고합니다.
4. **빌드 미검증.** `ConnectClientInfoExtraArgs(scoid, &ext, 0, NULL, 0, &tid)`의 인자 시그니처와 `secpol_type_id()` 존재를 SDK 헤더로 확인하지 못했습니다. `qcc -Wall -Wextra` 0 warning 증거가 필요합니다.

**Minor**
- bridge 타입은 모든 센서 채널을 쓸 수 있습니다. 설계 의도이지만 bridge가 침해되면 스푸핑이 가능합니다.
- 16바이트 write 경로는 `strict_errors=false`라서 `EBUSY`와 `EINVAL`이 호출자에게 전달되지 않습니다.
- `.secpol` 정책 파일은 이번 감사 범위에서 읽지 않았습니다.

**평가: 조건부 적합.** 설계 방향은 적합합니다. 위 Major 1~4를 해소하고 qcc 빌드와 QEMU 실측을 확보해야 합니다.

---

## 2. SHM seqlock과 프로필 영속성

**강점**
- 쓰기 순서가 올바릅니다. seq를 홀수로 올리고 `seq_cst` fence를 둔 뒤 데이터를 쓰고, crc32를 release로 저장한 뒤 seq를 짝수로 올립니다.
- 읽기 쪽은 `seq1(acquire)`, 데이터, `seq2` 순서로 읽고 `seq1` 홀수, `seq1!=seq2`, CRC 불일치를 모두 거부합니다.
- 상태 값(`prev_state > ESTOP`)과 프로필 값(`> CUSTOM`)의 범위를 검증합니다.
- 검증에 실패하면 ESTOP으로 가는 fail-safe입니다. 홀수 seq가 남은 경우 `persist_fsm_impl`이 `seq++`로 짝수로 복구합니다.
- CUSTOM 프로필의 규칙 블롭 복구 시 `prev_custom[2047]='\0'`로 종결하고 CRC를 상태 CRC에 포함합니다.
- LOAD_RULES는 실패 시 `prev_cache`로 롤백합니다. 복사 길이는 `sizeof-1`이라 오버플로가 없습니다.

**Major**
1. **SHM 크기 미검증.** `O_EXCL` 생성 후 `ftruncate` 전에 크래시하면, 다음 기동은 `EEXIST` 경로로 들어가 `fstat`로 `sizeof(ringbuffer_t)`를 확인하지 않고 mmap합니다. 크기 0 객체 접근은 SIGBUS를 일으킬 수 있고, 이후 기동이 영구히 실패하는 루프가 됩니다.
2. **링 인덱스 미검증.** 복구 시 `head`, `tail`, `total_pushed`는 CRC와 seqlock 범위 밖입니다. `head-tail > CAPACITY`인 손상 값을 그대로 신뢰합니다. 복구 시 `count > CAPACITY`이면 `tail=head`로 보정하거나 ESTOP으로 처리해야 합니다.
3. **레이아웃 버전/매직 필드 없음.** 펌웨어 업데이트로 `ringbuffer_t` 레이아웃이 바뀌면 이전 SHM을 그대로 해석합니다.

**Minor**
- `persist_fsm_to_shm()`이 패킷마다 호출되고 `seq_cst` fence가 4회입니다. WCET 영향이 있는데, `benchmark_wcet` 실측값으로 확인되지 않았습니다.
- 복구 실패(ESTOP) 시 `target_profile`이 PASSENGER로 남아, 트럭 차량도 승용 규칙으로 재기동합니다.
- `ringbuffer_pop`의 `atomic_thread_fence(acquire)`는 앞선 acquire load와 중복됩니다. 기능상 문제는 없습니다.

**평가: 조건부 적합.** seqlock 프로토콜 자체는 적합합니다. SHM 크기와 인덱스 검증이 빠져 있습니다.

---

## 3. 우선순위와 감시자, BIST

**강점**
- watcher는 SCHED_FIFO 55로 resmgr(SCHED_SPORADIC 50/15, 3ms/10ms 예산)보다 높습니다. resmgr가 크래시한 뒤에도 복구 스케줄링이 보장됩니다.
- 재시작 백오프는 1~20ms이고, 10초 안에 5회 크래시하면 서킷브레이커가 동작합니다.
- resmgr는 10ms `dispatch_timeout`으로 센서가 모두 침묵해도 starvation을 판정합니다.
- BIST 10단계(linter, 경계, 타당성, CRC, FSM 듀얼레일, 에스컬레이션, E2E, NaN과 starvation, 파서)는 실패하면 `resmgr_attach` 전에 종료합니다.

**Major**
1. **실질 우선순위 문제.** QNX는 메시지 수신 스레드가 송신자 우선순위를 상속합니다. 채널에 `_NTO_CHF_FIXED_PRIORITY`를 설정한 코드가 없으므로, ECU(19~22)의 write를 처리하는 resmgr 스레드는 19~22에서 실행됩니다. 그 사이 우선순위 23~49 스레드가 선점할 수 있습니다. 예로 테스트 클라이언트가 45를 씁니다. "resmgr 50"은 idle 타이머 틱에만 해당합니다.
2. **SCHED_SPORADIC 소진 시 15로 강등.** 15는 ECU(19~22)보다 낮습니다. 예산이 소진되면 starvation watchdog 틱이 ECU 부하에 밀립니다.
3. **watcher의 hang 감지 없음.** `waitpid`로 종료만 감지합니다. resmgr가 크래시 없이 교착되면 복구되지 않습니다. 외부 heartbeat가 필요합니다.
4. **spawn 실패 경로의 복구 시간.** 실패하면 `sleep(1)` 후 `continue`합니다. 50ms 이내 복구 목표를 위반하고, 시도 횟수 상한과 에스컬레이션이 없습니다.
5. **BIST 주기 점검 없음.** 부팅 후 잠재 결함(RAM 비트 플립, CRC 테이블 손상)은 탐지하지 못합니다. 동일 코드 경로를 재사용하므로 다양성이 없습니다. CRC 테이블 값 자체의 정합성도 검사하지 않습니다.

**Minor**
- 자식이 부모(55)의 스케줄링을 상속해 BIST와 SHM attach 구간이 55에서 실행됩니다. `POSIX_SPAWN_SETSCHEDULER`로 명시하면 좋습니다.
- `waitpid`가 EINTR로 -1을 반환하면 자식이 살아 있는데 이중 spawn을 시도할 수 있습니다.
- watcher 자체의 감시자는 없습니다.
- BIST가 SHM, ringbuffer, secpol, 액추에이터 클램프를 다루지 않습니다.

**평가: 조건부 적합.** 우선순위 55와 50의 상하 관계는 타당합니다. 그러나 메시지 우선순위 상속 때문에 실행 우선순위 전제가 성립하지 않을 수 있어, QEMU에서 실측해야 합니다.

---

## 4. SDK 액추에이터 제한과 리셋

**강점**
- 감속 램프가 `ESTOP_BRAKE_DECEL_KMH_S=25.0`으로 구현돼 있고, `actuator_brake_speed_to_stop`이 resmgr와 SDK에서 같은 코드를 씁니다.
- 슬루 클램프가 `dt<=0`일 때 우회하지 않고 마지막 값을 유지합니다. NaN/Inf는 0으로 처리하고, 정적 한계를 슬루 뒤에 한 번 더 적용합니다.
- 상태별 조향 한계는 NORMAL 540도/250deg/s, DEGRADED 180도/80, LIMP 45도/40, E-STOP 0도입니다.

**Major**
1. **클라이언트 타임스탬프로 슬루 dt 계산.** `pkt->timestamp_ns`를 dt로 쓰고 상한은 0.5초입니다. ECU나 호출자가 타임스탬프를 크게 벌리면 패킷당 최대 0.5초분(250deg/s에서 125도)이 허용됩니다. resmgr는 starvation에 커널 시각을 쓰면서 슬루에는 쓰지 않습니다.
2. **SDK의 시간축 혼용.** `safepulse_inspect`가 `record_channel_rx`에 `pkt->timestamp_ns`를 넘깁니다. resmgr는 "커널 단조 시각에만 고정"한다고 명시한 것과 상충합니다. 호출자가 `safepulse_check_starvation`에 다른 시계를 쓰면 오탐이나 미탐이 생깁니다.
3. **리셋 인터록이 명령값 기반이고 E-STOP에만 걸림.**
   - `last_clamped_speed`는 throttle 패킷 수신으로만 갱신됩니다. powertrain ECU가 침묵하면 값이 고정돼 리셋이 영구 거부되거나, 실제 속도와 무관한 값이 됩니다.
   - 실측 속도 입력이 없습니다.
   - LIMP_HOME과 DEGRADED에서는 속도와 무관하게 리셋됩니다.
   - resmgr가 respawn되면 `actuator_slew_init`으로 0이 되어, 주행 중인데도 즉시 리셋이 허용됩니다.
4. **SDK와 resmgr의 리셋 동작이 다릅니다.** resmgr는 `linter_init`과 프로필을 재적용하지만 SDK는 linter 상태를 유지합니다. 임계값 0.05도 두 곳에 상수로 중복돼 있습니다.

**Minor**
- 첫 샘플(`timestamp==0`)은 슬루 없이 통과합니다.
- `MAX_BRAKE_SLEW_RATE_BAR_S`(150)는 정의만 있고 `apply_actuator_limits`에서 쓰지 않습니다. 브레이크는 하한만 적용됩니다.
- SDK 컨텍스트는 스레드 안전하지 않습니다.
- 가속과 감속 모두 25 km/h/s로 대칭 제한됩니다. 설계 의도인지 확인이 필요합니다.

**평가: 조건부 적합.** 램프 구현은 적합합니다. 타임스탬프 신뢰와 리셋 인터록 근거가 ASIL-D 요건에 미흡합니다.

---

## 5. 오프라인 CLI 린터와 CI

**실제 구현된 검사:** 0x1001(경계와 NaN/Inf), 0x1002(jerk), 0x1003(deadline), 0x1004(CRC, 입력에 `crc_valid` 필드가 있을 때만), 0x1005(throttle>85%이고 speed<1), 0x1006과 0x1007(E2E, `alive_counter`가 있을 때만), 0x1008(전역과 채널별 starvation).

**강점**
- NaN은 jerk 이력을 오염시키지 않도록 `continue`합니다. 수동 실행에서 15프레임을 로드해 PASS가 나왔습니다.
- `deadline_ms`가 없으면 `period_ms+tolerance`로 유도합니다.
- 구현된 규칙의 FSM 에스컬레이션(0x1004, 0x1005, 0x1008 즉시 E-STOP)이 BIST와 일치합니다.

**Major**
1. **지시문의 8대 위반 중 4개가 미구현입니다.** Timestamp(역행, 중복), Inconsistent, Deadlock, Dropped Frame은 없습니다. Dropped Frame은 `alive_counter`가 있을 때 E2E 점프로만 근사됩니다.
2. **`VehicleTraceReader`가 `frames.sort(key=t)`를 하므로** 타임스탬프 역전이 로드 시점에 사라집니다. 타임스탬프 위반은 구조적으로 탐지할 수 없습니다.
3. **OpenXC 입력에는 CRC와 alive 필드가 생성되지 않습니다.** 샘플 트레이스 기준으로 CRC와 E2E 검사는 도달하지 않는 코드입니다.
4. **음성 테스트 벡터가 없습니다.** `sample_trace.json`은 100ms 정상 주행뿐이라 어떤 위반 클래스도 유발하지 않습니다. CI의 `check` 3회는 모두 PASS(exit 0)만 확인하므로, 규칙 엔진이 위반을 놓쳐도 CI가 통과합니다. 위반 탐지 검증은 `specs/test_cli_verification.py`에 달려 있는데, 이번에 읽지 않았습니다.
5. **CI에 게스트 빌드나 테스트가 없습니다.** `qcc` 빌드, 게스트 단위 테스트, E2E, 카오스 마라톤이 모두 로컬 수동 의존입니다. AGENTS.md의 완료 기준 1, 3, 4번은 CI로 보장되지 않습니다.

**Minor**
- `report`가 "ASIL-D 한계를 완벽히 준수함" 문구를 생성합니다. 100ms 트레이스 1개 통과로 이런 단정은 할 수 없고, AGENTS.md 철칙 4번과도 충돌합니다.
- YAML 파서가 지수 표기(`1e3`)를 문자열로 읽습니다.
- `numpy`를 import하지만 CLI 본체에서 쓰는 곳이 보이지 않습니다.
- 샘플 트레이스의 `brake_pedal_status`가 boolean입니다. 로더의 매핑을 확인하지 못했습니다.

**평가: 부적합 (지시문의 8대 위반 기준).** 린터가 7종 검사를 갖춘 것은 사실이나, 요구된 Timestamp, Inconsistent, Deadlock, Dropped Frame은 없거나 근사입니다. 테스트 벡터와 CI도 양성 판정 위주입니다.

---

## 종합

| 영역 | 평가 |
|---|---|
| 1. secpol 격리 | 조건부 적합 |
| 2. SHM seqlock과 영속성 | 조건부 적합 |
| 3. 우선순위, 감시자, BIST | 조건부 적합 |
| 4. SDK 액추에이터와 리셋 | 조건부 적합 |
| 5. CLI와 CI | 부적합 |

AGENTS.md 4단계 완료 기준 중 현재 증거로 확인된 것은 호스트 CLI 실행 1건뿐입니다. qcc 빌드, 게스트 테스트, QEMU 8/8 게이트, 50회 카오스 마라톤은 이번 감사에서 확인하지 못했습니다. 따라서 "개발/검증 단계"로 유지하는 것이 맞습니다.

우선 수정 순서로는 다음을 권고합니다. (1) watcher의 `envp` 전달과 secure 모드 fail-closed, (2) SHM 크기와 인덱스 검증, (3) 슬루 dt를 커널 시각으로 전환, (4) 크래시 주입 코드 분리, (5) CLI의 타임스탬프 검사와 음성 트레이스 CI 추가.

수정 착수가 필요하면 말씀해 주십시오. 코드는 수정하지 않았습니다.
