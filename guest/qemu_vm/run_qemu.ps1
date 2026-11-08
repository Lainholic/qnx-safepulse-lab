$scriptDir = $PSScriptRoot
$projectRoot = Split-Path (Split-Path $scriptDir -Parent) -Parent

$qemuExe = "C:\Program Files\qemu\qemu-system-x86_64.exe"
if (-not (Test-Path $qemuExe)) {
    $qemuExe = "qemu-system-x86_64"
}

$vmdk = Join-Path $scriptDir "output\disk-qemu.vmdk"
$ifs = Join-Path $scriptDir "output\ifs.bin"
$logPath = Join-Path $projectRoot "_reports\qemu_live.log"

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $qemuExe
$psi.Arguments = "-m 2048 -smp 2 -cpu max -drive file=`"$vmdk`",if=ide,id=drv0 -netdev user,id=net0,hostfwd=tcp:127.0.0.1:9876-:9876 -device virtio-net-pci,netdev=net0 -kernel `"$ifs`" -nographic -serial file:`"$logPath`""
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true

[System.Diagnostics.Process]::Start($psi)
Write-Host "Started QEMU background process."
