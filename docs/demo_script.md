# QNX SafePulse Lab — 시연 영상 대본 메모 (2분 30초 구성, 3분 이내)

> **목적**: PR #1 이후 고도화된 핵심 기능 3가지(**① 실시간 경로 AUTOSAR E2E Profile 1 적용**, **② 대시보드 QNX 커널 실제 FSM·스케줄러 상태 직결 표시**, **③ 150ms → 50ms로 3배 단축된 기아 감시 시간(`STARVATION_LIMIT_NS`)**)과 자가 치유(Self-Healing) 복구를 2분 30초 안에 직관적으로 보여주는 촬영 구성안입니다.

---

## 전체 타임라인 요약 (총 2분 30초)

| 구간 | 시간 | 핵심 장면 | 화면 구성 (좌: QNX 시리얼 콘솔 / 우: 3D 웹 대시보드) |
|:---|:---:|:---|:---|
| **Scene 1** | `0:00 ~ 0:30` (30초) | **8단계 BIST 부팅 & 대시보드 커널 실측 배지** | QNX 8.0 부팅 로그(`BIST 8/8 PASS`) + 대시보드 `FSM: NORMAL`, `RT: SCHED_SPORADIC (Pri 50)` |
| **Scene 2** | `0:30 ~ 1:10` (40초) | **실시간 경로 AUTOSAR E2E Profile 1 오류 포착** | `"AUTOSAR CRC-32 Bitflip"` 클릭 → `0x1004` 포착 및 카운터 건너뜀(`0x1007`)·반복(`0x1006`) 검증 |
| **Scene 3** | `1:10 ~ 1:45` (35초) | **단축된 50ms 기아 감시(`STARVATION_LIMIT_NS`)** | 정상 주행 오탐 0건 확인 후 센서 스트림 차단 → 50ms 즉시 `EMERGENCY_STOP` 전환 |
| **Scene 4** | `1:45 ~ 2:30` (45초) | **자가 치유 Watcher 복구(평균 61.4ms) & 블랙박스** | `SIGSEGV` 드라이버 크래시 주입 → 데이터 유실 0% 복구 및 6.0초 블랙박스 리플레이 |

---

## 장면별 상세 연출 및 내레이션 메모

### Scene 1: QNX 8.0 부팅, 8단계 BIST, 대시보드 실제 커널 상태 표시 (`0:00 ~ 0:30`)
- **화면 연출**:
  1. 좌측 터미널에서 QNX Neutrino 8.0 마이크로커널이 부팅되며 `/dev/safepulse` 리소스 매니저가 **8단계 Power-On BIST**(`[BIST 8/8] AUTOSAR E2E Profile 1 Inline Verification: PASS`)를 통과하는 로그를 클로즈업합니다.
  2. 우측 3D 디지털 트윈 대시보드 상단 배지를 줌인합니다:
     - 기존 추정치(`FSM (Est.)`)가 아닌, QNX 커널이 패킷 `reserved[8..11]`과 `DCMD_SAFEPULSE_GET_STATS`로 직접 내려보낸 **`FSM: NORMAL`** 및 **`RT: SCHED_SPORADIC (Pri 50)`** 실제 값이 표시됨을 보여줍니다.
     - 브리지 연결 전/해제 시에는 추측값 대신 **`알 수 없음 (UNKNOWN)`**으로 정직하게 표시되는 장면을 짧게(3초) 비교로 보여줍니다.
- **내레이션 포인트**:
  > "SafePulse Lab은 QNX Neutrino 8.0의 `/dev/safepulse` 리소스 매니저가 시작될 때 AUTOSAR E2E를 포함한 8단계 자체 진단(BIST)을 수행합니다. 우측 대시보드의 FSM 상태와 `SCHED_SPORADIC (Pri 50)` 배지는 호스트의 추측값이 아니라, QNX 커널 드라이버가 매 프레임 직접 스탬핑해 내려보내는 실제 커널 상태입니다."

---

### Scene 2: 실시간 경로 AUTOSAR E2E Profile 1 오류 포착 (`0:30 ~ 1:10`)
- **화면 연출**:
  1. 3개의 가상 ECU(`ecu_steering` 50Hz, `ecu_braking` 100Hz, `ecu_powertrain` 20Hz)가 매 패킷마다 `autosar_e2e_stamp_packet()`으로 16비트 Data ID, 4비트 롤링 카운터, `0xF4ACFB13` CRC-32를 스탬핑하여 전송 중임을 하단 프로토콜 인스펙터에서 보여줍니다.
  2. 우측 대시보드의 Chaos Engineering 패널에서 **`"AUTOSAR CRC-32 Bitflip"` (Fault 4)** 버튼을 클릭합니다.
  3. 클릭 즉시:
     - `safepulse_bridge`가 AUTOSAR E2E CRC 바이트(`reserved[4]`)의 비트 1개를 뒤집어 `/dev/safepulse`에 주입합니다.
     - 리소스 매니저의 실시간 검사 경로(`linter_inspect_packet` → `autosar_e2e_protect_verify()`)가 이를 즉시 적발하여 **`VIOLATION_CRC_CORRUPTED (0x1004)`**를 발생시키고, 안전 점수가 `0.0`으로 떨어지며 커널 FSM 배지가 즉시 **`FSM: EMERGENCY_STOP`**(적색)으로 전환됩니다.
  4. 이어서 E2E 테스트 로그(`test_comprehensive_e2e.py`) 오버레이를 통해 **카운터 건너뛰기(`VIOLATION_E2E_SEQ_LOSS = 0x1007`)**와 **동일 카운터 반복(`VIOLATION_E2E_REPEATED = 0x1006`)**도 실시간 경로에서 각각 정확히 포착됨을 보여줍니다.
- **내레이션 포인트**:
  > "AUTOSAR E2E Profile 1 검증기(`autosar_e2e_protect_verify`)가 `/dev/safepulse`의 실시간 패킷 검사 경로에 직접 결합되어 있습니다. 대시보드에서 비트 플립을 주입하면 1마이크로초 대의 인라인 린터가 E2E CRC 불일치(`0x1004`)는 물론 롤링 카운터 건너뜀(`0x1007`)과 반복(`0x1006`)까지 즉시 잡아내고 차량을 `EMERGENCY_STOP` 안전 상태로 전환합니다."

---

### Scene 3: 3배 단축된 50ms 기아 감시(`STARVATION_LIMIT_NS`) (`1:10 ~ 1:45`)
- **화면 연출**:
  1. `Clear Fault Latch`로 정상 상태(`FSM: NORMAL`)로 복귀한 뒤, 10분(600초, 약 10만 프레임) 정상 주행 소크 테스트(`_reports/starvation_soak_report_2026-11-11.md`)에서 **오탐(False Positive) 0건**을 기록한 결과표를 화면 하단에 띄웁니다.
  2. 이어서 3개 가상 ECU의 센서 전송을 일시 중단(Fault 11 / `SIGSTOP`)시켜 버스 침묵을 유발합니다.
  3. 기존 `150ms`에서 **`50ms` (`STARVATION_LIMIT_NS = 50,000,000 ns`)**로 3배 단축된 `safe_state_fsm_check_starvation()`이 동작하여, QNX 시리얼 콘솔에 `[SafePulse FSM] CRITICAL: Sensor packet starvation detected (Silence >= 50 ms). Forcing EMERGENCY_STOP!` 로그가 찍히고 대시보드가 즉시 **`FSM: EMERGENCY_STOP`**으로 전환되는 모습을 보여줍니다.
- **내레이션 포인트**:
  > "센서 데이터 기아(Starvation) 감시 시간은 기존 150ms에서 Windows/QEMU 스케줄링 지터 하에서도 오탐이 없는 50ms(`STARVATION_LIMIT_NS`)로 3배 단축했습니다. 10분간의 연속 주행에서 오탐 0건을 검증했으며, 센서 통신이 끊기면 50ms 만에 즉시 `EMERGENCY_STOP`으로 진입합니다."

---

### Scene 4: 자가 치유 Watcher 복구(평균 61.4ms) 및 6.0초 블랙박스 리플레이 (`1:45 ~ 2:30`)
- **화면 연출**:
  1. 대시보드의 붉은색 **`"INDUCE KERNEL DRIVER FAULT (SIGSEGV)"`** 버튼을 클릭합니다.
  2. 좌측 QNX 콘솔에서 `/dev/safepulse`가 `SIGSEGV`로 크래시된 직후, `posix_spawn` 기반 `safepulse_watcher`가 프로세스를 재기동하고 8단계 BIST와 POSIX 공유 메모리(`/safepulse_ringbuf`) 재연결을 마쳐 **단일 E2E 62.5ms (50회 카오스 마라톤 평균 61.4ms, 중앙값 62.5ms, p95 75.5ms, 최대 88.7ms)** 만에 무손실 복구하는 로그를 보여줍니다.
  3. 복구 후에도 크래시 직전의 `EMERGENCY_STOP` 래치 상태가 공유 메모리를 통해 그대로 유지됨을 확인합니다.
  4. 마지막으로 대시보드의 **Blackbox Flight Recorder Scrubber** 슬라이더를 좌우로 움직여, 크래시 전후 6.0초간의 조향·스로틀·안전 점수 변화를 프레임 단위로 역추적하며 영상을 마무리합니다.
- **내레이션 포인트**:
  > "드라이버에 치명적인 `SIGSEGV` 메모리 폴트를 주입해도, 자체 감시 데몬(`safepulse_watcher`)이 `posix_spawn`으로 드라이버를 평균 61.4ms 만에 재기동합니다. 2MB POSIX 공유 메모리 링버퍼 덕분에 패킷 유실은 0%이며, 엔지니어는 6초 블랙박스 스크러버로 사고 직전 프레임을 정밀 분석할 수 있습니다."
