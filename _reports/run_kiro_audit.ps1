$ErrorActionPreference = 'Stop'
Set-Location 'C:\AI\projects\qnx-safepulse-lab'
$promptText = Get-Content '_reports\kiro_audit_prompt.txt' -Raw -Encoding utf8
$kiroPath = 'C:\Users\USER\AppData\Local\Kiro-Cli\kiro-cli.exe'
& $kiroPath chat $promptText --no-interactive --trust-all-tools | Out-File -FilePath '_reports\kiro_verification_report_20261114.md' -Encoding utf8
