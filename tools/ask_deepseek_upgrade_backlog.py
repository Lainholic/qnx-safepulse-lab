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

def run_deepseek_backlog():
    prompt = """[SYSTEM INSTRUCTION: CONCRETE ENGINEERING UPGRADE & ACTIONABLE PUNCH-LIST]
You are a Principal Automotive Systems Architect and Senior Hackathon Mentor for the BlackBerry QNX Global Hackathon 2026.
Target repository: C:\\AI\\projects\\qnx-safepulse-lab on branch feat/demo-hardening (commit 4d5ad4e).

The developer (환님) wants a concrete, actionable engineering backlog of what to actually BUILD, MODIFY, or ADD next to push this project to the highest possible technical level.
Do not just give generic advice or critique wording. We want a concrete engineering backlog with specific files, components, and implementation designs.

Please answer the following in structured, clear Korean:
1. [P0 - Must Implement Now]: What are the 2-3 most critical engineering additions or code modifications required right now?
   - Exact files to create or modify
   - Exact data structures, APIs, or architectural connections
   - Why this will make the project undeniable to QNX judges
   (e.g., linking HAL directly into the resmgr receive loop, closed-loop vehicle physics simulator, hardware watchdog kicker, GSN safety case, etc.)
2. [P1 - High-Impact Upgrades]: What 2-3 features or verification components should be added next?
   - Exact implementation plan and technical specifications
3. [P2 - Polish & Integration]: What final integrations or demonstrations will maximize judging impact?
4. [Step-by-Step Implementation Roadmap]: The exact sequence of tasks we should execute next in the codebase.

STRICT RULES:
- Never use strikethroughs (~~...~~).
- Be concrete with file paths (e.g. guest/resmgr/..., include/..., host/...).
- Deliver in Korean.
"""

    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {API_KEY}"
    }
    payload = {
        "model": MODEL,
        "messages": [
            {"role": "system", "content": "You are a Principal Automotive Systems Architect. Provide a concrete, highly technical, and actionable engineering implementation punch-list."},
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
            output_path = r"C:\AI\projects\qnx-safepulse-lab\_reports\deepseek_upgrade_backlog.md"
            with open(output_path, "w", encoding="utf-8") as out_f:
                out_f.write(content)
            print("DEEPSEEK_BACKLOG_SUCCESS")
            return content
    except Exception as e:
        print(f"DEEPSEEK_BACKLOG_ERROR: {e}")
        return None

if __name__ == "__main__":
    run_deepseek_backlog()
