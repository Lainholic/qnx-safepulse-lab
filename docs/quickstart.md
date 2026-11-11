# SafePulse Developer Quickstart Guide (10-Minute Walkthrough)

SafePulse는 BlackBerry QNX 마이크로커널 및 임베디드 리눅스/POSIX 환경에서 구동되는 **ISO 26262 ASIL-D 준수 실시간 안전 감시 및 Linter SDK**입니다. 이 가이드는 외부 개발자가 10분 안에 라이브러리를 빌드하고 예제를 실행하는 방법을 안내합니다.

---

## 1. 전제 조건 (Prerequisites)

- **QNX 환경**: BlackBerry QNX 8.0 SDP (`C:\qnx800\qnxsdp-env.bat` 또는 `source ~/qnx800/qnxsdp-env.sh`)
- **Python**: Python 3.9+ (Numpy 패키지 포함)
- **Make**: GNU Make

---

## 2. 라이브러리 빌드 (Building `libsafepulse.a`)

QNX 컴파일러 환경이 로드된 터미널에서 `guest/` 폴더로 이동하여 빌드합니다.

```bash
# 1. QNX 환경 변수 로드
call C:\qnx800\qnxsdp-env.bat   # Windows
# source ~/qnx800/qnxsdp-env.sh # Linux

# 2. guest 폴더에서 빌드
cd guest
make all
```

성공 시 프로젝트 루트 및 `guest/`에 정적 라이브러리 `libsafepulse.a`와 공개 헤더 `include/safepulse_api.h`가 준비됩니다.

---

## 3. C SDK 미니멀 예제 실행 (50줄 이하)

[`examples/minimal_c/main.c`](file:///C:/AI/projects/qnx-safepulse-lab/examples/minimal_c/main.c) 예제를 빌드하고 검사합니다.

```bash
cd examples/minimal_c
make
```

### 코드 미리보기 (`examples/minimal_c/main.c`)
```c
#include <stdio.h>
#include "safepulse_api.h"

int main(void) {
    safepulse_ctx_t *ctx = safepulse_init();
    if (!ctx) return 1;

    safepulse_packet_t pkt = {0};
    pkt.magic = SAFEPULSE_MAGIC;
    pkt.sensor_type = SAFEPULSE_SENSOR_STEERING;
    pkt.val_primary = 25.0; /* 25 degrees (Normal) */

    safepulse_verdict_t verdict;
    safepulse_inspect(ctx, &pkt, &verdict);

    printf("Safety Verdict: %s (Score: %.1f)\n",
           verdict.passed ? "PASS" : "FAIL", verdict.safety_score);

    safepulse_destroy(ctx);
    return 0;
}
```

---

## 4. 커스텀 차량 규칙 적용 (설정 파일 기반)

코드 수정 없이 [`rules.yaml`](file:///C:/AI/projects/qnx-safepulse-lab/rules.yaml) 또는 [`examples/custom_rules/truck_rules.yaml`](file:///C:/AI/projects/qnx-safepulse-lab/examples/custom_rules/truck_rules.yaml)만 변경하여 상용차, UAM, 전기차 신호 규격을 검사할 수 있습니다.

```bash
cd examples/custom_rules
make
```

---

## 5. 호스트 CLI 도구 (`safepulse-cli`) 사용법

호스트 개발자는 커널 없이도 명령줄에서 트레이스 정적 분석, 리플레이, 마크다운 리포트를 즉시 생성할 수 있습니다.

### 5-1. 주행 기록 검사 (`check`)
```bash
python host/tools/safepulse_cli.py check _work/datasets/openxc_sample_trace.json --rules rules.yaml
```

### 5-2. QNX 마이크로커널로 재생 (`replay`)
```bash
python host/tools/safepulse_cli.py replay _work/datasets/openxc_sample_trace.json --target 127.0.0.1:9876 --speed 2.0
```

### 5-3. 마크다운 리포트 생성 (`report`)
```bash
python host/tools/safepulse_cli.py report --out _reports/safepulse_cli_report.md
```
생성된 보고서: [`_reports/safepulse_cli_report.md`](file:///C:/AI/projects/qnx-safepulse-lab/_reports/safepulse_cli_report.md)
