# QNX SafePulse Lab — 엔지니어링 업그레이드 백로그
**Branch:** `feat/demo-hardening` @ `4d5ad4e` | **Target:** BlackBerry QNX Global Hackathon 2026

---

## 1. [P0 — Must Implement Now] 지금 즉시 구현해야 할 3가지

### P0-1. HAL → resmgr 수신 루프 직결 (Zero-Copy Sensor Ingest Path)

**문제:** 현재 센서 데이터가 host 시뮬레이터 → 파일/파이프 → guest resmgr로 우회하는 구조라면, 심사위원이 "이게 진짜 QNX IPC인가?"를 의심할 수 있음. QNX의 핵심 가치(결정론적 IPC, priority inheritance)를 코드로 증명해야 함.

**생성/수정 파일:**
- `guest/resmgr/src/sp_hal_ingest.c` (신규)
- `guest/resmgr/src/sp_resmgr.c` (수정 — `MsgReceive` 루프에 HAL pulse 통합)
- `guest/resmgr/include/sp_hal.h` (신규)
- `guest/resmgr/src/sp_channel.c` (수정 — `ChannelCreatePulse` 등록)

**핵심 설계:**
```c
// sp_hal.h
typedef struct {
    uint64_t timestamp_ns;
    float    accel_g[3];
    float    gyro_dps[3];
    uint32_t seq;
    uint8_t  crc8;
} sp_sensor_frame_t;

typedef struct {
    int      hal_fd;              // /dev/sp_hal0 (custom resource manager)
    int      coid;                // connection to HAL resmgr
    uint32_t pulse_code;          // _PULSE_CODE_MINAVAIL + 1
    struct sigevent ev;
} sp_hal_ctx_t;
```

**아키텍처 연결:**
1. HAL을 별도 resmgr(`/dev/sp_hal0`)로 노출 → `SIGEV_PULSE_INIT`으로 `sp_resmgr`의 `chid`에 pulse 바인딩
2. `sp_resmgr`의 메인 루프를 `MsgReceivePulse` + `MsgReceive` 혼합 루프로 재작성
3. Pulse 수신 시 `MsgReceivePulse`의 `pulse.value.sival_int`에 `seq`를 실어 **drop detection** 구현
4. `SchedSet(0, SCHED_RR, ...)` + `ThreadCtl(_NTO_TCTL_IO, 0)`로 HAL 스레드 priority 20, resmgr priority 15 설정 → **priority inversion 방지 증명**

**심사위원에게 undeniable한 이유:**
- QNX의 `SIGEV_PULSE` + `MsgReceivePulse`는 RTOS 심사위원이 즉시 "아, 이 사람 QNX 안다"고 인식하는 시그니처
- Pulse drop 카운터를 `sp_metrics`에 노출하면 **결정론적 IPC의 실측 증거**가 됨
- Zero-copy가 아니라도 "pulse 기반 비동기 수신 + seq gap 검출"은 실무 QNX 패턴 그 자체

---

### P0-2. Closed-Loop Vehicle Physics Simulator (Host 측, 결정론적)

**문제:** 데모가 open-loop(센서 → 판단 → 로그)라면 "그래서 뭐가 안전해졌나?"를 증명 못 함. **판단 결과가 차량 거동을 바꾸고, 그 거동이 다시 센서로 피드백되는 closed-loop**가 있어야 safety case가 성립.

**생성/수정 파일:**
- `host/sim/src/sp_vehicle_model.c` (신규)
- `host/sim/include/sp_vehicle_model.h` (신규)
- `host/sim/src/sp_sensor_fusion.c` (수정 — 모델 출력을 센서 입력으로 주입)
- `host/sim/src/sp_main.c` (수정 — 100Hz 고정 스텝 루프)

**핵심 설계:**
```c
// sp_vehicle_model.h
typedef struct {
    double x, y, yaw;          // pose
    double v_long, v_lat;      // body velocity
    double yaw_rate;
    double steer_angle;
    double a_cmd;              // from SafePulse decision
    double dt;                 // 0.01s fixed
} sp_vehicle_state_t;

typedef struct {
    double wheelbase;          // 2.7m
    double max_steer;          // 0.6 rad
    double max_decel;          // -8.0 m/s^2
    double mass;               // 1500 kg
} sp_vehicle_params_t;

void sp_vehicle_step(sp_vehicle_state_t *s, const sp_vehicle_params_t *p,
                     double a_cmd, double steer_cmd);
void sp_vehicle_to_sensor(const sp_vehicle_state_t *s, sp_sensor_frame_t *out);
```

**아키텍처 연결:**
- **Kinematic bicycle model** (횡방향) + **longitudinal 1차 지연** (구동계 관성)
- SafePulse guest가 내린 `a_cmd`(감속 명령)를 `host/sim`이 수신 → `sp_vehicle_step()` → `sp_vehicle_to_sensor()` → HAL로 재주입
- **100Hz 고정 스텝** + `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`으로 결정론 확보
- 시나리오: 전방 장애물 → TTC(Time-To-Collision) < 1.5s → SafePulse AEB 트리거 → 실제로 8 m/s² 감속 → 정지거리 로그

**심사위원에게 undeniable한 이유:**
- "판단 → 제어 → 물리 → 센서" 루프가 닫혀야 **ISO 26262 SOTIF** 관점의 시나리오 검증이 성립
- 정지거리, TTC, jerk를 수치로 제시 가능 → **정량적 safety metric**
- QNX guest가 실제로 차량을 멈췄다는 **end-to-end 증거**

---

### P0-3. Hardware Watchdog Kicker + Safety State Machine

**문제:** QNX 심사위원은 "watchdog 없이 safety-critical이라고?"이라고 반드시 물음. `sp_guard` 프로세스가 heartbeat를 못 받으면 **fail-safe 상태로 전이**하는 구조가 필수.

**생성/수정 파일:**
- `guest/guard/src/sp_watchdog.c` (신규)
- `guest/guard/src/sp_safety_fsm.c` (신규)
- `guest/guard/include/sp_safety_fsm.h` (신규)
- `guest/resmgr/src/sp_resmgr.c` (수정 — 매 루프 `sp_wd_kick()` 호출)
- `build/qnx/aarch64/guard.build` (수정 — `-lwd` 링크)

**핵심 설계:**
```c
// sp_safety_fsm.h
typedef enum {
    SP_STATE_INIT = 0,
    SP_STATE_NOMINAL,
    SP_STATE_DEGRADED,     // 센서 1개 이상 stale
    SP_STATE_SAFE_STOP,    // AEB / limp-home
    SP_STATE_FAULT,        // watchdog timeout
} sp_safety_state_t;

typedef struct {
    sp_safety_state_t state;
    uint64_t last_kick_ns;
    uint32_t missed_kicks;
    uint32_t wd_timeout_ms;   // 200ms
} sp_safety_ctx_t;

void sp_fsm_tick(sp_safety_ctx_t *ctx, uint64_t now_ns);
void sp_wd_kick(sp_safety_ctx_t *ctx);
```

**아키텍처 연결:**
- QNX `watchdog` resource manager(`/dev/watchdog0`) 사용, `devictl`로 timeout 200ms 설정
- `sp_resmgr`가 매 센서 프레임 처리 후 `sp_wd_kick()` 호출
- `sp_guard`가 100ms 주기로 `sp_fsm_tick()` → `missed_kicks >= 2`면 `SP_STATE_FAULT` 전이 → **`SIGKILL` 대신 graceful safe-stop 명령을 vehicle model에 전송**
- FSM 전이는 `slog2`로 감사 로그 기록 (`slog2f` with `SLOG2_FAULT`)

**심사위원에게 undeniable한 이유:**
- Watchdog + FSM은 **ISO 26262 Part 6의 freedom from interference** 요구사항을 코드로 구현한 것
- "fail-operational → fail-safe" 전이를 데모에서 시연 가능 (resmgr에 `SIGSTOP` 걸어서)
- `slog2` 감사 로그는 QNX 심사위원이 가장 좋아하는 artifact

---

## 2. [P1 — High-Impact Upgrades] 다음 단계 3가지

### P1-1. GSN (Goal Structuring Notation) Safety Case 문서 + 자동 생성

**파일:**
- `docs/safety/gsn/sp_gsn_model.yaml` (신규 — GSN 노드 정의)
- `docs/safety/gsn/render_gsn.py` (신규 — YAML → Mermaid/SVG)
- `docs/safety/gsn/SP_SafetyCase.md` (신규 — 렌더 결과)
- `docs/safety/gsn/evidence_map.yaml` (신규 — 각 Goal에 대응하는 테스트/로그 매핑)

**설계:**
```yaml
# sp_gsn_model.yaml
goals:
  - id: G1
    text: "SafePulse는 센서 stale 시 200ms 내 safe-stop에 진입한다"
    supported_by: [S1, S2]
  - id: S1
    text: "Watchdog FSM이 DEGRADED→SAFE_STOP 전이를 보장"
    evidence: [test_fsm_transition, slog2_audit_log]
  - id: S2
    text: "resmgr pulse seq gap 검출이 3 프레임 내 stale 판정"
    evidence: [test_pulse_drop, metrics_dump]
```

**연결:** `evidence_map.yaml`의 각 evidence ID를 `tests/`의 실제 테스트 이름과 1:1 매핑 → CI에서 `pytest` 결과를 GSN에 자동 주입 → **"living safety case"**

**임팩트:** QNX 심사위원 중 기능안전 배경이 있는 사람은 GSN을 보는 순간 "이 팀은 진짜다"라고 판단. Mermaid 렌더로 GitHub에서 바로 보이게.

---

### P1-2. Deterministic Latency Benchmark Harness (`sp_bench`)

**파일:**
- `guest/bench/src/sp_bench_latency.c` (신규)
- `guest/bench/src/sp_bench_jitter.c` (신규)
- `scripts/run_bench.sh` (신규)
- `docs/bench/RESULTS.md` (신규 — 자동 생성)

**설계:**
- **Latency:** HAL pulse timestamp → resmgr decision timestamp → vehicle model ack timestamp의 3구간 측정
- **Jitter:** `ClockCycles()`로 10,000회 샘플, p50/p99/p99.9/max 출력
- **Priority inversion test:** resmgr priority를 15→5로 낮추고 HAL priority 20 유지 → latency 변화 측정 → **priority inheritance가 실제로 동작함을 수치로 증명**
- 결과를 `docs/bench/RESULTS.md`에 자동 append (CI artifact)

**임팩트:** "QNX는 결정론적이다"를 **p99.9 latency 수치**로 제시. Linux 대비 비교표까지 넣으면 완벽.

---

### P1-3. Multi-Guest Isolation Demo (QNX의 핵심 가치 시연)

**파일:**
- `guest/resmgr/src/sp_resmgr.c` (수정 — `procmgr_ability()`로 capability 제한)
- `guest/guard/src/sp_guard.c` (수정 — `setprio` 격리)
- `build/qnx/aarch64/*.build` (수정 — 각 guest별 `[uid=... gid=...]` 설정)
- `scripts/demo_isolation.sh` (신규)

**설계:**
- `sp_resmgr`를 **unprivileged uid**로 실행, `sp_guard`만 `PROCMGR_AID_IO` 보유
- 데모: `sp_resmgr`에 `kill -SEGV` → guard가 FSM을 `FAULT`로 전이 → vehicle model이 safe-stop → **resmgr crash가 전체 시스템을 죽이지 않음**을 시연
- `pidin -F "%a %N %h"` 출력을 데모 로그에 포함

**임팩트:** QNX 마이크로커널의 **fault isolation**을 라이브로 보여줌. "Linux였다면 커널 패닉"이라는 대비 강조.

---

## 3. [P2 — Polish & Integration] 최종 데모 극대화

### P2-1. 통합 데모 시나리오 스크립트 (`scripts/demo_full.sh`)
- **시나리오 A (Nominal):** 정상 주행 30초 → latency/jitter 로그
- **시나리오 B (AEB):** 전방 장애물 → TTC < 1.5s → safe-stop → 정지거리 출력
- **시나리오 C (Fault Injection):** `sp_resmgr` SIGSTOP → watchdog timeout → FSM FAULT → safe-stop
- **시나리오 D (Isolation):** `sp_resmgr` SIGSEGV → guard 생존 → safe-stop
- 각 시나리오 종료 시 `slog2info` + `sp_metrics` dump를 `docs/demo/run_<timestamp>/`에 저장

### P2-2. 실시간 대시보드 (`host/dash/`)
- `host/dash/src/sp_dash.c` (신규 — ncurses 또는 간단한 HTTP)
- vehicle state, FSM state, latency p99, pulse drop count를 100ms 갱신
- 데모 영상에서 **"실시간으로 안전 상태가 바뀐다"**를 시각적으로 증명

### P2-3. README + 아키텍처 다이어그램 재작성
- `docs/ARCHITECTURE.md` (신규) — Mermaid로 HAL→resmgr→guard→vehicle model 데이터플로우
- `README.md` 상단에 **"3분 데모 영상 링크 + p99 latency 수치 + GSN 링크"** 배치
- `docs/COMPLIANCE.md` — ISO 26262 Part 6 / IEC 61508 매핑 표

---

## 4.