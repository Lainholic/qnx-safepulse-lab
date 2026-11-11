# BlackBerry QNX SafePulse Lab — Demonstration Shot List & Run Metrics

- **Platform Target**: BlackBerry QNX Neutrino RTOS 8.0 (x86_64 SMP, 2 vCPUs, 2048 MB) on QEMU (`PID=41040`)
- **Recording Specification**: 1920x1080 (Full HD), 30 fps, H.264 (`libx264`, `yuv420p`)
- **Video & Console Output Directory**: `C:\Users\USER\qnx_video_check\` (videos are not committed to Git)
- **Metrics Source**: Strictly measured from the live recording run (`qnx_serial_console.log` & `host_replay_bridge_console.log`)

---

## Shot List Table

| Scene File | Script Segment | Duration | Resolution & FPS | Measured Events & On-Screen Metrics (This Run Only) |
|:---|:---|:---:|:---:|:---|
| `scene_01.mp4` | **Scene 1**: QNX 8.0 Boot, 8-Stage BIST, Kernel Badges & `REAL CAN REPLAY` (`0:00 ~ 0:30`) | `32.27 s` | 1920x1080 @ 30fps | - QNX 8.0 pre-flight unit tests (14/14 PASS) & SDK dynamic rules check (4/4 verdict flips PASS)<br>- 100,000-run WCET profiler (`3893492830 Hz`): Min `0.831 us`, Median `1.012 us`, Mean `1.147 us`, p95 `1.202 us`, p99 `1.452 us`, p99.9 `14.094 us`, Max `1018.737 us` (`10.187%` of 10ms FTTI)<br>- 8-Stage Power-On BIST PASS (`0` latent faults); `/dev/safepulse` initial service ready in **`25.385 ms`** (`PID 344083`)<br>- Kernel scheduling active: **`SCHED_SPORADIC (Pri: 50/15, Budget: 3ms/10ms, FFI active)`**, top badge **`FSM: NORMAL`**<br>- `REAL CAN REPLAY` activated on `comma2k19` Toyota RAV4 Highway 280 (`9,448` loaded / `9,447` filtered frames): virtual ECUs stopped cleanly (`0` spurious starvation trips), live telemetry streaming at Score `100`, `NONE`, Cyan vehicle (`46 km/h`, `1,617` inspected) |
| `scene_02.mp4` | **Scene 2**: AUTOSAR E2E Profile 1 Faults — CRC Bitflip (`0x1004`), Alive Skip (`0x1007`) & Resets (`0:30 ~ 1:10`) | `37.87 s` | 1920x1080 @ 30fps | - **Fault 4 (`AUTOSAR CRC-32 Bitflip`)**: Trapped as **`CRC32_CORRUPTED 0x1004`**, Score `0`, Red vehicle, `FSM: EMERGENCY_STOP` (host-to-trap latency: **`13.49 ms`**)<br>- **Reset #1 (`Type=0`)**: Restores `ASIL_STATE_NORMAL`, Score `100`, Cyan vehicle, Violation `NONE` (`0` spurious starvation lines)<br>- **Fault 9 (`Alive Counter Skip (+5 jump)`)**: Trapped as **`E2E_SEQ_LOSS 0x1007`**, escalated across FTTI to `FSM: EMERGENCY_STOP`, Score `0`, Red vehicle (host-to-E-STOP latency: **`35.53 ms`**)<br>- **Reset #2 (`Type=0`)**: Restores `ASIL_STATE_NORMAL`, Score `100`, Cyan vehicle, Violation `NONE` |
| `scene_03.mp4` | **Scene 3**: 50ms Starvation Watchdog (`80ms` Bus Silence -> `0x1008`) & Reset (`1:10 ~ 1:45`) | `32.20 s` | 1920x1080 @ 30fps | - Dashboard button: **`Bus Silence Starvation (80ms silence / 50ms limit)`** (`Watchdog 0x1008`)<br>- **Fault 11 (`Type=11, Duration=80 ms`)**: Host pauses CAN stream for `80 ms` (`>= 50 ms` `STARVATION_LIMIT_NS`)<br>- Single runtime starvation trip in `qnx_serial_console.log`: `[SafePulse FSM] CRITICAL: Sensor packet starvation detected (Silence >= 50 ms). Forcing EMERGENCY_STOP!`<br>- Dashboard displays **`STARVATION_WATCHDOG 0x1008`**, Score `0`, Red vehicle, `FSM: EMERGENCY_STOP` (host-to-E-STOP latency across 80ms silence: **`88.05 ms`**)<br>- **Reset #3 (`Type=0`)**: Restores `ASIL_STATE_NORMAL`, Score `100`, Cyan vehicle, Violation `NONE` |
| `scene_04.mp4` | **Scene 4**: Self-Healing Watcher (`SIGSEGV` -> `0x1009`), Reset to `NONE` & Blackbox Scrubber (`1:45 ~ 2:30`) | `41.43 s` | 1920x1080 @ 30fps | - **Fault 3 (`SIGSEGV - Null Pointer Dereference`)**: Driver terminated with signal 11; dashboard latches distinct code **`CRASH_SIGSEGV 0x1009`**, Score `0`, Red vehicle, `FSM: EMERGENCY_STOP`<br>- `safepulse_watcher` respawns `/proc/boot/safepulse_resmgr` (`Run #2`), re-runs 8-stage BIST PASS, reconnects to 2MB POSIX SHM:<br>  `[SafePulse] SHM Ringbuffer Intact: Retained 11450 historical packets in 2MB SHM (0 unread, 0 dropped).`<br>- **Labeled Recovery Times**:<br>  1. **QNX Watcher Respawn** (`crash -> /dev/safepulse open`): **`31.494 ms`**<br>  2. **Host End-to-End to First Valid Packet** (`host inject -> epoch 0->1 RX`): **`41.87 ms`**<br>- **Reset #4 (`Type=0`)**: Restores `ASIL_STATE_NORMAL`, Score `100`, Cyan vehicle, Violation `NONE`<br>- **Blackbox Flight Recorder Scrubber**: Pauses replay, scrubs backward to `CRASH_SIGSEGV 0x1009` (`Frame 60/1500`, `-10.00s`, Red vehicle, Score `0`), scrubs forward to post-reset `NORMAL` (`NONE`, Cyan vehicle, Score `100`), and returns to live stream (`TX=13015 | RX=13014`) |

---

## File Manifest (Saved to `C:\Users\USER\qnx_video_check`, Not Committed to Git)
1. `scene_01.mp4` — `8,735,376 bytes` (`32.27 s`)
2. `scene_02.mp4` — `11,839,112 bytes` (`37.87 s`)
3. `scene_03.mp4` — `10,383,677 bytes` (`32.20 s`)
4. `scene_04.mp4` — `12,709,079 bytes` (`41.43 s`)
5. `qnx_serial_console.log` — `10,682 bytes`
6. `host_replay_bridge_console.log` — `4,988 bytes`
7. `shot_list.md` — Empirical scene metrics from this run only
