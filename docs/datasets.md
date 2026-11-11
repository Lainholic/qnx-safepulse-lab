# Real Vehicle Datasets for SafePulse Validation

이 문서는 SafePulse 임베디드 안전 모니터링 시스템의 실차 데이터 검증 및 리플레이를 위해 채택된 공개 주행/CAN 데이터셋의 명세, 출처, 라이선스를 정리합니다.

---

## 1. Primary Dataset: comma2k19 (comma.ai)

- **데이터셋 이름**: `comma2k19` (Highway 280 Real-Vehicle Driving & CAN Dataset)
- **제공자 / 저작권자**: comma.ai Inc.
- **출처 (URL)**:
  - Repository: https://github.com/commaai/comma2k19
  - Paper: [comma2k19: a dataset of highway driving (arXiv:1812.05752)](https://arxiv.org/abs/1812.05752)
  - Processed CAN Segment: `Example_1/b0c9d2329ad1606b|2018-08-02--08-34-47/40/processed_log/CAN/`
- **라이선스**: **MIT License** (상업적 이용, 재배포, 연구, 수정 100% 허용)
- **데이터 특성**:
  - 실제 주행 차량: Toyota RAV4 2017
  - 주행 환경: 미국 캘리포니아 I-280 고속도로 실차 주행
  - CAN 신호 규격:
    - `steering_angle` (조향각, deg / 100Hz 주기): 4,974 샘플
    - `speed` / `wheel_speed` (차량 속도, m/s / 100Hz 주기): 4,974 샘플
    - `raw_can` (원시 CAN 트래픽): 135,468 프레임, 165개 CAN ID
  - SafePulse 신호 매핑 및 생성 원칙:
    - **실제 센서 측정 데이터**: 조향각(`steering_angle`)과 차량 속도(`speed`)는 Toyota RAV4 실차 CAN 버스에서 직접 측정한 실제 데이터입니다. (실제 데이터는 조향·속도이고, 브레이크·가속은 속도에서 계산해 만든 값)
    - **유도/계산 합성 데이터**: 브레이크 압력(`SENSOR_TYPE_BRAKE`) 및 가속 페달(`SENSOR_TYPE_THROTTLE`)은 실제 속도 신호의 미분값(감속도)과 속도 환산식을 통해 속도에서 계산해 만든 유도값입니다.
    - `steering_angle` -> `SENSOR_TYPE_STEERING` (ID: 0x01, 범위: -45.0 deg ~ +45.0 deg, 실제 측정값)
    - `speed` (m/s -> km/h 변환) -> `SENSOR_TYPE_THROTTLE` / 파워트레인 속도 (ID: 0x03, 범위: 0 ~ 200 km/h, 속도 기반 계산값)
    - 감속도(급제동) 도출 -> `SENSOR_TYPE_BRAKE` (ID: 0x02, 범위: 0 ~ 100%, 속도 미분 기반 계산값)

---

## 2. Secondary Dataset: OpenXC Vehicle Traces (Ford Motor Company)

- **데이터셋 이름**: `OpenXC Vehicle Trace Data`
- **제공자 / 저작권자**: Ford Motor Company
- **출처 (URL)**:
  - Repository: https://github.com/openxc/openxc-python
  - Specification: https://github.com/openxc/openxc-message-format
  - Sample Trace: `tests/trace.json`
- **라이선스**: **BSD 3-Clause License** (재배포 및 연구 사용 허용)
- **데이터 특성**:
  - 포맷: Newline-delimited JSON (NDJSON)
  - 신호 규격:
    - `steering_wheel_angle`
    - `vehicle_speed`
    - `accelerator_pedal_position`
    - `torque_at_transmission`
    - `engine_speed`

---

## 3. 원본 데이터 관리 정책

1. **Git 저장 금지**: 대용량 바이너리 및 원본 주행 로그(`.npy`, `.json`, `.bz2`, `.hevc`)는 Git 저장소에 일절 커밋하지 않으며, `.gitignore`의 `_work/datasets/` 규칙으로 제외됩니다.
2. **자동 다운로드**: 개발자는 `tools/fetch_dataset.ps1` 또는 `tools/fetch_dataset.sh` 스크립트를 통해 언제든지 원본을 재현 다운로드할 수 있습니다.
3. **결정론적 변환**: 수집된 데이터는 `host/replay/can_replay.py`를 통해 `/dev/safepulse` 64바이트 AUTOSAR E2E 패킷 형식으로 동기화되어 QNX 마이크로커널로 안전하게 주입됩니다.
