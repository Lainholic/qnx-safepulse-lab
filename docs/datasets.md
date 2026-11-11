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
  - SafePulse 신호 매핑:
    - `steering_angle` -> `SENSOR_TYPE_STEERING` (ID: 0x01, 범위: -45.0 deg ~ +45.0 deg)
    - `speed` (m/s -> km/h 변환) -> `SENSOR_TYPE_THROTTLE` / 파워트레인 속도 (ID: 0x03, 범위: 0 ~ 200 km/h)
    - 감속도(급제동) 도출 -> `SENSOR_TYPE_BRAKE` (ID: 0x02, 범위: 0 ~ 100%)

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
