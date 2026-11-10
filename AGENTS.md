# AGENTS.md — QNX SafePulse Lab 프로젝트 지침

이 문서는 BlackBerry QNX Global Hackathon 2026 공식 출품작인 **QNX SafePulse Lab (실시간 텔레메트리 린터 & 자가치유 스위트)**의 개발 및 검증을 수행하는 모든 AI 에이전트가 반드시 준수해야 하는 공학적 원칙과 규칙을 정의합니다.

환님의 명시적 지시를 최우선으로 따르며, 상위 지침인 `C:\AI\AGENTS.md` 및 `C:\AI\운영지침\공통절차.md`를 엄격히 계승합니다.

---

## 1. 절대 금지 및 철칙 사항 (Hard Rules)

1. **취소선(~~...~~, `<strike>`, `<s>`, `<del>`) 절대 사용 금지**:
   - 코드, 커밋 메시지, 주석, 마크다운 문서, 환님 보고 등 모든 텍스트에서 취소선을 절대 사용하지 않는다.
2. **에이전트 비동기 백그라운드 태스크(Task) 유발 엄단**:
   - 명령어 실행 시 충분한 타임아웃(`WaitMsBeforeAsync: 10000`)을 부여하여 동기적으로 즉시 완료한다.
3. **사용자 호칭**:
   - 환님을 항상 **‘환님’**으로 정중히 부른다.
4. **조급한 완성 단정 및 섣부른 마무리 엄단**:
   - 뼈대 코드나 더미 데이터 단위 테스트 몇 개 통과한 것을 두고 '완성', '완벽'으로 포장하지 않는다.
   - 실제 QEMU QNX 8.0 게스트 및 호스트 연동 실측 데이터가 입증될 때까지 냉정하게 "개발/검증 단계"로 명시한다.
5. **무단 외부 Git Push 및 원격 전송 금지**:
   - 로컬 Git(상태, 커밋, 브랜치 등)은 자유롭게 활용하되, 원격 Push는 환님이 "푸시해"라고 명시한 그 시점 단 1회에만 수행한다.

---

## 2. 공학적 아키텍처 및 철칙 (Engineering Principles)

### [게스트 영역: QNX Neutrino 8.0 (QEMU Target)]
- **언어 및 런타임**: 순수 C11 / POSIX + QNX Native API (`sys/neutrino.h`, `sys/iofunc.h`, `sys/dispatch.h`).
- **Zero-Dependency 철칙**:
  - QNX 게스트 내부에는 외부 라이브러리, Python, Node.js, 고용량 프레임워크를 일절 올리지 않는다.
  - 바이너리 합산 크기는 5MB 미만으로 엄격히 제한한다.
- **결정론적 실시간성 (Deterministic Real-time)**:
  - 동적 메모리 할당(`malloc`/`free`)은 초기 부팅 시 1회만 고정 크기로 수행하며, 런타임 루프 내 빈번한 할당을 금지한다.
  - 무복사(Zero-Copy) 원형 링 버퍼(Ring Buffer, 2MB 고정)를 기반으로 `/dev/safepulse` 리소스 매니저를 구현한다.
- **마이크로커널 고가용성 (`posix_spawn` Watcher 연동)**:
  - `posix_spawn` 기반 자체 감시 프로그램(`safepulse_watcher.c`)과 결속하여, 리소스 매니저 비정상 종료(SIGSEGV, SIGFPE, SIGABRT, SIGILL) 발생 시 무중단 복구 및 POSIX 공유메모리(`/safepulse_ringbuf`) 상태 재결합을 증명한다.

### [호스트 영역: Windows / Linux Host]
- **언어 및 스택**: Python 3.10+, HTML5 / Canvas / Three.js (`host/dashboard/index.html`, `host/dashboard/app.js`).
- **역할 분리**:
  - 3D 시각화, 시계열 파형 차트, 6초 블랙박스 리플레이 등 시각화는 호스트에서 처리한다.
  - QNX 게스트와는 TCP 소켓(VirtIO Port 9876)으로 64바이트 바이너리 패킷 및 16바이트 결함 주입 명령만 교환한다.

---

## 3. 디렉토리 구조 및 역할

```text
qnx-safepulse-lab/
├── AGENTS.md                  # 본 에이전트 작업 지침
├── README.md                  # 프로젝트 아키텍처 및 빌드·실행 안내
├── SUBMISSION.md              # 해커톤 제출 기술 문서
├── specs/                     # 시스템 스펙 및 바이너리 패킷 프로토콜 정의
│   ├── protocol_v1.md         # QNX <-> Host 통신 패킷 스펙 (64바이트 헤더)
│   ├── safety_rules.md        # 실시간 린터 안전 규칙 및 FSM 정의
│   └── test_protocol_and_logic.py # 호스트 프로토콜 단위 검증
├── guest/                     # QNX 8.0 Neutrino C-Native 타깃 코드
│   ├── include/               # 공통 헤더 (safepulse.h, ringbuffer.h, linter.h, safe_state_fsm.h 등)
│   ├── resmgr/                # /dev/safepulse QNX 리소스 매니저, 링버퍼, FSM
│   ├── linter/                # 실시간 안전 검증 린터 엔진 및 AUTOSAR E2E 라이브러리
│   ├── ecu/                   # 가상 ECU 3종 (ecu_steering, ecu_braking, ecu_powertrain)
│   ├── safepulse_watcher.c    # posix_spawn 기반 자가치유 감시 데몬
│   ├── safepulse_bridge.c     # QNX <-> Host TCP 9876 브리지 및 결함 주입 핸들러
│   ├── Makefile               # QNX qcc 크로스 컴파일러 빌드 스크립트 (build.bat / build.sh 병행)
│   └── qemu_vm/               # mkqnximage 설정 및 QEMU 실행 스크립트
├── host/                      # 호스트 시스템
│   ├── bridge/qnx_bridge.py   # QEMU TCP 소켓 수신 및 HTTP/REST 브리지 서버
│   ├── dashboard/             # Web UI 대시보드 (index.html, app.js)
│   ├── test_comprehensive_e2e.py # 8-Gate 자율 E2E 검증 하네스
│   └── test_chaos_marathon.py # 50-Crash 4-Domain 연속 복구 마라톤 하네스
└── _reports/                  # 실측 벤치마크 및 테스트 검증 리포트
```

---

## 4. 검증 기준 (Definition of Done)

어떠한 기능도 "추측"으로 완료 보고를 하지 않는다. 다음 4단계 검증 증거가 확보되어야 완료로 인정한다:

1. **빌드 검증**: QNX SDK `qcc` 컴파일러 경고(Warning) 0개 통과 (`guest/Makefile` 또는 `guest/build.bat`).
2. **단위 테스트**: 호스트 프로토콜 테스트(`specs/test_protocol_and_logic.py`) 및 게스트 단위 테스트 통과.
3. **결합 테스트**: QEMU 가상머신 내 구동 및 `host/test_comprehensive_e2e.py` 8/8 게이트 통과.
4. **장애 복구 검증**: `host/test_chaos_marathon.py` 50회 연속 크래시 복구 및 공유메모리 무손실 검증.
