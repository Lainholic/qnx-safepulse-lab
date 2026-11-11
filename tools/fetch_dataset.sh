#!/usr/bin/env bash
# Fetch Real Vehicle Datasets for SafePulse Validation
# Downloads comma2k19 and OpenXC real-world driving data into _work/datasets/

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
TARGET_DIR="$PROJECT_ROOT/_work/datasets"

mkdir -p "$TARGET_DIR"
echo "[Dataset] Staging target directory: $TARGET_DIR"

COMMA_BASE="https://raw.githubusercontent.com/commaai/comma2k19/master/Example_1/b0c9d2329ad1606b%7C2018-08-02--08-34-47/40/processed_log/CAN"

download_file() {
    local name="$1"
    local url="$2"
    local dest="$TARGET_DIR/$name"

    if [ -f "$dest" ]; then
        echo "  -> [Cached] $name already exists."
    else
        echo "  -> [Downloading] $name from $url..."
        curl -fsSL "$url" -o "$dest"
        echo "  -> [Success] Downloaded $name"
    fi
}

download_file "comma2k19_steering_t.npy" "$COMMA_BASE/steering_angle/t"
download_file "comma2k19_steering_val.npy" "$COMMA_BASE/steering_angle/value"
download_file "comma2k19_speed_t.npy" "$COMMA_BASE/speed/t"
download_file "comma2k19_speed_val.npy" "$COMMA_BASE/speed/value"
download_file "comma2k19_wheel_speed_t.npy" "$COMMA_BASE/wheel_speed/t"
download_file "comma2k19_wheel_speed_val.npy" "$COMMA_BASE/wheel_speed/value"
download_file "openxc_sample_trace.json" "https://raw.githubusercontent.com/openxc/openxc-python/master/tests/trace.json"

echo "[Dataset] All datasets successfully fetched in $TARGET_DIR"
