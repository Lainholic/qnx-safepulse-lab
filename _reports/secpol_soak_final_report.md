# Real-Vehicle Data Soak & Safety Verification Report

- **일시**: 2026-11-11
- **주행 데이터셋**: comma2k19 (Toyota RAV4 2017 Highway 280 Real-Vehicle CAN Logging)
- **라이선스**: MIT License (comma.ai Inc.)
- **타깃 OS / 커널**: BlackBerry QNX 8.0 SDP Microkernel (x86_64 Virtualized)
- **실행 시간**: 602.01 초 (10.03 분)
- **데이터 성격 명시**: 실제 데이터는 조향·속도이고, 브레이크·가속은 속도에서 계산해 만든 값입니다. (Toyota RAV4 실차 CAN 조향각 및 차속 원본 측정값, 브레이크/가속도는 차속 미분 유도값)

---

## 1. 종합 실측 요약 (Empirical Summary)

| 검증 항목 | 실측값 (Measured) | 기준 요건 (Target) | 판정 (Status) |
|---|---|---|---|
| **총 주입/검사 프레임 수** | **70,954 TX / 70,954 RX** | >= 30,000 frames & TX == RX | **PASS** |
| **1:1 시퀀스 매칭 (TX == RX)** | **70,954 / 70,954 (일치)** | 100% 1:1 일치 | **PASS** |
| **정상 주행 오탐 횟수 (False Positive)** | **0 회** | 0 회 (Zero-FP) | **PASS** |
| **이상 신호 감지 수 (Detected Anomalies)** | **4 건** (주입 4종 전량) | 4/4 전량 즉각 포착 | **PASS** |
| **평균 검사 지연 (Avg Latency)** | **1.505 ms** | <= 5.0 ms | **PASS** |
| **99분위 지연 (P99 Latency)** | **3.470 ms** | <= 10.0 ms | **PASS** |
| **최대 측정 지연 (Max Latency)** | **18.825 ms** | <= 50.0 ms | **PASS** |

---

## 2. 주입 고장 4종 실차 환경 교차 검증 (Fault Matrix)

실차 주행 CAN 스트림 재생 중 4종의 치명적 결함을 동적 주입하였으며, 호스트 독립 단일 시계(Host Monotonic Clock)로 반응 지연을 계측하였습니다.

| 고장 종류 (Fault Type) | 주입 시점 | 감지 위반 코드 / 커널 반응 | 감지/복구 지연 (Host Clock) | 판정 |
|---|---|---|---|---|
| **1. 비트 반전 (AUTOSAR CRC-32)** | T=120s | `VIOLATION_CRC_CORRUPTED (0x1004)` -> Score=0 | **15.56 ms** | **PASS** |
| **2. 카운터 건너뛰기 (Alive Skip)** | T=240s | `VIOLATION_E2E_SEQ_LOSS (0x1007)` -> Score=60 | **2.87 ms** | **PASS** |
| **3. 신호 끊김 (Starvation Silence)** | T=360s | Bus Silence -> `EMERGENCY_STOP(3)` | **74.08 ms** | **PASS** |
| **4. 드라이버 강제 크래시 (SIGSEGV)** | T=480s | Watcher Respawn -> 새 프로세스 첫 패킷 수신 | **56.66 ms** | **PASS** |

---

## 3. 포착된 이상 신호 샘플 대조 (Snapshot of Detected Anomalies)

아래는 실차 주행 데이터 재생 중 감지된 실제 이상 상황의 원본 측정값 및 타임스탬프 대조 기록입니다.

### 샘플 1: AUTOSAR_CRC32_BITFLIP (0x1004)
- **발생 타임스탬프**: T=48431.12s
- **순간 원본 측정값 및 커널 상태**: `Sensor=1, Angle=-0.00 deg, Score=0.0, FSM=3`
- **포착/반응 지연 시간**: `15.56 ms`

### 샘플 2: ALIVE_COUNTER_SKIP (0x1007)
- **발생 타임스탬프**: T=48551.11s
- **순간 원본 측정값 및 커널 상태**: `Sensor=3, Score=60.0, FSM=1`
- **포착/반응 지연 시간**: `2.87 ms`

### 샘플 3: BUS_SILENCE_STARVATION (FSM EMERGENCY_STOP)
- **발생 타임스탬프**: T=48671.18s
- **순간 원본 측정값 및 커널 상태**: `Bus silence, Kernel FSM=3`
- **포착/반응 지연 시간**: `74.08 ms`

### 샘플 4: RESMGR_KERNEL_SIGSEGV (Self-Healing Watcher Respawn)
- **발생 타임스탬프**: T=48791.16s
- **순간 원본 측정값 및 커널 상태**: `New process respawned, Epoch 0 -> 1, RxSeq=56776`
- **포착/반응 지연 시간**: `56.66 ms`

---

## 4. 결론 및 종합 판정

- **종합 결과**: **ALL GATES PASS (100%)**
- **정상 주행 신호 적합성**: `dt < 1ms` 중복/지터 프레임 필터 적용 후, 60,000 프레임 전체 구간에서 오탐 횟수 **0회** 달성.
- **실차 환경 자가 복구 검증**: SIGSEGV 크래시 후 이전 프로세스의 패킷이 아닌 새로 생성된 프로세스(epoch 증가)의 첫 패킷이 **56.66 ms** 만에 수신되어 자가 복구 무손실 증명 완료.

### 결과 정정
위 결론의 60,000 프레임은 고정 문구로 실제 TX는 70,954건입니다. 복구 epoch 수신은 확인했으나 공유 메모리 무손실은 이 시험에서 직접 측정하지 않았습니다.
