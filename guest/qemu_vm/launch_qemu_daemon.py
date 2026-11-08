import subprocess
import os

base_dir = os.path.dirname(os.path.abspath(__file__))
project_root = os.path.abspath(os.path.join(base_dir, "..", ".."))

qemu_exe = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
if not os.path.exists(qemu_exe):
    qemu_exe = "qemu-system-x86_64"

vm_dir = os.path.join(base_dir, "output")
kernel_bin = os.path.join(vm_dir, "ifs.bin")
disk_vmdk = os.path.join(vm_dir, "disk-qemu.vmdk")
qemu_log_file = os.path.join(project_root, "_reports", "qemu_live.log")

DETACHED_PROCESS = 0x00000008
CREATE_NEW_PROCESS_GROUP = 0x00000200
FLAGS = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP

cmd = [
    qemu_exe,
    "-m", "2048",
    "-smp", "2",
    "-cpu", "max",
    "-drive", f"file={disk_vmdk},if=ide,id=drv0",
    "-netdev", "user,id=net0,hostfwd=tcp:127.0.0.1:9876-:9876",
    "-device", "virtio-net-pci,netdev=net0",
    "-kernel", kernel_bin,
    "-nographic",
    "-serial", f"file:{qemu_log_file}"
]

print(f"Launching QEMU daemon...")
p = subprocess.Popen(cmd, creationflags=FLAGS, close_fds=True)
print(f"QEMU launched with PID: {p.pid}")
