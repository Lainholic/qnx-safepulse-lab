import os
import sys
from pathlib import Path
from deepseek_reviewer import call_deepseek

BASE_DIR = Path(__file__).resolve().parent.parent
REPORTS_DIR = BASE_DIR / "_reports"
REPORT_MD = REPORTS_DIR / "deepseek_6round_audit_report.md"

REPORTS_DIR.mkdir(parents=True, exist_ok=True)
rounds_log = []

def run_round(round_num, topic, target_files, prompt_specifics):
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
"""
    critique = call_deepseek(full_prompt)
    rounds_log.append({
        "round": round_num,
        "topic": topic,
        "files": [os.path.basename(f) for f in target_files],
        "critique": critique
    })
    return critique

if __name__ == "__main__":
    print("Starting DeepSeek Audit Loop...")
