import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from deepseek_reviewer import call_deepseek

BASE_DIR = Path(__file__).resolve().parent.parent

def run_final_deepseek_evaluation():
    submission_path = BASE_DIR / "SUBMISSION.md"
    report_e2e_path = BASE_DIR / "_reports" / "final_evaluation_report.md"
    report_chaos_path = BASE_DIR / "_reports" / "chaos_marathon_report.md"
    resmgr_c_path = BASE_DIR / "guest" / "resmgr" / "safepulse_resmgr.c"
    fsm_c_path = BASE_DIR / "guest" / "resmgr" / "safe_state_fsm.c"
    watcher_c_path = BASE_DIR / "guest" / "safepulse_watcher.c"
    ringbuffer_h_path = BASE_DIR / "guest" / "include" / "ringbuffer.h"

    submission_text = submission_path.read_text(encoding="utf-8")
    e2e_text = report_e2e_path.read_text(encoding="utf-8")
    chaos_text = report_chaos_path.read_text(encoding="utf-8")
    resmgr_c = resmgr_c_path.read_text(encoding="utf-8")
    fsm_c = fsm_c_path.read_text(encoding="utf-8")
    watcher_c = watcher_c_path.read_text(encoding="utf-8")
    ringbuffer_h = ringbuffer_h_path.read_text(encoding="utf-8")

    done_flag = BASE_DIR / "_reports" / "deepseek_eval.done"
    if done_flag.exists():
        done_flag.unlink()

    prompt = f"""You are the Chief Safety Assessor and Lead Judge for the BlackBerry QNX Global Hackathon 2026.
Please evaluate "QNX SafePulse Lab" across:
1. Technical Depth & QNX Neutrino Microkernel Exploitation (0-100)
2. ISO 26262 ASIL-D & AUTOSAR Rigor (0-100)
3. Production Robustness & Empirical Evidence (0-100)
4. Commercial Viability & OEM Adoption Attractiveness (0-100)

[SUBMISSION.md]
{submission_text[:1500]}

[safe_state_fsm.c]
{fsm_c[:1500]}

[safepulse_resmgr.c]
{resmgr_c[:1500]}

[safepulse_watcher.c]
{watcher_c[:1200]}
"""
    verdict = call_deepseek(prompt, max_tokens=3072, timeout=180)
    eval_out_path = BASE_DIR / "_reports" / "deepseek_3rd_final_grand_verdict.md"
    eval_out_path.write_text("# DeepSeek Final Evaluation: QNX SafePulse Lab\n\n" + verdict, encoding="utf-8")
    done_flag.write_text("DONE\n", encoding="utf-8")

if __name__ == "__main__":
    run_final_deepseek_evaluation()
