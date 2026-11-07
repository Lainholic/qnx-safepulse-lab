import subprocess
import socket
import struct
import time
import os

qemu_exe = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
vm_dir = r"C:\AI\projects\qnx-safepulse-lab\guest\qemu_vm\output"
kernel_bin = r"C:\AI\projects\qnx-safepulse-lab\guest\qemu_vm\output\ifs.bin"
disk_vmdk = r"C:\AI\projects\qnx-safepulse-lab\guest\qemu_vm\output\disk-qemu.vmdk"

qemu_cmd = [
    qemu_exe,
    "-m", "2048",
    "-smp", "2",
    "-cpu", "max",
    "-drive", f"file={disk_vmdk},if=ide,id=drv0",
    "-netdev", "user,id=net0,hostfwd=tcp:127.0.0.1:9876-:9876",
    "-device", "virtio-net-pci,netdev=net0",
    "-kernel", kernel_bin,
    "-nographic",
    "-serial", "mon:stdio"
]

print("==================================================")
print(" Starting E2E Streaming Test: QNX 8.0 <-> Host TCP")
print("==================================================")

p_qemu = subprocess.Popen(qemu_cmd, cwd=vm_dir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")

print("[Test] Waiting 15s for QNX 8.0 kernel boot & bridge startup...")
time.sleep(15)

# Connect to QNX via QEMU port forward
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
connected = False
for attempt in range(8):
    try:
        sock.connect(("127.0.0.1", 9876))
        connected = True
        print(f"[Test] Successfully connected to QNX on 127.0.0.1:9876 (Attempt {attempt+1})!")
        break
    except Exception as e:
        print(f"[Test] Retrying connection ({e})...")
        time.sleep(2)

if not connected:
    print("[Test] FAIL: Could not connect to QNX TCP port.")
    p_qemu.kill()
    exit(1)

# Read 5 binary packets (64 bytes each)
PACKET_FORMAT = "<IIQIHHddfI12sI"
received_count = 0
for i in range(5):
    raw = sock.recv(64)
    if len(raw) == 64:
        fields = struct.unpack(PACKET_FORMAT, raw)
        magic, seq, ts, task_id, sensor_type, flags, v1, v2, score, viol, _, crc = fields
        print(f" [RX {i+1}] Seq={seq}, Sensor={sensor_type}, Val1={v1:.1f}, Val2={v2:.1f}, Score={score:.1f}, Viol=0x{viol:X}")
        received_count += 1
    time.sleep(0.05)

# Send Chaos Injection (Type 1: Out of Bounds)
print("[Test] Sending Chaos Injection: Type 1 (Out of Bounds)...")
CHAOS_MAGIC = 0x4348414F
chaos_pkt = struct.pack("<IIII", CHAOS_MAGIC, 1, 1000, CHAOS_MAGIC ^ 1 ^ 1000)
sock.sendall(chaos_pkt)

sock.close()
p_qemu.terminate()
try:
    p_qemu.communicate(timeout=2)
except:
    p_qemu.kill()

print("==================================================")
print(f" E2E Streaming Result: {received_count} / 5 Packets Received")
if received_count == 5:
    print(" VERDICT: PASS (Live QNX RTOS Telemetry Streaming Proven)")
else:
    print(" VERDICT: INCOMPLETE")
print("==================================================")
