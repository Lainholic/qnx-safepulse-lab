# BlackBerry QNX SafePulse Lab — Demonstration Shot List & Verified Metrics

- **Platform Target**: BlackBerry QNX Neutrino RTOS 8.0 (x86_64 SMP, 2 vCPUs, 2048 MB) on QEMU 11.1.0
- **Recording Specification**: 1920x1080 (Full HD), 30 fps, H.264 High Profile (`libx264`, yuv420p, CRF 18)
- **Directory**: `C:\AI\projects\qnx-safepulse-lab\_video\`
- **Privacy Verification**: 100% Studio Clean Canvas. No personal files, bookmarks, desktop icons, passwords, or unrelated applications displayed.

---

## Shot List Table

| Scene File | Target Script Segment | Duration | Resolution & FPS | Key Captured Events & On-Screen Metrics |
|:---|:---|:---:|:---:|:---|
| [`scene_01.mp4`](file:///C:/AI/projects/qnx-safepulse-lab/_video/scene_01.mp4) | **Scene 1**: 8-Stage BIST Boot & Authoritative Kernel Badges (`0:00 ~ 0:30`) | `31.23s` | 1920x1080 @ 30fps | - Initial disconnected state: `FSM: 알 수 없음 (UNKNOWN)`, `RT: 알 수 없음 (UNKNOWN)`<br>- QNX 8.0 Microkernel boot & `/dev/safepulse` initial service ready in `15.189 ms` (PID 319507)<br>- **8-Stage Power-On BIST PASS**: `[BIST 1/8]` to `[BIST 8/8] AUTOSAR E2E Profile 1 Inline Verification (Normal/Bitflip/Skip/Repeat): PASS` (0 Latent Faults)<br>- Microsecond linter latency: `1.27 us < 1.0 ms` target<br>- QNX Microkernel Scheduling: `SCHED_SPORADIC (Pri: 50/15, Budget: 3ms/10ms, FFI active)`<br>- Live dashboard transition: authoritative top badges show **`FSM: NORMAL`** (Green) and **`RT: SCHED_SPORADIC (Pri 50)`** (Purple)<br>- Telemetry stream active at 50Hz, Wire frame `0x53504C53`, Total Inspected `1,940+`, Score `100 (SAFE)` |
| [`scene_02.mp4`](file:///C:/AI/projects/qnx-safepulse-lab/_video/scene_02.mp4) | **Scene 2**: Live Inline AUTOSAR E2E Profile 1 Fault Trapping (`0:30 ~ 1:10`) | `41.20s` | 1920x1080 @ 30fps | - Nominal 3-ECU stream: `ecu_steering` (50Hz), `ecu_braking` (100Hz), `ecu_powertrain` (20Hz) with `autosar_e2e_stamp_packet()` (DataID `0x1234`, CRC-32 `0xF4ACFB13`)<br>- Injection of **Fault 4 (`AUTOSAR CRC-32 Bitflip`)**: Inverting bit 0 in AUTOSAR E2E CRC byte (`reserved[4]`)<br>- Live Inline Linter detection: `[Linter Engine] CRITICAL: AUTOSAR E2E CRC mismatch! Expected=0xF4ACFB13, Got=0xF4ACFB12`<br>- Trapped code: **`VIOLATION_CRC_CORRUPTED (0x1004)`**, Safety Score drops to `0.0 (CRITICAL)`<br>- Authoritative Safe State transition: **`FSM: EMERGENCY_STOP`** (Red, Latched: TRUE)<br>- E2E sequence verification: Alive Counter Jump (`VIOLATION_E2E_SEQ_LOSS = 0x1007`, Score `60.0`) and Alive Counter Repeat (`VIOLATION_E2E_REPEATED = 0x1006`, Score `60.0`) PASS |
| [`scene_03.mp4`](file:///C:/AI/projects/qnx-safepulse-lab/_video/scene_03.mp4) | **Scene 3**: 3x Shortened 50ms Starvation Watchdog (`1:10 ~ 1:45`) | `36.17s` | 1920x1080 @ 30fps | - Operator Clear Fault Latch (`sendChaos(0)`): Restored `ASIL_STATE_NORMAL`, Score `100.0`<br>- On-screen 10-minute continuous endurance soak metrics (`_reports/starvation_soak_report`): `101,993 frames` across `600.0s`, **False-Positive Starvation Events: 0**, Max inter-packet jitter **`28.09 ms < 50.0 ms`**<br>- Hardened constant: **`STARVATION_LIMIT_NS = 50,000,000 ns`** (3x faster than legacy 150ms)<br>- Injection of **Fault 11** (Total ECU Bus Silence >= 50ms): Freezing virtual ECUs for 80 ms<br>- QNX Console watchdog trigger: `[SafePulse FSM] CRITICAL: Sensor packet starvation detected (Silence >= 50 ms). Forcing EMERGENCY_STOP!`<br>- Dashboard immediately escalates to **`FSM: EMERGENCY_STOP`** within deterministic 50ms budget |
| [`scene_04.mp4`](file:///C:/AI/projects/qnx-safepulse-lab/_video/scene_04.mp4) | **Scene 4**: Self-Healing Watcher Recovery & 6.0s Blackbox Replay (`1:45 ~ 2:30`) | `46.27s` | 1920x1080 @ 30fps | - Microkernel fault isolation demo: 2MB POSIX Shared Memory (`/safepulse_ringbuf`) + `safepulse_watcher`<br>- Injection of **Fault 3 (`SIGSEGV - Null Pointer Dereference`)**: Driver process terminated with signal 11; microkernel core remains fully functional<br>- High-availability supervisor: `safepulse_watcher` respawns `/proc/boot/safepulse_resmgr` (Run #2) via `posix_spawn()`<br>- Zero-copy driver reattachment to shared memory: **0 Historical Packets Lost (0.00% Data Loss)**<br>- State persistence: `[SafePulse FSM] Restored persistent safe state across crash: ASIL_STATE_EMERGENCY_STOP (Latched: TRUE)`<br>- E2E recovery latency: **`60.398 ms`** (50-Crash Marathon stats: Mean `61.4 ms`, Median `62.5 ms`, p95 `75.5 ms`, Max `88.7 ms`)<br>- **Blackbox Flight Recorder Scrubber**: Toggling `❚❚ REPLAY` mode and scrubbing backward across the 6.0-second rolling buffer (`-2.5s`, `-4.0s`, return to `LIVE`) to inspect pre-crash and post-crash waveform telemetry |

---

## File Manifest (Not Committed to Git)
1. `scene_01.mp4` — `5,420,943 bytes` (Duration: `31.23s`)
2. `scene_02.mp4` — `6,613,661 bytes` (Duration: `41.20s`)
3. `scene_03.mp4` — `6,031,756 bytes` (Duration: `36.17s`)
4. `scene_04.mp4` — `8,046,130 bytes` (Duration: `46.27s`)
5. `shot_list.md` — Detailed scene descriptions and empirical telemetry metrics
