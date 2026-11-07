import os
import sys
import time
import json
import subprocess

from deepseek_reviewer import call_deepseek

REPORTS_DIR = r"C:\AI\projects\qnx-safepulse-lab\_reports"
REPORT_MD = os.path.join(REPORTS_DIR, "deepseek_6round_audit_report.md")

os.makedirs(REPORTS_DIR, exist_ok=True)

rounds_log = []

def run_round(round_num, topic, target_files, prompt_specifics):
    print(f"\n==================================================")
    print(f" [Round {round_num}/6] DeepSeek Security & RTOS Audit: {topic}")
    print(f" Target Files: {', '.join([os.path.basename(f) for f in target_files])}")
    print(f"==================================================")

    code_context = ""
    for fpath in target_files:
        if os.path.exists(fpath):
            with open(fpath, "r", encoding="utf-8", errors="replace") as f:
                code_context += f"\n--- FILE: {os.path.basename(fpath)} ---\n" + f.read()

    full_prompt = f"""You are a Principal QNX Neutrino RTOS Kernel Engineer and ISO 26262 ASIL-D Systems Architect.
Audit the following C code for the topic: '{topic}'.

SPECIFIC AREAS TO CRITIQUE:
{prompt_specifics}

CODE TO AUDIT:
{code_context}

Please provide:
1. VULNERABILITIES & CODE SMELLS (List exact lines or patterns that could fail under multi-core concurrency, real-time deadlines, or kernel crashes).
2. CONCRETE HARDENING RECOMMENDATIONS (Explain exact POSIX/QNX APIs, memory barriers, or data structures to use).
3. OPTIMAL HARDENED C CODE SNIPPET (Provide the exact drop-in C code replacement).
Keep your analysis concise, rigorous, and strictly engineering-focused.
"""
    print("[DeepSeek] Sending prompt to deepseek-ai/DeepSeek-V4-Pro...")
    critique = call_deepseek(full_prompt)
    print("[DeepSeek] Received critique response.")

    rounds_log.append({
        "round": round_num,
        "topic": topic,
        "files": [os.path.basename(f) for f in target_files],
        "critique": critique
    })
    return critique

if __name__ == "__main__":
    print("Starting DeepSeek 6-Round Audit Loop...")
