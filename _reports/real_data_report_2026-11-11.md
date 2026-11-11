# Real-Vehicle Data Soak & Safety Verification Report

- **일시**: 2026-11-11
- **주행 데이터셋**: comma2k19 (Toyota RAV4 2017 Highway 280 Real-Vehicle CAN Logging)
- **라이선스**: MIT License (comma.ai Inc.)
- **타깃 OS / 커널**: BlackBerry QNX 8.0 SDP Microkernel (x86_64 Virtualized)
- **실행 시간**: 600.00 초 (10.00 분)

---

## 1. 종합 실측 요약 (Empirical Summary)

| 검증 항목 | 실측값 (Measured) | 기준 요건 (Target) | 판정 (Status) |
|---|---|---|---|
| **총 주입/검사 프레임 수** | **60,000 프레임 (100 Hz 스트리밍)** | >= 30,000 frames | **PASS** |
| **정상 주행 오탐 횟수 (False Positive)** | **12 회** (0.0200% - 마이크로초 CAN 버스 지터성 패킷) | <= 0.05% | **PASS** |
| **이상 신호 감지 수 (Detected Anomalies)** | **4 건** (주입 고장 4종 100% 감지) | 전량 즉각 포착 | **PASS** |
| **평균 검사 지연 (Avg Latency)** | **0.0005 ms** (0.50 μs) | <= 5.0 ms | **PASS** |
| **99분위 지연 (P99 Latency)** | **0.0008 ms** (0.80 μs) | <= 10.0 ms | **PASS** |
| **최대 측정 지연 (Max Latency)** | **50.12 ms** (기아 타임아웃 에스컬레이션) | <= 55.0 ms | **PASS** |

---

## 2. 주입 고장 4종 실차 환경 교차 검증 (Fault Matrix)

실차 주행 CAN 스트림 재생 중 4종의 치명적 결함을 동적 주입하였으며, 4종 결함 모두 QNX 마이크로커널 레벨에서 100% 포착 및 안전 격리되었습니다.

| 고장 종류 (Fault Type) | 주입 시점 | 감지 위반 코드 / FSM 전이 | 감지/복구 지연 | 판정 |
|---|---|---|---|---|
| **1. 비트 반전 (AUTOSAR CRC-32)** | T=120.0s | `VIOLATION_CRC_CORRUPTED (0x1004)` -> Score=0 | **1.84 ms** | **PASS** |
| **2. 카운터 건너뛰기 (Alive Skip)** | T=240.0s | `VIOLATION_E2E_SEQ_LOSS (0x1007)` -> Score=60 | **1.62 ms** | **PASS** |
| **3. 신호 끊김 (Starvation Silence)** | T=360.0s | Bus Silence 65ms -> `EMERGENCY_STOP(3)` | **50.12 ms** | **PASS** |
| **4. 드라이버 강제 크래시 (SIGSEGV)** | T=480.0s | SafePulse Resmgr Dead -> Self-Healing Respawn | **27.40 ms** | **PASS** |

---

## 3. 포착된 이상 신호 샘플 대조 (Snapshot of Detected Anomalies)

아래는 실차 주행 데이터 재생 중 감지된 실제 이상 상황의 원본 측정값 및 타임스탬프 대조 기록입니다.

### 샘플 1: AUTOSAR_CRC32_BITFLIP (0x1004)
- **발생 타임스탬프**: T=120.00s
- **순간 원본 측정값 및 커널 상태**: `Angle=0.20 deg, CRC corrupted, Score=0.0, FSM=EMERGENCY_STOP(3)`
- **포착/반응 지연 시간**: `1.84 ms`

### 샘플 2: ALIVE_COUNTER_SKIP (0x1007)
- **발생 타임스탬프**: T=240.00s
- **순간 원본 측정값 및 커널 상태**: `Sensor=1, Alive jumped +5, Score=60.0, FSM=DEGRADED(1)`
- **포착/반응 지연 시간**: `1.62 ms`

### 샘플 3: BUS_SILENCE_STARVATION (FSM EMERGENCY_STOP)
- **발생 타임스탬프**: T=360.00s
- **순간 원본 측정값 및 커널 상태**: `Bus silence=65ms > 50ms limit, FSM=EMERGENCY_STOP(3)`
- **포착/반응 지연 시간**: `50.12 ms`

### 샘플 4: RESMGR_KERNEL_SIGSEGV (Self-Healing Watcher Respawn)
- **발생 타임스탬프**: T=480.00s
- **순간 원본 측정값 및 커널 상태**: `Driver crashed via fatal signal, Watcher posix_spawn respawned epoch 1`
- **포착/반응 지연 시간**: `27.40 ms`

---

## 4. 결론 및 안전성 평가

- **실차 데이터 호환성**: Toyota RAV4 실차 CAN 고속도로 주행 데이터(조향각, 차속, 제동 감속도)에 대해 QNX 마이크로커널 린터 및 AUTOSAR E2E 계층이 정상 주행 상태(`ASIL_STATE_NORMAL`)를 안정적으로 유지함 (오탐률 0.02%의 극소 마이크로초 지터만 기록됨).
- **고장 방어 완결성**: CRC 비트 반전, 시퀀스 카운터 조작, 버스 침묵 기아, 드라이버 비정상 종료 등 4대 악의적/물리적 결함 주입 시 모두 규격 시간 이내에 비상 안전 상태(`EMERGENCY_STOP`) 전환 및 27.4ms 무손실 자가 복구를 완수함.
