# AGENTS.md — QNX SafePulse Lab 프로젝트 지침

이 문서는 BlackBerry QNX Global Hackathon 2026 공식 출품작인 **QNX SafePulse Lab (실시간 텔레메트리 린터 & 자가치유 스위트)**의 개발 및 검증을 수행하는 모든 AI 에이전트(애니, 테라, 딥시크, 서브에이전트 등)가 반드시 준수해야 하는 공학적 원칙과 규칙을 정의합니다.

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
- **언어 및 런타임**: 순수 C99 / POSIX + QNX Native API (`sys/neutrino.h`, `sys/iofunc.h`, `sys/dispatch.h`).
- **Zero-Dependency 철칙**:
  - QNX 게스트 내부에는 외부 라이브러리, Python, Node.js, 고용량 프레임워크를 일절 올리지 않는다.
  - 바이너리 합산 크기는 5MB 미만으로 엄격히 제한한다.
- **결정론적 실시간성 (Deterministic Real-time)**:
  - 동적 메모리 할당(`malloc`/`free`)은 초기 부팅 시 1회만 고정 크기로 수행하며, 런타임 루프 내 빈번한 할당을 금지한다.
  - 무복사(Zero-Copy) 원형 링 버퍼(Ring Buffer, 2MB 고정)를 기반으로 `/dev/safepulse` 리소스 매니저를 구현한다.
- **마이크로커널 고가용성 (HAM 연동)**:
  - QNX의 핵심 기능인 HAM (High Availability Manager)과 결속하여, 린터 프로세스 비정상 종료(SIGSEGV 등) 발생 시 1ms 내 무중단 복구 및 상태 재결합을 증명한다.

### [호스트 영역: Windows Host (개발 PC)]
- **언어 및 스택**: Node.js / React / TypeScript / Vite / Three.js (또는 경량 웹 UI).
- **역할 분리**:
  - 화려한 3D 시각화, 장기 시계열 센서 차트, 통계 분석, LLM 원인 분석 브리핑 등 무거운 작업은 호스트 PC에서 100% 처리한다.
  - QNX 게스트와는 TCP 소켓 또는 가상 직렬(VirtIO/Serial) 채널로 컴팩트 바이너리 패킷만 교환한다.
- **결함 주입기 (Fault Injector)**:
  - 심사위원이 웹 UI에서 버튼 하나로 센서 오작동(Out of Bound), 슬립 유발(Watchdog Timeout), 프로세스 강제 킬(Crash)을 주입하고, QNX가 이를 실시간으로 방어/복구하는 모습을 직관적으로 볼 수 있게 설계한다.

---

## 3. 디렉토리 구조 및 역할

```text
C:\AI\projects\qnx-safepulse-lab\
├── AGENTS.md                  # 본 에이전트 작업 지침
├── README.md                  # 프로젝트 전체 청사진 및 대회 제출 규격
├── specs/                     # 시스템 스펙 및 바이너리 패킷 프로토콜 정의
│   ├── protocol_v1.md         # QNX <-> Host 통신 패킷 스펙 (바이너리 헤더)
│   └── safety_rules.md        # 실시간 린터 안전 규칙 정의 (MISRA/ISO 26262 기반)
├── guest/                     # QNX 8.0 Neutrino C-Native 타깃 코드
│   ├── include/               # 공통 헤더 (safepulse.h, ringbuffer.h 등)
│   ├── resmgr/                # /dev/safepulse QNX 리소스 매니저
│   ├── linter/                # 바이트코드 안전 검증 린터 엔진
│   ├── ham/                   # HAM 자가치유 모니터링 클라이언트
│   └── Makefile               # QNX qcc 크로스 컴파일러 빌드 스크립트
├── host/                      # Windows 호스트 시스템
│   ├── bridge/                # QEMU TCP 소켓 수신 및 브로드캐스트 브리지
│   ├── dashboard/             # Web UI 대시보드 (차량 모델 + 실시간 게이지)
│   └── fault_injector/        # 결함 주입 시뮬레이터 (CLI/API)
├── docs/                      # 데모 시나리오, Devpost 제출 초안, 다이어그램
├── _reports/                  # 실측 벤치마크 및 테스트 검증 리포트
└── _work/tmp/                 # 작업 중 임시 파일 (작업 완료 시 자가 청소)
```

---

## 4. 검증 기준 (Definition of Done)

어떠한 기능도 "추측"으로 완료 보고를 하지 않는다. 다음 4단계 검증 증거가 확보되어야 완료로 인정한다:

1. **빌드 검증**: QNX SDK `qcc` 컴파일러 경고(Warning) 0개 통과.
2. **단위 테스트**: 리소스 매니저 링버퍼 오버플로우/경계조건 통과 로그.
3. **결합 테스트**: QEMU 가상머신 내 백그라운드 구동 및 Windows 브리지와의 패킷 정합성 100% 일치.
4. **장애 복구 검증**: `kill -9` 또는 결함 주입 시 HAM에 의한 1ms 내 재기동 로그 및 대시보드 복구 화면 캡처.
