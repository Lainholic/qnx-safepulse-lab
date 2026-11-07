import struct
import zlib
import math
import time

# Protocol definitions
SAFEPULSE_MAGIC = 0x53504C53
CHAOS_MAGIC     = 0x4348414F

SENSOR_TYPE_STEERING = 1
SENSOR_TYPE_BRAKE    = 2
SENSOR_TYPE_THROTTLE = 3
SENSOR_TYPE_IMU_YAW  = 4

STATUS_FLAG_NORMAL      = 0x0000
STATUS_FLAG_WARNING     = 0x0001
STATUS_FLAG_CRITICAL    = 0x0002
STATUS_FLAG_HAM_RECOVER = 0x0004

VIOLATION_NONE          = 0x00000000
VIOLATION_OUT_OF_BOUNDS = 0x00001001
VIOLATION_JERK_EXCEEDED = 0x00001002
VIOLATION_DEADLINE_MISS = 0x00001003
VIOLATION_CRC_CORRUPTED = 0x00001004

# Wire format:
# magic(I), seq_num(I), timestamp_ns(Q), task_id(I), sensor_type(H), status_flags(H),
# val_primary(d), val_secondary(d), safety_score(f), violation_code(I), reserved(12s), crc32(I)
PACKET_FORMAT = "<IIQIHHddfI12sI"
PACKET_SIZE = struct.calcsize(PACKET_FORMAT)

CHAOS_FORMAT = "<IIII"
CHAOS_SIZE = struct.calcsize(CHAOS_FORMAT)

def pack_packet(magic, seq_num, timestamp_ns, task_id, sensor_type, status_flags,
                val_primary, val_secondary, safety_score, violation_code):
    partial = struct.pack("<IIQIHHddfI12s", magic, seq_num, timestamp_ns, task_id,
                          sensor_type, status_flags, val_primary, val_secondary,
                          safety_score, violation_code, b'\x00'*12)
    crc = zlib.crc32(partial) & 0xFFFFFFFF
    return partial + struct.pack("<I", crc)

def unpack_packet(data):
    fields = struct.unpack(PACKET_FORMAT, data)
    crc_payload = data[:60]
    expected_crc = zlib.crc32(crc_payload) & 0xFFFFFFFF
    actual_crc = fields[11]
    is_crc_valid = (expected_crc == actual_crc)
    return fields, is_crc_valid

def run_tests():
    print("==================================================")
    print(" QNX SafePulse Lab - Wire Protocol & Linter Verifier")
    print("==================================================")

    # TEST 1: Packet Wire Size
    print(f"[TEST 1] Verifying 64-byte wire alignment... ", end="")
    assert PACKET_SIZE == 64, f"Expected 64 bytes, got {PACKET_SIZE}"
    assert CHAOS_SIZE == 16, f"Expected 16 bytes, got {CHAOS_SIZE}"
    print(f"PASS (Packet: {PACKET_SIZE} bytes, Chaos: {CHAOS_SIZE} bytes)")

    # TEST 2: Normal Packet CRC
    print(f"[TEST 2] Testing normal packet packing & CRC32... ", end="")
    raw = pack_packet(SAFEPULSE_MAGIC, 1, 1000000000, 101, SENSOR_TYPE_STEERING,
                      STATUS_FLAG_NORMAL, 12.5, 0.0, 100.0, VIOLATION_NONE)
    fields, crc_valid = unpack_packet(raw)
    assert crc_valid is True
    assert fields[0] == SAFEPULSE_MAGIC
    assert fields[1] == 1
    assert fields[6] == 12.5
    print("PASS (CRC32 validated)")

    # TEST 3: CRC Corrupted Packet
    print(f"[TEST 3] Testing CRC corruption detection... ", end="")
    corrupted = bytearray(raw)
    corrupted[10] ^= 0xFF # Flip a bit in timestamp
    _, crc_valid = unpack_packet(bytes(corrupted))
    assert crc_valid is False
    print("PASS (Tampered packet detected)")

    # TEST 4: Performance Benchmark (100,000 serializations)
    print(f"[TEST 4] Benchmarking 100,000 packet serializations... ", end="")
    t0 = time.perf_counter()
    for i in range(100000):
        _ = pack_packet(SAFEPULSE_MAGIC, i, i * 10000000, 101, SENSOR_TYPE_STEERING,
                        STATUS_FLAG_NORMAL, 15.0, 0.0, 100.0, VIOLATION_NONE)
    t1 = time.perf_counter()
    avg_us = ((t1 - t0) / 100000) * 1e6
    print(f"PASS ({avg_us:.2f} microseconds / packet)")

    print("==================================================")
    print(" All wire protocol tests PASSED successfully.")
    print("==================================================")

if __name__ == "__main__":
    run_tests()
