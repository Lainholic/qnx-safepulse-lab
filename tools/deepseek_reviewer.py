import urllib.request
import json
import os
import sys
import time

NEBIUS_API_KEY = "v1.CmMKHHN0YXRpY2tleS1lMDBkcmExemN6YXZ0enAxd2sSIXNlcnZpY2VhY2NvdW50LWUwMGJwbjZjYXhhYjM3ODlicTIMCIOT_dYGEOO864sBOgsI_pWVogcQgMjQB0ACWgNlMDA.AAAAAAAAAAG8zHcQFvEH8gxp9qDPZWUH0v_7AW6FetjyEpNArrs6pPSBFLTweVp7429RRzXnMogCir1b3899kwfhr4J5Wz4J"
NEBIUS_URL = "https://api.tokenfactory.nebius.com/v1/chat/completions"
MODEL = "deepseek-ai/DeepSeek-V4-Pro"

def call_deepseek(prompt, system_prompt="You are a Principal RTOS Systems Architect and ISO 26262 Automotive Safety Auditor."):
    payload = {
        "model": MODEL,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": prompt}
        ],
        "temperature": 0.2,
        "max_tokens": 2048
    }
    
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {NEBIUS_API_KEY}"
    }

    req = urllib.request.Request(NEBIUS_URL, data=json.dumps(payload).encode("utf-8"), headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            return data["choices"][0]["message"]["content"]
    except Exception as e:
        return f"ERROR: {e}"

if __name__ == "__main__":
    if len(sys.argv) > 1:
        prompt = sys.argv[1]
    else:
        prompt = "Hello DeepSeek, confirm you are ready to audit QNX 8.0 C code."
    print(call_deepseek(prompt))
