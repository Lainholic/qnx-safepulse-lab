import urllib.request
import json
import os
import sys

DEEPSEEK_API_KEY = os.environ.get("DEEPSEEK_API_KEY", "")
DEEPSEEK_URL = "https://api.deepseek.com/v1/chat/completions"
MODEL = "deepseek-flash"

def call_deepseek(prompt, system_prompt="You are a Principal RTOS Systems Architect and ISO 26262 Automotive Safety Auditor.", max_tokens=3072, timeout=180):
    payload = {
        "model": MODEL,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": prompt}
        ],
        "temperature": 0.2,
        "max_tokens": max_tokens
    }
    
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {DEEPSEEK_API_KEY}"
    }

    req = urllib.request.Request(DEEPSEEK_URL, data=json.dumps(payload).encode("utf-8"), headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            msg = data["choices"][0]["message"]
            return msg.get("content") or msg.get("reasoning") or json.dumps(msg, ensure_ascii=False)
    except Exception as e:
        return f"ERROR: {e}"

if __name__ == "__main__":
    if len(sys.argv) > 1:
        prompt = sys.argv[1]
    else:
        prompt = "Hello DeepSeek, confirm you are ready to audit QNX 8.0 C code."
    print(call_deepseek(prompt))
