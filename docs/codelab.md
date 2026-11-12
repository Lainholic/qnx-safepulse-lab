# SafePulse on QNX: live demo walkthrough

This Windows walkthrough builds the SafePulse guest for **QNX Neutrino 8.0 (QNX SDP 8.0)**, boots it in **QEMU** with a QNX security policy, and drives the live dashboard: real CAN replay, a bus-silence fault, a CRC fault, and a rogue process attack.

## Step 1. Check the tools and get the repository

Install QNX SDP 8.0 (default `C:\qnx800`) and QEMU (default `C:\Program Files\qemu`). In PowerShell:

```powershell
git clone https://github.com/Lainholic/qnx-safepulse-lab.git
Set-Location qnx-safepulse-lab
git remote get-url origin
python --version
python -c "import numpy; print('NumPy', numpy.__version__)"
& 'C:\Program Files\qemu\qemu-system-x86_64.exe' --version | Select-Object -First 1
cmd /c "call C:\qnx800\qnxsdp-env.bat >nul & qcc -V 2>&1"
```

Expected output:

```text
https://github.com/Lainholic/qnx-safepulse-lab.git
Python 3.10.6
NumPy 1.26.4
QEMU emulator version 11.1.0 (v11.1.0-12130-ge470268ff4)
cc: targets available in C:/qnx800/host/win64/x86_64/etc/qcc:
	12.2.0,gcc_ntoaarch64le
	12.2.0,gcc_ntoaarch64le_cxx
	12.2.0,gcc_ntoaarch64le_gpp
	12.2.0,gcc_ntox86_64	(default)
```

The real CAN replay needs NumPy; if the import fails, run `python -m pip install numpy`.

## Step 2. Build the guest binaries

From the repository root in PowerShell:

```powershell
cmd /c "call C:\qnx800\qnxsdp-env.bat && guest\build.bat"
```

Expected output (last lines):

```text
[Build] All QNX 8.0 target binaries, libsafepulse.a, and SDK examples built successfully (0 errors).
```

All guest programs are compiled with `qcc -Vgcc_ntox86_64`.

## Step 3. Build the secure QEMU image

Open a **Command Prompt** at the repository root and run:

```cmd
call C:\qnx800\qnxsdp-env.bat
cd guest\qemu_vm
mkqnximage --type=qemu --secpol=secure --usb=no --policy=../secpol/safepulse.secpol --clean --force
cd ..\..
```

Do not run `mkqnximage` from the repository root; it must run inside `guest\qemu_vm`.

Expected output:

```text
Target type: qemu
Architecture: x86_64
Security: secure
--policy='../secpol/safepulse.secpol'
--secpol='secure'
--usb='no'
```

The image now boots every SafePulse service under the `safepulse.secpol` security policy.

## Step 4. Start SafePulse and open the dashboard

From the repository root:

```powershell
python run_safepulse.py
```

Expected output:

```text
==================================================
 BlackBerry QNX SafePulse Lab Launching System...
==================================================
[1/3] Starting QNX 8.0 Microkernel (QEMU)...
      -> QNX 8.0 Guest starting in background.
[2/3] Starting Telemetry Bridge & Dashboard Server...
      -> Bridge daemon spawned on port 8765.
[3/3] Synchronizing Dashboard link...
      -> Dashboard HTTP 200 received.
==================================================
 QNX SafePulse Lab READY!
 Dashboard URL: http://127.0.0.1:8765
==================================================
```

`READY` means an HTTP GET to http://127.0.0.1:8765 returned a 2xx status. The launcher opens the dashboard in your browser; if it does not, open http://127.0.0.1:8765 yourself.

When the guest stream arrives, the dashboard shows:

```text
QNX 8.0 RTOS LIVE LINK
FSM: NORMAL
100
SAFE (NORMAL)
```

## Step 5. Replay real CAN data

If `_work\datasets` does not exist yet, download the comma2k19 sample from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch_dataset.ps1
```

Expected output:

```text
[Dataset] Fetching real vehicle datasets (comma2k19 + OpenXC)...
  -> [Cached] comma2k19_steering_t.npy already exists.
  -> [Cached] comma2k19_steering_val.npy already exists.
  -> [Cached] comma2k19_speed_t.npy already exists.
  -> [Cached] comma2k19_speed_val.npy already exists.
  -> [Cached] comma2k19_wheel_speed_t.npy already exists.
  -> [Cached] comma2k19_wheel_speed_val.npy already exists.
  -> [Cached] openxc_sample_trace.json already exists.
[Dataset] All datasets successfully fetched and staged in C:\AI\projects\qnx-safepulse-lab\_work\datasets
```

On a first run the script downloads the files instead of printing `[Cached]`.

On the dashboard, click **⚡ REAL CAN REPLAY**. Leave the dashboard open; the host bridge forwards the replay to the guest over its existing connection.

Expected dashboard state during the replay:

```text
Real CAN replay running
■ STOP REAL CAN
FSM: NORMAL
100
SAFE (NORMAL)
QNX 8.0 RTOS LIVE LINK
```

After 10 seconds:

```text
Replay finished (1795 packets, max gap 21.599 ms)
```

The virtual ECUs pause while the recorded vehicle data streams and resume when the replay ends.

## Step 6. Cut the bus during replay (Fault 11)

Click **⚡ REAL CAN REPLAY** again. While `Real CAN replay running` is shown, click **Bus Silence Starvation (80ms silence / 50ms limit)**.

Expected dashboard state:

```text
FSM: EMERGENCY_STOP
0
EMERGENCY STOP
DETECTED VIOLATION
STARVATION_WATCHDOG 0x1008
[VIOLATION TRAP] STARVATION_WATCHDOG 0x1008 (Code: 0x1008)
```

The safety ring turns fully red (`stroke-dashoffset` 0), and the guest serial log `_reports\qemu_live.log` records:

```text
[SafePulse FSM] CRITICAL: Sensor packet starvation detected (Silence >= 50 ms). Forcing EMERGENCY_STOP!
```

The replay goes silent for 80 ms, and the guest's 50 ms watchdog forces the emergency stop.

## Step 7. Reset, then inject a CRC fault and a rogue process

Wait for `Replay finished`, then click **RESET / CLEAR FAULT LATCH**. Expected:

```text
FSM: NORMAL
100
SAFE (NORMAL)
```

Click **AUTOSAR CRC-32 Bitflip**. Expected:

```text
FSM: EMERGENCY_STOP
0
EMERGENCY STOP
DETECTED VIOLATION
CRC32_CORRUPTED 0x1004
```

A single flipped bit fails the AUTOSAR CRC-32 check and latches the emergency stop.

Click **RESET / CLEAR FAULT LATCH** again, then click **Rogue process attack**. Expected:

```text
Write open refused by SafePulse resource manager using client credentials from QNX (errno 13). Reset refused by SafePulse resource manager using client credentials from QNX (errno 1). SIGKILL refused by QNX process manager / kernel ability check (errno 1).
```

The QNX security policy keeps the untrusted process from writing telemetry, resetting statistics, or killing the resource manager, and the FSM stays `NORMAL`.
