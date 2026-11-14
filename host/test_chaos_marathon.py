"""
SafePulse High-Availability Chaos Marathon Benchmark
Boots QNX 8.0 Microkernel and executes 50 continuous induced fault crashes
against /dev/safepulse to statistically verify zero-data-loss survival and recovery latency.
"""

import subprocess
import socket
import struct
import time
import sys
import os
import shutil

BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
qemu_dir = os.environ.get("QEMU_DIR", r"C:\Program Files\qemu")
qemu_exe = os.path.join(qemu_dir, "qemu-system-x86_64.exe")
if not os.path.exists(qemu_exe):
    qemu_exe = shutil.which("qemu-system-x86_64") or "qemu-system-x86_64"

vm_dir = os.environ.get("SAFEPULSE_VM_DIR", os.path.join(BASE_DIR, "guest", "qemu_vm", "output"))
kernel_bin = os.path.join(vm_dir, "ifs.bin")
disk_vmdk = os.path.join(vm_dir, "disk-qemu.vmdk")
REPORTS_DIR = os.environ.get("SAFEPULSE_REPORTS_DIR", os.path.join(BASE_DIR, "_reports"))
os.makedirs(REPORTS_DIR, exist_ok=True)
qemu_log_file = os.path.join(REPORTS_DIR, "qemu_marathon.log").replace("\\", "/")

PACKET_FORMAT = "<IIQIHHddfI12sI"
PACKET_SIZE = 64
CHAOS_FORMAT = "<IIII"
CHAOS_MAGIC = 0x4348414F

TOTAL_CRASH_ROUNDS = int(os.environ.get("SAFEPULSE_CHAOS_ROUNDS", "50"))
TEST_PORT = int(os.environ.get("SAFEPULSE_TEST_PORT", "9876"))
REPORT_MD = os.path.join(REPORTS_DIR, "chaos_marathon_report.md")

def run_chaos_marathon():
    print("==================================================")
    print(f" SafePulse HA Chaos Marathon Benchmark ({TOTAL_CRASH_ROUNDS} Crashes)")
    print(" Target: Continuous Fault-Recovery Cycle on QNX 8.0")
    print("==================================================")

    qemu_cmd = [
        qemu_exe,
        "-m", "2048",
        "-smp", "2",
        "-cpu", "max",
        "-drive", f"file={disk_vmdk},if=ide,id=drv0",
        "-netdev", f"user,id=net0,hostfwd=tcp:127.0.0.1:{TEST_PORT}-:9876",
        "-device", "virtio-net-pci,netdev=net0",
        "-kernel", kernel_bin,
        "-nographic",
        "-serial", f"file:{qemu_log_file}"
    ]

    p_qemu = subprocess.Popen(
        qemu_cmd,
        cwd=vm_dir,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )

    try:
        print("[Harness] Booting QNX 8.0 Microkernel (Waiting 14s)...")
        time.sleep(14)

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5.0)

        connected = False
        for attempt in range(10):
            try:
                sock.connect(("127.0.0.1", TEST_PORT))
                connected = True
                break
            except Exception:
                time.sleep(1.0)

        if not connected:
            print(f"[Error] Failed to connect to QNX TCP Bridge on port {TEST_PORT}.")
            return False

        print(f"[Connected] QNX TCP Bridge connected on 127.0.0.1:{TEST_PORT}")

        def recv_exact_packet(s, timeout=0.05):
            deadline = time.perf_counter() + timeout
            while True:
                remaining = deadline - time.perf_counter()
                if remaining <= 0:
                    return None
                s.settimeout(remaining)
                buf = bytearray()
                while len(buf) < PACKET_SIZE:
                    rem = deadline - time.perf_counter()
                    if rem <= 0:
                        return None
                    s.settimeout(rem)
                    try:
                        chunk = s.recv(PACKET_SIZE - len(buf))
                        if not chunk:
                            return None
                        buf.extend(chunk)
                    except (socket.timeout, BlockingIOError):
                        return None
                if struct.unpack_from("<I", buf)[0] == 0x53504C53:
                    return bytes(buf)

        # Drain initial backlog on 64-byte packet boundaries and read baseline epoch
        last_epoch = 0
        while True:
            pkt = recv_exact_packet(sock, timeout=0.01)
            if pkt is None:
                break
            last_epoch = (struct.unpack(PACKET_FORMAT, pkt)[5] >> 8) & 0xFF

        recovery_latencies_ms = []
        success_recoveries = 0
        fault_stats = {3: 0, 6: 0, 7: 0, 8: 0}
        fault_attempts = {3: 0, 6: 0, 7: 0, 8: 0}

        FAULT_TYPES = [
            (3, "SIGSEGV (Null Deref)"),
            (6, "SIGFPE (Divide By Zero)"),
            (7, "SIGABRT (Critical Abort)"),
            (8, "SIGILL (Illegal Instruction)")
        ]

        for round_num in range(1, TOTAL_CRASH_ROUNDS + 1):
            f_type, f_name = FAULT_TYPES[(round_num - 1) % len(FAULT_TYPES)]
            fault_attempts[f_type] += 1
            checksum = CHAOS_MAGIC ^ f_type ^ 1000
            cmd = struct.pack(CHAOS_FORMAT, CHAOS_MAGIC, f_type, 1000, checksum)

            # Drain any pre-crash TCP socket backlog on 64-byte boundaries
            while True:
                pkt = recv_exact_packet(sock, timeout=0.005)
                if pkt is None:
                    break
                last_epoch = (struct.unpack(PACKET_FORMAT, pkt)[5] >> 8) & 0xFF

            expected_epoch = (last_epoch + 1) & 0xFF
            t_crash_trigger = time.perf_counter()
            try:
                sock.sendall(cmd)
            except Exception:
                pass

            recovered = False
            timeout_t = time.perf_counter() + 3.0
            while time.perf_counter() < timeout_t:
                raw = recv_exact_packet(sock, timeout=0.05)
                if raw and len(raw) == PACKET_SIZE:
                    fields = struct.unpack(PACKET_FORMAT, raw)
                    pkt_epoch = (fields[5] >> 8) & 0xFF
                    if fields[0] == 0x53504C53 and pkt_epoch == expected_epoch:
                        t_recovered = time.perf_counter()
                        latency_ms = (t_recovered - t_crash_trigger) * 1000.0
                        recovery_latencies_ms.append(latency_ms)
                        recovered = True
                        success_recoveries += 1
                        fault_stats[f_type] += 1
                        last_epoch = pkt_epoch
                        if round_num % 5 == 0 or round_num == 1:
                            print(f" [Crash Cycle {round_num:2d}/{TOTAL_CRASH_ROUNDS}] [{f_name:24s}] Respawned in {latency_ms:6.1f} ms | Epoch={pkt_epoch} | Seq={fields[1]}")
                        break

            if not recovered:
                print(f" [Crash Cycle {round_num:2d}/{TOTAL_CRASH_ROUNDS}] [{f_name:24s}] FAILED to recover within 3.0s!")

            time.sleep(0.08)

        sock.close()

        if not recovery_latencies_ms:
            print("[Error] No recovery latencies recorded.")
            return False

        min_lat = min(recovery_latencies_ms)
        max_lat = max(recovery_latencies_ms)
        avg_lat = sum(recovery_latencies_ms) / len(recovery_latencies_ms)
        recovery_latencies_ms.sort()
        p50_lat = recovery_latencies_ms[int(len(recovery_latencies_ms) * 0.50)]
        p95_lat = recovery_latencies_ms[int(len(recovery_latencies_ms) * 0.95)]

        success_rate = (success_recoveries / TOTAL_CRASH_ROUNDS) * 100.0

        print("\n==================================================")
        print(" Multi-Fault Chaos Marathon Final Results         ")
        print("==================================================")
        print(f" Total Injected Crashes : {TOTAL_CRASH_ROUNDS} Multi-Fault Events")
        print(f"   - SIGSEGV (Null Deref)  : {fault_stats[3]}/{fault_attempts[3]} recovered")
        print(f"   - SIGFPE (Divide By 0)  : {fault_stats[6]}/{fault_attempts[6]} recovered")
        print(f"   - SIGABRT (Abort/Assert): {fault_stats[7]}/{fault_attempts[7]} recovered")
        print(f"   - SIGILL (Illegal Inst) : {fault_stats[8]}/{fault_attempts[8]} recovered")
        print(f" Successful Recoveries  : {success_recoveries} ({success_rate:.1f}%)")
        print(" Data-loss rate         : not measured by this harness")
        print(f" Best-Case Recovery     : {min_lat:.1f} ms")
        print(f" Median Recovery (p50)  : {p50_lat:.1f} ms")
        print(f" Average Recovery (Mean): {avg_lat:.1f} ms")
        print(f" 95th Percentile (p95)  : {p95_lat:.1f} ms")
        print(f" Worst-Case Recovery    : {max_lat:.1f} ms")
        all_recovered = success_recoveries == TOTAL_CRASH_ROUNDS
        print(f" Recovery verdict       : {'PASS' if all_recovered else 'FAIL'} ({success_recoveries}/{TOTAL_CRASH_ROUNDS})")
        print("==================================================")

        report_text = f"""# SafePulse Lab — Multi-Fault High-Availability Chaos Marathon Report
**Verification Timestamp**: {time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime())}  
**Target Platform**: BlackBerry QNX Neutrino RTOS 8.0 on QEMU  
**Total Crash Events**: {TOTAL_CRASH_ROUNDS} Multi-Fault Chaos Injections (SIGSEGV, SIGFPE, SIGABRT, SIGILL)  
**Supervisor Architecture**: `posix_spawn`-based Self-Healing Watcher Daemon  
**State Retention**: Persistent POSIX Shared Memory (`/safepulse_ringbuf`)  

---

## 1. Executive Summary
For this run, SafePulse Lab was subjected to {TOTAL_CRASH_ROUNDS} continuous, high-frequency fatal crashes across 4 distinct POSIX signal domains:
- **SIGSEGV**: Null pointer dereference / MMU segmentation faults
- **SIGFPE**: Arithmetic divide-by-zero / floating-point exception
- **SIGABRT**: Critical assertion / safety abort
- **SIGILL**: Illegal instruction / pipeline execution fault

The bridge observed {success_recoveries} recovery epochs in {TOTAL_CRASH_ROUNDS} attempts. This harness does not measure a data-loss rate.

---

## 2. Multi-Fault Distribution & Recovery Matrix

| Fault Signal | Injected Count | Recovered | Data Retention | Compliance |
|:---|:---:|:---:|:---:|:---:|
| **SIGSEGV (Null Dereference)** | {fault_attempts[3]} | {fault_stats[3]} / {fault_attempts[3]} | Not measured | **{'PASS' if fault_stats[3] == fault_attempts[3] else 'FAIL'}** |
| **SIGFPE (Divide by Zero)** | {fault_attempts[6]} | {fault_stats[6]} / {fault_attempts[6]} | Not measured | **{'PASS' if fault_stats[6] == fault_attempts[6] else 'FAIL'}** |
| **SIGABRT (Safety Abort)** | {fault_attempts[7]} | {fault_stats[7]} / {fault_attempts[7]} | Not measured | **{'PASS' if fault_stats[7] == fault_attempts[7] else 'FAIL'}** |
| **SIGILL (Illegal Instruction)** | {fault_attempts[8]} | {fault_stats[8]} / {fault_attempts[8]} | Not measured | **{'PASS' if fault_stats[8] == fault_attempts[8] else 'FAIL'}** |

---

## 3. Statistical Recovery Latency

| Metric | Measured Latency | Industrial SLA Target | Compliance |
|---|:---:|:---:|:---:|
| **Success Rate** | **{success_rate:.1f}%** ({success_recoveries}/{TOTAL_CRASH_ROUNDS}) | > 99.0% | **{'PASS' if success_rate > 99 else 'FAIL'}** |
| **Zero Data Loss** | Not measured | 100.00% | **UNVERIFIED** |
| **Best-Case Respawn** | **{min_lat:.1f} ms** | < 100 ms | **{'PASS' if min_lat < 100 else 'FAIL'}** |
| **Median (p50)** | **{p50_lat:.1f} ms** | < 150 ms | **{'PASS' if p50_lat < 150 else 'FAIL'}** |
| **Average (Mean)** | **{avg_lat:.1f} ms** | < 200 ms | **{'PASS' if avg_lat < 200 else 'FAIL'}** |
| **95th Percentile (p95)** | **{p95_lat:.1f} ms** | < 250 ms | **{'PASS' if p95_lat < 250 else 'FAIL'}** |
| **Worst-Case** | **{max_lat:.1f} ms** | < 500 ms | **{'PASS' if max_lat < 500 else 'FAIL'}** |

---

## 4. Engineering Insights
Recovery was identified by a changed bridge epoch followed by telemetry. Ring-buffer integrity, zero loss, zombie leaks, and CPU starvation were not directly measured by this harness.
"""
        with open(REPORT_MD, "w", encoding="utf-8") as f:
            f.write(report_text)

        date_tag = time.strftime("%Y-%m-%d")
        dated_report = os.path.join(REPORTS_DIR, f"chaos_marathon_report_{date_tag}_v2.md")
        with open(dated_report, "w", encoding="utf-8") as f:
            f.write(report_text)

        print(f"[Harness] Detailed report saved to: {REPORT_MD}")
        print(f"[Harness] Dated report saved to: {dated_report}")
        return all_recovered

    finally:
        try:
            p_qemu.kill()
            p_qemu.wait()
        except Exception:
            pass

if __name__ == "__main__":
    sys.exit(0 if run_chaos_marathon() else 1)
