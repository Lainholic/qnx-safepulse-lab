# Protocol Spec v1 (SafePulse Binary Telemetry Protocol)

QNX 게스트(`/dev/safepulse`)와 호스트 브리지(Node.js TCP) 간에 오가는 고속 바이너리 패킷 규격입니다.
네트워크 바이트 오더(Big-Endian)를 따르며, 패킷 크기는 고정 64바이트로 정렬(Aligned)하여 무복사 파싱을 보장합니다.

---

## 1. 패킷 구조 (64 Bytes Fixed Size)

| 오프셋 (Byte) | 필드명 | 데이터 타입 | 설명 |
| :--- | :--- | :--- | :--- |
| `0x00 - 0x03` | `magic` | `uint32_t` | 고정 매직 넘버 (`0x53504C53`, ASCII "SPLS") |
| `0x04 - 0x07` | `seq_num` | `uint32_t` | 순차 증가 패킷 시퀀스 번호 |
| `0x08 - 0x0F` | `timestamp_ns` | `uint64_t` | QNX `ClockCycles()` 기반 나노초 단위 타임스탬프 |
| `0x10 - 0x13` | `task_id` | `uint32_t` | 센서 송신 태스크 ID / PID |
| `0x14 - 0x17` | `sensor_type` | `uint16_t` | 센서 종류 (1: 조향, 2: 브레이크, 3: 스로틀, 4: IMU) |
| `0x16 - 0x17` | `status_flags` | `uint16_t` | 상태 플래그 (Bit 0: Warning, Bit 1: Critical, Bit 2: HAM Recovered) |
| `0x18 - 0x1F` | `val_primary` | `double` (64-bit) | 주 센서 계측값 (예: 조향각 deg, 속도 km/h) |
| `0x20 - 0x27` | `val_secondary` | `double` (64-bit) | 보조 계측값 (예: 가속도, 압력 bar) |
| `0x28 - 0x2B` | `safety_score` | `float` (32-bit) | 린터가 산출한 안전 점수 (0.0 ~ 100.0) |
| `0x2C - 0x2F` | `violation_code`| `uint32_t` | 감지된 규칙 위반 코드 (0: 정상, 0x1001: 한계초과 등) |
| `0x30 - 0x3B` | `reserved` | `uint8_t[12]` | 향후 확장을 위한 예약 필드 (0으로 채움) |
| `0x3C - 0x3F` | `crc32` | `uint32_t` | 패킷 전체 무결성 검증 CRC-32 체크섬 |

---

## 2. 결함 주입 명령 패킷 (Host -> QNX, 16 Bytes Fixed)

| 오프셋 (Byte) | 필드명 | 데이터 타입 | 설명 |
| :--- | :--- | :--- | :--- |
| `0x00 - 0x03` | `magic` | `uint32_t` | 고정 매직 넘버 (`0x4348414F`, ASCII "CHAO") |
| `0x04 - 0x07` | `fault_type` | `uint32_t` | 결함 종류 (1: 센서 급변, 2: 슬립 데드락, 3: 프로세스 SIGSEGV) |
| `0x08 - 0x0B` | `duration_ms` | `uint32_t` | 결함 지속 시간 또는 강도 |
| `0x0C - 0x0F` | `checksum` | `uint32_t` | 명령 패킷 체크섬 |
