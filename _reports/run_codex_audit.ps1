$ErrorActionPreference = 'Stop'
Set-Location 'C:\AI\projects\qnx-safepulse-lab'
$promptText = Get-Content '_reports\codex_audit_prompt.txt' -Raw -Encoding utf8
cmd.exe /c "codex.cmd exec --sandbox read-only -o _reports\codex_verification_report_20261114.md - < _reports\codex_audit_prompt.txt"
