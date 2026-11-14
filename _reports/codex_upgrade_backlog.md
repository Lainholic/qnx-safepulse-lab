환님, **지금 가장 효과적인 투자는 ① 안전 명령과 실제 피드백을 분리한 폐루프, ② CAN HAL의 운영 경로 연결, ③ 멈춤 장애까지 감지하는 독립 안전 감시**입니다. 이 세 가지를 구현하면 “오류를 찾아 화면에 표시하는 데모”에서 **오류가 발생해도 차량 모델을 안전하게 제어하고, 그 결과를 검증하는 QNX 시스템**으로 발전합니다.

현재 `feat/demo-hardening`, 커밋 `4d5ad4e`를 확인했습니다. 아래는 실제 소스를 읽고 작성한 구현 제안입니다. 이번에는 파일 수정이나 빌드·QEMU 재시험을 수행하지 않았습니다. 심사 결과를 보장할 수는 있지만, 아래 작업은 기술적 설득력을 높이는 데 직접 연결됩니다.

## 1. P0 — Must Implement Now

### P0-1. 텔레메트리 보정에서 독립 차량 모델을 제어하는 폐루프로 전환

**현재 확인한 구현**

`guest/resmgr/safepulse_resmgr.c`의 `safepulse_io_write()`는 입력 패킷을 검사한 뒤 `val_primary`, `val_secondary`를 제한값으로 덮어씁니다. E-STOP에서는 `actuator_brake_speed_to_stop()`으로 출력 속도를 감소시킵니다. 반면 `guest/ecu/ecu_powertrain.c`의 속도는 스로틀 기반 수식으로 생성됩니다.

따라서 지금 출력되는 정지 속도는 독립적인 차량 모델이 브레이크 명령을 받아 계산한 피드백으로 검증되지 않습니다.

**수정·추가 파일**

| 구분 | 파일 | 구현 내용 |
|---|---|---|
| 추가 | `guest/include/safepulse_control.h` | 제어 명령·피드백·유효기간 정의 |
| 수정 | `guest/resmgr/safepulse_resmgr.c` | 입력 측정값 보존, 안전 명령 별도 생성 |
| 수정 | `guest/resmgr/safe_state_fsm.c` | FSM 제한값을 제어 명령으로 변환 |
| 추가 | `host/hil/vehicle_plant.py` | 고정 시간 간격으로 실행하는 차량 모델 |
| 수정 | `guest/safepulse_bridge.c` | 명령 송신·피드백 수신 연결 |
| 수정 | `host/bridge/qnx_bridge.py` | 차량 모델 연결 및 측정값/명령값 분리 |
| 수정 | `guest/include/safepulse_devctl.h` | 최신 명령 조회·피드백 전달 API |

**제안 API와 데이터 구조**

```c
typedef struct {
    uint64_t command_id;
    uint64_t issued_ns;
    uint64_t valid_until_ns;   /* QNX monotonic clock */
    double steering_deg;
    double throttle_pct;
    double brake_bar;
    uint32_t safety_state;
    uint32_t reason_code;
} sp_actuator_command_t;

typedef struct {
    uint64_t applied_command_id;
    uint64_t received_ns;      /* QNX가 수신 시 기록 */
    double speed_mps;
    double steering_deg;
    double brake_bar;
    uint32_t status_flags;
} sp_vehicle_feedback_t;

int sp_control_step(
    uint64_t now_ns,
    const actuator_limits_t *limits,
    const sp_vehicle_feedback_t *feedback,
    sp_actuator_command_t *out);

int sp_feedback_accept(
    const sp_vehicle_feedback_t *feedback,
    uint64_t rx_ns);
```

연결은 다음과 같습니다.

```text
차량 모델 센서 → QNX 린터 → FSM → 안전 명령
      ↑                              ↓
      └──── 차량 모델의 운동·제동 계산 ──┘
```

호스트 차량 모델은 10 ms 고정 스텝으로 속도·이동 거리·조향을 적분합니다. 초기에는 종방향 운동과 브레이크 응답 지연만 구현해도 충분합니다. 차량 질량, 제동 계수, 응답 지연을 설정값으로 두고 결과를 재현할 수 있게 합니다.

**반드시 적용할 설계**

- 센서 측정값과 제어 명령을 별도 레코드로 보존합니다.
- 센서 입력이 끊겨도 주기적인 제어 틱에서 안전 명령을 갱신합니다.
- 명령 수신이 끊기면 차량 모델 자체의 명령 유효기간 감시가 스로틀 차단·제동을 적용합니다.
- 호스트와 게스트의 절대 monotonic timestamp를 직접 비교하지 않습니다. 모델 측 유효기간은 수신 시점부터의 상대 시간으로 처리합니다.
- Reset은 보정된 출력 속도가 아닌 **신선한 피드백의 정지 상태**를 확인해야 허용합니다.

**통과 기준**

CRC 오류, ECU 침묵, resmgr 종료 각각에 대해 다음을 자동 검증합니다.

1. 안전 명령에서 스로틀이 차단되고 제동이 요청된다.
2. 모델이 해당 `command_id`를 적용했다고 응답한다.
3. 모델의 속도·정지 거리가 설정한 물리 모델과 허용 오차 안에서 일치한다.
4. 피드백이 오래됐거나 속도가 남아 있으면 Reset을 거부한다.

**심사 설득력:** “FSM이 E-STOP으로 바뀌었다”를 넘어 **감지 → 제어 → 독립 모델의 응답 → 정지 확인**을 보여줄 수 있습니다.

### P0-2. 기존 CAN HAL을 실제 검사 경로에 연결하고 검증 가능한 가상 CAN 백엔드로 만들기

**현재 확인한 구현**

`guest/hal/safepulse_hal_can.c`는 존재하지만, `guest/build.bat`에서 `hal_can.o`를 컴파일하는 것과 운영 경로 연결은 별개입니다. 현재 resmgr에는 HAL 호출이 없습니다. `host/hil/hil_bench_simulator.py`의 bus-off 시나리오 역시 호스트 카운터와 대기 시간으로 PASS를 생성합니다.

**수정·추가 파일**

- 수정: `include/safepulse_hal.h`
- 수정: `guest/hal/safepulse_hal_can.c`
- 추가: `guest/hal/safepulse_can_ingress.c`
- 수정: `guest/resmgr/safepulse_resmgr.c`
- 수정: `guest/include/safepulse_devctl.h`
- 수정: `guest/Makefile`, `guest/build.bat`
- 수정: `host/hil/hil_bench_simulator.py`

**정확한 연결 설계**

```c
int sp_can_decode(
    const hal_can_frame_t *frame,
    safepulse_packet_t *out);

int sp_ingest_packet(
    const safepulse_packet_t *input,
    uint64_t rx_ns,
    safepulse_packet_t *evaluated);

int hal_can_service(uint8_t channel, uint64_t now_ns);
```

`sp_ingest_packet()`을 기존 `safepulse_io_write()`에서 추출해 공통 검사 경로로 만듭니다.

```text
기존 ECU write ───────────────────────┐
                                    ├→ sp_ingest_packet → FSM → 기록·제어
가상 CAN → HAL RX → sp_can_decode ────┘
```

HAL은 **resmgr의 동일한 직렬 실행 문맥에서 비차단 방식으로 처리**하는 것을 우선합니다. 별도 수신 스레드가 린터·FSM·링버퍼를 직접 수정하는 구조는 피합니다. 한 번에 처리할 프레임 수도 제한해 watchdog과 IPC 처리가 밀리지 않게 합니다.

**우선 구현할 세부 항목**

- CRC 계산 전에 `payload_len <= 64`, DLC와 길이의 일치, 채널·CAN ID·플래그 유효성을 검사합니다.
- CAN ID와 센서 종류의 매핑을 고정 테이블로 정의합니다.
- `ACTIVE → ERROR_PASSIVE → BUS_OFF → RECOVERING → ACTIVE` 상태를 명시적으로 관리합니다.
- 복구 함수에서 즉시 정상 상태로 초기화하지 않고 `hal_can_service()`가 모델 시간과 상태에 따라 진행하도록 합니다.
- 통계에 `rx_overflow`, `tx_rejected`, `bus_off_count`, `recovery_count`, `state_entered_ns`를 추가합니다.
- 보호된 devctl로 결함 주입·상태 조회를 연결합니다. 정상 데이터 입력 권한과 결함 주입 권한을 구분합니다.

**통과 기준**

호스트 테스트가 요청 ID와 게스트 상태를 대조해 다음을 확인해야 합니다.

- BUS_OFF에서 송신이 거부된다.
- 해당 채널의 수신 중단을 안전 감시가 감지한다.
- 복구 조건 충족 후에만 ACTIVE로 전환한다.
- HAL 통계와 수신 시퀀스가 기대값과 일치한다.
- 복구 후에도 E-STOP 래치는 Reset 조건이 충족될 때까지 유지된다.

**심사 설득력:** HAL, QNX 리소스 매니저, 안전 로직이 하나의 실행 경로로 연결됩니다. 실제 장비 연결 전까지는 **가상 CAN 통합 시험**으로 명확하게 제시하는 편이 근거가 강합니다.

### P0-3. 프로세스 종료 감시를 실행 정지·교착 감시로 확장

**현재 확인한 구현**

`guest/safepulse_watcher.c`는 `waitpid(..., 0)`으로 자식 종료를 기다립니다. 이 방식만으로는 프로세스가 살아 있는 상태의 무한 루프·교착·`SIGSTOP`을 감지하지 못합니다.

**수정·추가 파일**

- 수정: `guest/safepulse_watcher.c`
- 수정: `guest/resmgr/safepulse_resmgr.c`
- 추가: `guest/include/safepulse_health.h`
- 수정: `guest/safepulse_bridge.c`
- 수정: 기존 QNX 이미지 구성·보안 정책 파일
- 수정: `host/test_chaos_marathon.py`

**제안 구조**

```c
typedef struct {
    _Atomic uint64_t dispatch_progress;
    _Atomic uint64_t control_progress;
    _Atomic uint64_t last_progress_ns;
    _Atomic uint32_t generation;
    _Atomic uint32_t fault_flags;
} sp_health_page_t;
```

- 초기화 시 고정 크기 공유메모리를 준비합니다.
- resmgr는 실제 dispatch·제어 처리가 끝났을 때 progress를 갱신합니다.
- watcher는 주기적으로 progress와 자식 종료를 함께 검사합니다.
- 데이터가 없는 정상 상태에서도 제어 틱의 진행 여부를 확인합니다.
- heartbeat 전용 스레드가 무조건 값을 올리는 구조는 사용하지 않습니다. 핵심 루프 정지를 숨길 수 있기 때문입니다.
- watcher 자체가 멈추더라도 P0-1의 명령 유효기간 감시가 제동을 유지하도록 합니다.
- 재시작 후에는 새 generation의 진행과 장치 응답을 확인합니다. `open()` 성공만으로 복구 완료를 판정하지 않습니다.

주기·타임아웃은 구성값으로 두고, 초기 시험값을 설정한 뒤 QEMU 지연 실측으로 조정합니다. 센서 침묵 한계, 실행 정지 한계, 재시작 완료 한계를 각각 관리해야 합니다.

**통과 기준**

SIGSEGV 외에 `SIGSTOP`, 핵심 루프 정지, 반복 재시작을 주입합니다. 각 경우에 **안전 명령 만료에 따른 제동**, **장애 감지**, **재시작**, **안전 상태 보존**을 별도로 검증합니다.

**심사 설득력:** “죽으면 다시 띄운다”에서 **살아 있지만 작동하지 않는 장애까지 다룬다**로 확장됩니다.

## 2. P1 — High-Impact Upgrades

### P1-1. 복구 마라톤에 패킷 보존·손실 회계 추가

현재 `host/test_chaos_marathon.py`는 epoch 변경과 텔레메트리 재개를 확인하며, 손실률은 측정하지 않는다고 명시합니다. `ringbuffer_push()`는 버퍼가 가득 차면 실패하지만, resmgr 호출부는 반환값을 처리하지 않습니다.

**수정 파일**

- `guest/include/ringbuffer.h`
- `guest/resmgr/ringbuffer.c`
- `guest/resmgr/safepulse_resmgr.c`
- `guest/safepulse_bridge.c`
- `host/test_chaos_marathon.py`

**구현 설계**

- 송신 측 `(producer_id, sequence)`와 게스트 수락 시점을 추적합니다.
- `ringbuffer_push()` 실패를 통계·응답에 반영합니다.
- `produced`, `accepted`, `committed`, `delivered`, `dropped`, `duplicates`를 분리합니다.
- 테스트가 송신한 프레임과 수신한 프레임을 정확히 대조합니다.
- push 직전·commit 직후·응답 직전의 장애를 구분해 주입합니다.
- 필요하면 호스트 송신자가 미확인 프레임을 재전송하고, 게스트가 중복을 제거합니다.

**중요한 계약:** 공유메모리에 commit된 프레임의 보존과, 종료 시점에 아직 처리되지 않은 입력의 전달 보장은 별도입니다. 후자까지 보장하려면 ACK·재전송·중복 제거가 필요합니다.

**통과 기준:** 버퍼 용량 이내 시험에서는 commit된 프레임의 누락·변조가 없어야 합니다. 용량 초과 시험에서는 손실이 정확하게 계수돼야 하며, 무손실 PASS로 처리하면 안 됩니다.

### P1-2. 복제 논리 테스트를 운영 함수의 결정·경계 테스트로 교체

`guest/test_mcdc_matrix.c`에는 `eval_decision_*()`로 별도 작성한 논리식이 있습니다. 이를 운영 함수 직접 호출 중심으로 바꾸는 것이 우선입니다.

**수정 파일**

- `guest/test_mcdc_matrix.c`
- `guest/test_linter.c`
- `guest/test_autosar_e2e.c`
- `guest/Makefile`
- `guest/build.bat`

**기술 사양**

- `linter_inspect_packet()`, `safe_state_fsm_step()`, `actuator_slew_clamp_*()`를 직접 호출합니다.
- watchdog의 임계값 직전·정확한 임계값·직후를 검사합니다.
- 첫 패킷 미도착, 한 ECU만 미도착, 처음부터 한 ECU가 없는 상태를 포함합니다.
- NaN/Inf, timestamp 역행·미래값, counter wrap, 잘못된 CAN 길이, 명령·피드백 만료를 포함합니다.
- FSM 유지 시간과 제어 유효기간에는 수신 측 `now_ns`를 사용하고, 송신 timestamp는 데이터 검증 대상으로 분리합니다.
- 지원되는 호스트 커버리지 도구로 운영 소스의 커버리지를 수집하고, QNX 빌드에서는 동일 테스트 벡터의 동작을 확인합니다.

**통과 기준:** 각 안전 요구사항이 실제 운영 함수의 테스트와 연결돼야 합니다. MC/DC 수치는 도구와 독립 조건 쌍으로 입증한 범위에 대해서만 산출합니다.

### P1-3. 전체 안전 반응 시간의 단계별 계측

**수정·추가 파일**

- 추가: `guest/include/safepulse_trace.h`
- 수정: `guest/resmgr/safepulse_resmgr.c`
- 수정: `guest/safepulse_bridge.c`
- 수정: `host/hil/vehicle_plant.py`
- 수정: `host/bridge/qnx_bridge.py`
- 수정: `host/test_comprehensive_e2e.py`

**제안 API**

```c
void sp_trace_emit(
    uint32_t event,
    uint64_t correlation_id,
    uint64_t now_ns,
    uint32_t detail);
```

고정 크기 이벤트 버퍼에 다음을 기록합니다.

`RX_ACCEPT → FAULT_DETECTED → FSM_CHANGED → COMMAND_PUBLISHED → COMMAND_APPLIED → STOP_CONFIRMED`

게스트 내부 구간과 호스트 모델 내부 구간은 각자의 시계로 측정합니다. 두 시계의 timestamp를 단순 차감해 전체 지연으로 보고하지 않습니다. 초기에는 호스트에서 주입부터 적용 응답까지 측정한 왕복 경과시간과 게스트 내부 처리 시간을 함께 제시하면 됩니다.

**통과 기준:** 최대값·p99·누락 이벤트 수를 계산하고, 결함별 감지 기한과 제어 기한을 따로 검사합니다. 기존 `FTTI_THRESHOLD_NS` 상수는 해당 경로의 실제 검사 조건에 연결해야 합니다.

## 3. P2 — Polish & Integration

| 통합 항목 | 파일 | 구현할 내용 |
|---|---|---|
| 심사 시연 자동화 | `host/dashboard/app.js`, `host/dashboard/index.html` | 기존 guided run에 폐루프 정지·HAL bus-off·실행 정지 시나리오 연결 |
| 명령과 피드백 비교 | 동일 대시보드 파일 | 요청 스로틀/브레이크와 실제 속도·압력을 함께 표시 |
| 장애 타임라인 | 대시보드·호스트 브리지 | 감지, 안전 명령, 적용, 정지, 재시작을 같은 correlation ID로 표시 |
| 실행 결과 내보내기 | `host/test_comprehensive_e2e.py`, 호스트 브리지 | commit·바이너리 해시·설정·검증 결과·원시 로그 위치를 기계 판독 형식으로 출력 |
| SDK 사용 예제 | `include/safepulse_api.h`, `guest/safepulse_api.c`, `examples/minimal_c/main.c` | 상태 조회, 명령·피드백 읽기, 오류 반환을 사용하는 작은 클라이언트 제공 |
| 재현 가능한 실행 | `run_safepulse.py`, `docs/codelab.md` | 실행 환경·이미지·보안 모드 확인과 시연 초기화 절차 연결 |

기존 `_reports`의 업무별 기록을 재사용하고, 실행별 근거는 기존 증거 보관 구조에 누적하는 방식이 적합합니다.

최종 시연의 핵심 장면은 다음처럼 구성하면 좋습니다.

**주행 → 센서 오류 → QNX 안전 명령 → 독립 모델의 제동 → resmgr 정지 → 명령 만료 중에도 제동 유지 → watcher 복구 → 안전 래치 유지 → 정지 확인 후 Reset**

이 흐름 하나가 시각화, QNX IPC, 프로세스 격리, 장애 복구, 안전 제어를 연결합니다.

## 4. Step-by-Step Implementation Roadmap

| 순서 | 실제 작업 | 완료 조건 |
|---|---|---|
| 1 | 현재 변경되지 않은 기준선의 빌드·단위·QEMU 결과 확보 | commit·이미지·실행 결과가 연결됨 |
| 2 | `sp_ingest_packet()` 추출, 수신 시계와 송신 시계 분리, ring push 실패 처리 | 기존 ECU 경로 회귀 테스트 통과 |
| 3 | `safepulse_control.h`와 명령·피드백 API 구현 | 측정값을 보존하며 명령을 별도로 조회 가능 |
| 4 | 주기적 제어 틱과 `vehicle_plant.py` 연결 | 정상 가속·제동·정지 폐루프 통과 |
| 5 | 명령 만료·피드백 신선도·Reset 인터록 구현 | 입력·통신 단절 시에도 제동, 오래된 피드백으로 Reset 불가 |
| 6 | HAL 입력 검증·상태 머신·공통 수신 경로 연결 | bus-off·복구·overflow를 게스트 관측으로 판정 |
| 7 | watcher progress 감시와 generation 확인 구현 | crash·정지·핵심 루프 정지 시험 통과 |
| 8 | 50-crash 테스트에 시퀀스·commit·손실 회계 추가 | 복구율과 보존 결과가 독립적으로 산출됨 |
| 9 | 운영 함수 테스트·trace 계측 연결 | 결함별 감지·명령·적용 기한을 자동 판정 |
| 10 | 기존 guided run과 결과 내보내기 통합 | 깨끗한 시작 상태에서 전체 시연을 재현 가능 |

**첫 구현 단위는 2번의 `sp_ingest_packet()` 추출과 시간 도메인 분리로 잡는 것을 권합니다.** 변경 범위를 작게 유지하면서 이후 폐루프와 HAL이 같은 안전 경로를 사용하게 만드는 기반입니다. 각 단위를 마칠 때 빌드·회귀 결과를 확인하고 다음 작업으로 넘어가는 순서가 적절합니다.

최종 완료 판정은 프로젝트 기준인 **qcc 경고 0개, 호스트·게스트 단위 테스트, 실제 QEMU 결합 시험, 50회 장애 복구 시험**에 위의 폐루프·실행 정지·손실 회계 검증을 더해 내려야 합니다.

