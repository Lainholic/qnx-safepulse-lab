#!/usr/bin/env python3
"""
CAN and Real-Vehicle Telemetry Replay Tool for SafePulse
Streams comma2k19 (CAN numpy) or OpenXC (NDJSON) driving datasets
into SafePulse (/dev/safepulse via TCP bridge) with real-time rate control (1x, 2x, 10x)
and deterministic fault injection capabilities.
"""

import sys
import os
import time
import json
import socket
import struct
import zlib
import argparse
import threading
import numpy as np

# AUTOSAR E2E Profile 1 Lookup Table (Polynomial: 0xF4ACFB13)
POLYNOMIAL = 0xF4ACFB13
AUTOSAR_TABLE = []
for i in range(256):
    c = i
    for _ in range(8):
        c = (c >> 1) ^ POLYNOMIAL if (c & 1) else (c >> 1)
    AUTOSAR_TABLE.append(c)


def autosar_crc32(data_id: int, payload: bytes) -> int:
    crc = 0xFFFFFFFF
    crc = (crc >> 8) ^ AUTOSAR_TABLE[(crc ^ (data_id & 0xFF)) & 0xFF]
    crc = (crc >> 8) ^ AUTOSAR_TABLE[(crc ^ ((data_id >> 8) & 0xFF)) & 0xFF]
    for b in payload:
        crc = (crc >> 8) ^ AUTOSAR_TABLE[(crc ^ b) & 0xFF]
    return (~crc) & 0xFFFFFFFF


def build_safepulse_packet(
    seq: int,
    timestamp_ns: int,
    task_id: int,
    sensor_type: int,
    val_primary: float,
    val_secondary: float,
    alive_counter: int,
    safety_score: float = 100.0,
    violation_code: int = 0,
    corrupt_crc: bool = False,
    override_alive: int = -1,
) -> bytes:
    """Pack a 64-byte safepulse_packet_t with AUTOSAR E2E Profile 1 protection."""
    magic = 0x53504C53  # "SPLS"
    flags = 0
    # Bytes 0..39: E2E protected payload
    payload_40 = struct.pack(
        "<IIQIHHdd",
        magic,
        seq,
        timestamp_ns,
        task_id,
        sensor_type,
        flags,
        float(val_primary),
        float(val_secondary),
    )

    data_id = 0x1000 + sensor_type
    e2e_crc = autosar_crc32(data_id, payload_40)
    if corrupt_crc:
        e2e_crc ^= 0xDEADBEEF

    effective_alive = (
        alive_counter if override_alive < 0 else override_alive
    ) & 0x0F

    reserved = bytearray(12)
    reserved[0] = data_id & 0xFF
    reserved[1] = (data_id >> 8) & 0xFF
    reserved[2] = effective_alive
    reserved[3] = 0xE2  # SAFEPULSE_E2E_MARKER
    reserved[4:8] = struct.pack("<I", e2e_crc)

    header_60 = (
        payload_40
        + struct.pack("<fI", float(safety_score), int(violation_code))
        + bytes(reserved)
    )

    full_crc = zlib.crc32(header_60) & 0xFFFFFFFF
    if corrupt_crc:
        full_crc ^= 0xCAFEBABE

    return header_60 + struct.pack("<I", full_crc)


class VehicleTraceReader:
    """Reads either comma2k19 CAN directory/files or OpenXC NDJSON file."""

    def __init__(self, trace_path: str):
        self.trace_path = os.path.abspath(trace_path)
        self.frames = []
        self._load()

    def _load(self):
        if os.path.isdir(self.trace_path):
            self._load_comma_dir(self.trace_path)
        elif self.trace_path.endswith(".json") or self.trace_path.endswith(
            ".ndjson"
        ):
            self._load_openxc_json(self.trace_path)
        elif self.trace_path.endswith(".npy"):
            # Assume comma2k19 parent dir
            self._load_comma_dir(os.path.dirname(self.trace_path))
        else:
            raise ValueError(f"Unsupported trace format: {self.trace_path}")

        # Sort frames strictly by timestamp
        self.frames.sort(key=lambda x: x["t"])
        if not self.frames:
            raise RuntimeError(f"No frames found in trace: {self.trace_path}")

    def _load_comma_dir(self, dpath: str):
        """Loads comma2k19 numpy series: steering and speed."""
        steer_t_file = os.path.join(dpath, "comma2k19_steering_t.npy")
        steer_v_file = os.path.join(dpath, "comma2k19_steering_val.npy")
        speed_t_file = os.path.join(dpath, "comma2k19_speed_t.npy")
        speed_v_file = os.path.join(dpath, "comma2k19_speed_val.npy")

        # Fallback to subdirectory structure if present
        if not os.path.exists(steer_t_file):
            alt_st_t = os.path.join(dpath, "steering_angle", "t")
            alt_st_v = os.path.join(dpath, "steering_angle", "value")
            if os.path.exists(alt_st_t):
                steer_t_file, steer_v_file = alt_st_t, alt_st_v

        if not os.path.exists(speed_t_file):
            alt_sp_t = os.path.join(dpath, "speed", "t")
            alt_sp_v = os.path.join(dpath, "speed", "value")
            if os.path.exists(alt_sp_t):
                speed_t_file, speed_v_file = alt_sp_t, alt_sp_v

        # Load steering
        if os.path.exists(steer_t_file) and os.path.exists(steer_v_file):
            st_t = np.load(steer_t_file).flatten()
            st_v = np.load(steer_v_file).flatten()
            for t, val in zip(st_t, st_v):
                self.frames.append(
                    {
                        "t": float(t),
                        "type": 1,  # SENSOR_TYPE_STEERING
                        "val_primary": float(val),
                        "val_secondary": 0.0,
                    }
                )

        # Load speed
        if os.path.exists(speed_t_file) and os.path.exists(speed_v_file):
            sp_t = np.load(speed_t_file).flatten()
            sp_v = np.load(speed_v_file).flatten()
            prev_v = None
            prev_t = None
            for t, val in zip(sp_t, sp_v):
                speed_kmh = float(val) * 3.6
                accel_pct = min(100.0, max(0.0, speed_kmh * 0.8))
                self.frames.append(
                    {
                        "t": float(t),
                        "type": 3,  # SENSOR_TYPE_THROTTLE
                        "val_primary": accel_pct,
                        "val_secondary": speed_kmh,
                    }
                )
                # Synthesize brake based on deceleration
                if prev_v is not None and prev_t is not None and (t > prev_t):
                    decel = (prev_v - float(val)) / (t - prev_t)
                    if decel > 0.5:
                        brake_bar = min(100.0, decel * 20.0)
                        self.frames.append(
                            {
                                "t": float(t) + 0.0001,
                                "type": 2,  # SENSOR_TYPE_BRAKE
                                "val_primary": brake_bar,
                                "val_secondary": 0.0,
                            }
                        )
                prev_v = float(val)
                prev_t = float(t)

    def _load_openxc_json(self, fpath: str):
        """Loads OpenXC newline-delimited JSON trace."""
        with open(fpath, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('{"metadata"'):
                    continue
                try:
                    obj = json.loads(line)
                except Exception:
                    continue
                name = obj.get("name")
                val = obj.get("value")
                ts = obj.get("timestamp")
                if name is None or val is None or ts is None:
                    continue

                if name == "steering_wheel_angle":
                    self.frames.append(
                        {
                            "t": float(ts),
                            "type": 1,
                            "val_primary": float(val),
                            "val_secondary": 0.0,
                        }
                    )
                elif (
                    name == "accelerator_pedal_position"
                    or name == "engine_speed"
                ):
                    self.frames.append(
                        {
                            "t": float(ts),
                            "type": 3,
                            "val_primary": float(val),
                            "val_secondary": 0.0,
                        }
                    )
                elif name == "brake_pedal_status":
                    brake_bar = 50.0 if val else 0.0
                    self.frames.append(
                        {
                            "t": float(ts),
                            "type": 2,
                            "val_primary": brake_bar,
                            "val_secondary": 0.0,
                        }
                    )


def replay_trace(
    trace_path: str,
    target: str = "127.0.0.1:9876",
    speed: float = 1.0,
    duration_s: float = 0.0,
    inject_fault: str = "none",
    loop: bool = True,
):
    """
    Connects to QNX SafePulse TCP endpoint and replays the dataset.
    Supports speed multiplier: 1.0 (realtime), 2.0 (2x), 10.0 (10x).
    """
    host, port_str = target.split(":")
    port = int(port_str)

    reader = VehicleTraceReader(trace_path)
    total_frames = len(reader.frames)
    print(
        f"[CAN Replay] Loaded {total_frames} frames from {trace_path} (speed={speed}x, target={target})"
    )

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(5.0)
    try:
        sock.connect((host, port))
        print(f"[CAN Replay] Connected to SafePulse bridge at {target}")
    except Exception as e:
        print(f"[CAN Replay] Connection to {target} failed: {e}")
        return False

    sock.settimeout(0.2)
    # Enter Real-Vehicle Replay Mode (pause guest virtual ECUs)
    replay_on_cmd = struct.pack("<IIII", 0x4348414F, 12, 0, 0x4348414F ^ 12)
    sock.sendall(replay_on_cmd)
    time.sleep(0.05)

    stop_rx = threading.Event()
    def _drain_rx():
        while not stop_rx.is_set():
            try:
                if not sock.recv(4096):
                    break
            except Exception:
                time.sleep(0.005)

    rx_t = threading.Thread(target=_drain_rx, daemon=True)
    rx_t.start()

    seq = 1
    alive_counters = {1: 0, 2: 0, 3: 0, 4: 0}
    start_wall = time.perf_counter()
    loop_start = start_wall
    replayed_count = 0
    anomalies_detected = []
    fault_injected = False
    last_send = None
    max_send_gap = 0.0
    max_lateness = 0.0

    try:
        while True:
            t0_trace = reader.frames[0]["t"]
            for frame in reader.frames:
                now_wall = time.perf_counter()
                elapsed_wall = now_wall - start_wall
                if duration_s > 0 and elapsed_wall >= duration_s:
                    print(
                        f"[CAN Replay] Target duration ({duration_s}s) reached."
                    )
                    return True

                # Use an absolute deadline so scheduling delays do not accumulate.
                deadline = loop_start + (frame["t"] - t0_trace) / max(0.1, speed)
                remaining = deadline - time.perf_counter()
                if remaining > 0.020:
                    time.sleep(remaining - 0.020)
                while time.perf_counter() < deadline:
                    pass
                now_wall = time.perf_counter()
                elapsed_wall = now_wall - start_wall
                if duration_s > 0 and elapsed_wall >= duration_s:
                    print(f"[CAN Replay] Target duration ({duration_s}s) reached.")
                    return True
                max_lateness = max(max_lateness, now_wall - deadline)

                stype = frame["type"]
                alive_counters[stype] = (alive_counters[stype] + 1) & 0x0F
                ts_ns = int(time.time() * 1e9)

                # Fault injection handling
                corrupt_crc = False
                override_alive = -1
                if inject_fault != "none" and not fault_injected and elapsed_wall > 2.0:
                    fault_injected = True
                    print(
                        f"[CAN Replay] Injecting fault: {inject_fault} at elapsed={elapsed_wall:.2f}s"
                    )
                    if inject_fault == "crc_bitflip":
                        corrupt_crc = True
                    elif inject_fault == "alive_skip":
                        # Jump alive counter by 5 (exceeding E2E tolerance of 2)
                        override_alive = (alive_counters[stype] + 5) & 0x0F
                    elif inject_fault == "starvation":
                        # Silence the bus for 70ms (> 50ms STARVATION_LIMIT_NS)
                        time.sleep(0.070)
                    elif inject_fault == "sigsegv":
                        # Send chaos command type 3
                        chaos_cmd = struct.pack("<IIII", 0x4348414F, 3, 100, 0)
                        sock.sendall(chaos_cmd)
                        time.sleep(0.05)

                pkt = build_safepulse_packet(
                    seq=seq,
                    timestamp_ns=ts_ns,
                    task_id=200 + stype,
                    sensor_type=stype,
                    val_primary=frame["val_primary"],
                    val_secondary=frame["val_secondary"],
                    alive_counter=alive_counters[stype],
                    corrupt_crc=corrupt_crc,
                    override_alive=override_alive,
                )

                send_time = time.perf_counter()
                if last_send is not None:
                    max_send_gap = max(max_send_gap, send_time - last_send)
                last_send = send_time
                sock.sendall(pkt)
                seq += 1
                replayed_count += 1

                if replayed_count % 500 == 0:
                    fps = replayed_count / max(0.1, time.perf_counter() - start_wall)
                    print(
                        f"  -> Replayed {replayed_count} pkts | Elapsed: {time.perf_counter()-start_wall:.1f}s | Rate: {fps:.1f} pkts/s"
                    )

            if not loop:
                break
            loop_start += (reader.frames[-1]["t"] - t0_trace) / max(0.1, speed)
    except KeyboardInterrupt:
        print("\n[CAN Replay] Interrupted by operator.")
    finally:
        stop_rx.set()
        try:
            replay_off_cmd = struct.pack("<IIII", 0x4348414F, 13, 0, 0x4348414F ^ 13)
            sock.sendall(replay_off_cmd)
        except Exception:
            pass
        sock.close()
        print(
            f"[CAN Replay] Finished. Total replayed: {replayed_count} packets in {time.perf_counter()-start_wall:.2f}s"
        )
        print(f"[CAN Replay] Max send gap: {max_send_gap * 1000:.3f} ms | Max deadline lateness: {max_lateness * 1000:.3f} ms")
    return True


def main():
    parser = argparse.ArgumentParser(
        description="SafePulse Real-Vehicle CAN Replay Engine"
    )
    parser.add_argument(
        "--trace",
        required=True,
        help="Path to trace file (.json) or directory containing comma2k19 numpy logs",
    )
    parser.add_argument(
        "--target",
        default="127.0.0.1:9876",
        help="Target QNX bridge IP:Port (default: 127.0.0.1:9876)",
    )
    parser.add_argument(
        "--speed",
        type=float,
        default=1.0,
        choices=[1.0, 2.0, 10.0],
        help="Replay speed factor: 1.0 (normal), 2.0 (2x), 10.0 (10x)",
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=0.0,
        help="Run duration in seconds (0 = infinite / single pass)",
    )
    parser.add_argument(
        "--inject-fault",
        default="none",
        choices=["none", "crc_bitflip", "alive_skip", "starvation", "sigsegv"],
        help="Fault type to inject during replay",
    )
    parser.add_argument(
        "--loop",
        action="store_true",
        default=True,
        help="Loop trace continuously (default: True)",
    )
    parser.add_argument(
        "--no-loop",
        dest="loop",
        action="store_false",
        help="Stop replay after single pass",
    )

    args = parser.parse_args()
    success = replay_trace(
        trace_path=args.trace,
        target=args.target,
        speed=args.speed,
        duration_s=args.duration,
        inject_fault=args.inject_fault,
        loop=args.loop,
    )
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
