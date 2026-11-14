$ErrorActionPreference = 'Stop'
Set-Location 'C:\AI\projects\qnx-safepulse-lab'
cmd.exe /c "codex.cmd exec --sandbox read-only -o _reports\codex_podium_evaluation_20261114.md - < _reports\codex_podium_prompt.txt"
