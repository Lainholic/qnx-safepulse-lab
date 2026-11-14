import os
import sys
import json
import urllib.request

def get_api_key():
    pin_path = r"C:\AI\.secrets\ds.pin"
    if os.path.exists(pin_path):
        with open(pin_path, "r", encoding="utf-8") as f:
            key = f.read().strip()
            if key:
                return key
    return os.environ.get("DEEPSEEK_API_KEY", "").strip()

API_KEY = get_api_key()
API_URL = "https://api.deepseek.com/chat/completions"
MODEL = "deepseek-chat"

def run_audit():
    prompt = """[SYSTEM INSTRUCTION: STRICT AUTOMOTIVE SAFETY AUDIT & REALITY CHECK]
You are a Lead Safety Assessor and Principal Automotive Systems Judge for the BlackBerry QNX Global Hackathon 2026.
Evaluate the current codebase in C:\\AI\\projects\\qnx-safepulse-lab on branch feat/demo-hardening (commit 4d5ad4e).

Project: SafePulse Lab - BlackBerry QNX Neutrino 8.0 ASIL-D Zero-Copy Safety Gateway with Resilient Microkernel Recovery, Actuator Slew Clamping, and Real-Time Telemetry Bridge.

Recent additions in commit 4d5ad4e:
1. CAN FD Hardware Abstraction Layer (include/safepulse_hal.h, guest/hal/safepulse_hal_can.c) & HIL Bench Simulator (host/hil/hil_bench_simulator.py):
   - Bosch M_CAN/FlexCAN register emulation, 64-byte payload, CRC-21, TEC/REC error counters, ISO 11898-1 128x11-bit recessive bus-off recovery.
   - 4 HIL scenarios: quartz drift +/-100ppm, EMI burst bit-flip noise, physical bus-off recovery in 12.8ms, 100% bus overload stress at 3,196 pkts/s - all PASS.
2. Real-time deterministic WCET & Jitter Hardening (guest/benchmark_wcet.c):
   - Thread pinned to CPU Core 0 via ThreadCtl(_NTO_TCTL_RUNMASK, 0x01), mlockall(MCL_CURRENT | MCL_FUTURE), SCHED_FIFO Priority 55.
   - Live QEMU boot WCET: 255.043 us (2.55% of 10ms FTTI), p99.99 = 10.375 us.
3. ISO 26262 Part 5 FMEDA & Part 6 Table 8 100% MC/DC Coverage:
   - guest/test_mcdc_matrix.c: 15/15 test cases PASS in live QNX.
   - _reports/iso26262_fmeda_and_metrics.md: Quantitative FMEDA work product with SPFM = 99.42% (>=99%), LFM = 95.80% (>=90%), PMHF = 3.82 FIT (<10 FIT).

Please provide a cold, unvarnished, and realistic evaluation without any marketing hype or exaggerations:
1. [Podium / 1st Place Reality Check]: Is this genuinely competitive for Grand Prize (1st place) in the BlackBerry QNX Global Hackathon? Compare with typical high-end automotive submissions.
2. [Exaggeration & Fluff Detection]: What claims or phrases in the repository should be toned down or stated more accurately? (e.g., fail-safe vs fail-operational, ISO 26262 certified vs architected/verified prototype, zero-copy boundaries).
3. [Remaining Technical Boundaries]: What are the hard boundaries (e.g. QEMU vs actual silicon, emulated CAN controller vs physical transceivers) that must be stated transparently to judges?
4. [Final Verdict]: Clear, objective verdict for the developer (환님).

STRICT RULES:
- Never use strikethroughs (~~...~~).
- Answer in structured, clear Korean.
"""

    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {API_KEY}"
    }
    payload = {
        "model": MODEL,
        "messages": [
            {"role": "system", "content": "You are a Principal Automotive Safety Architect and Lead Judge for embedded automotive hackathons. Provide rigorous, objective, and brutally honest engineering evaluations without flattery."},
            {"role": "user", "content": prompt}
        ],
        "temperature": 0.2,
        "max_tokens": 3500
    }

    req = urllib.request.Request(API_URL, data=json.dumps(payload).encode("utf-8"), headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            res_data = json.loads(resp.read().decode("utf-8"))
            content = res_data["choices"][0]["message"]["content"]
            output_path = r"C:\AI\projects\qnx-safepulse-lab\_reports\deepseek_wave3_evaluation_report.md"
            with open(output_path, "w", encoding="utf-8") as out_f:
                out_f.write(content)
            print("DEEPSEEK_AUDIT_SUCCESS")
            return content
    except Exception as e:
        print(f"DEEPSEEK_AUDIT_ERROR: {e}")
        return None

if __name__ == "__main__":
    run_audit()
