$ErrorActionPreference = 'Stop'
Set-Location 'C:\AI\projects\qnx-safepulse-lab'
cmd.exe /c "codex.cmd exec --sandbox read-only -o _reports\codex_reevaluation_report_20261114.md - < _reports\codex_reevaluation_prompt.txt"
