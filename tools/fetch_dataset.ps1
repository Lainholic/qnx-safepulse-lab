# Fetch Real Vehicle Datasets for SafePulse Validation
# Downloads comma2k19 and OpenXC real-world driving data into _work/datasets/

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$TargetDir = Join-Path $ProjectRoot "_work\datasets"

if (-not (Test-Path $TargetDir)) {
    New-Item -ItemType Directory -Path $TargetDir -Force | Out-Null
    Write-Host "[Dataset] Created target directory: $TargetDir" -ForegroundColor Cyan
}

$CommaBase = "https://raw.githubusercontent.com/commaai/comma2k19/master/Example_1/b0c9d2329ad1606b%7C2018-08-02--08-34-47/40/processed_log/CAN"

$Files = @(
    @{ Name = "comma2k19_steering_t.npy"; Url = "$CommaBase/steering_angle/t" },
    @{ Name = "comma2k19_steering_val.npy"; Url = "$CommaBase/steering_angle/value" },
    @{ Name = "comma2k19_speed_t.npy"; Url = "$CommaBase/speed/t" },
    @{ Name = "comma2k19_speed_val.npy"; Url = "$CommaBase/speed/value" },
    @{ Name = "comma2k19_wheel_speed_t.npy"; Url = "$CommaBase/wheel_speed/t" },
    @{ Name = "comma2k19_wheel_speed_val.npy"; Url = "$CommaBase/wheel_speed/value" },
    @{ Name = "openxc_sample_trace.json"; Url = "https://raw.githubusercontent.com/openxc/openxc-python/master/tests/trace.json" }
)

Write-Host "[Dataset] Fetching real vehicle datasets (comma2k19 + OpenXC)..." -ForegroundColor Yellow

foreach ($item in $Files) {
    $dest = Join-Path $TargetDir $item.Name
    if (Test-Path $dest) {
        Write-Host "  -> [Cached] $($item.Name) already exists." -ForegroundColor Gray
    } else {
        Write-Host "  -> [Downloading] $($item.Name) from $($item.Url)..." -ForegroundColor Green
        try {
            Invoke-WebRequest -Uri $item.Url -OutFile $dest -UseBasicParsing -TimeoutSec 30
            Write-Host "  -> [Success] Downloaded $($item.Name)" -ForegroundColor Green
        } catch {
            Write-Error "Failed to download $($item.Name): $_"
        }
    }
}

Write-Host "[Dataset] All datasets successfully fetched and staged in $TargetDir" -ForegroundColor Cyan
