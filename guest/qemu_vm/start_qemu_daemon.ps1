$scriptDir = $PSScriptRoot
$projectRoot = Split-Path (Split-Path $scriptDir -Parent) -Parent

$qemuDir = if ($env:QEMU_DIR) { $env:QEMU_DIR } else { "C:\Program Files\qemu" }
$qemu = Join-Path $qemuDir "qemu-system-x86_64.exe"
if (-not (Test-Path $qemu)) {
    $qemu = "qemu-system-x86_64"
}

$outputDir = Join-Path $scriptDir "output"
$vmdk = (Join-Path $outputDir "disk-qemu.vmdk") -replace '\\', '/'
$ifs = (Join-Path $outputDir "ifs.bin") -replace '\\', '/'
$reportsDir = Join-Path $projectRoot "_reports"
if (-not (Test-Path $reportsDir)) {
    New-Item -ItemType Directory -Force -Path $reportsDir | Out-Null
}
$logPath = (Join-Path $reportsDir "qemu_live.log") -replace '\\', '/'

$cmdLine = "`"$qemu`" -m 2048 -smp 2 -cpu max -drive file=$vmdk,if=ide,id=drv0 -netdev user,id=net0,hostfwd=tcp:127.0.0.1:9876-:9876 -device virtio-net-pci,netdev=net0 -kernel $ifs -nographic -serial file:$logPath"

$res = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
    CommandLine = $cmdLine
    CurrentDirectory = $outputDir
}
Write-Output "Spawned QEMU daemon PID: $($res.ProcessId), Return: $($res.ReturnValue)"
