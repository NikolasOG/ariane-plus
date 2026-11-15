# Restart Ariane (e.g. after a rebuild) without losing the agent layer:
# saves a checkpoint, stops the editor, starts it, waits for the agent bridge,
# re-binds the agent scene and restores the checkpoint (objects + world removals).
#   powershell -NoProfile -File restart_ariane.ps1 [-Checkpoint name] [-NoSave] [-Workspace dir] [-Port n]
# Workspace layout (see README.md here): <ws>\ariane (this repo), <ws>\agent-data
# (token.txt, state\, samp_layer_agent.ipl), <ws>\.venv-agent, <ws>\run_ariane_samp.bat
param([string]$Checkpoint = "autosave-restart", [switch]$NoSave,
      [string]$Workspace = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..")).Path,
      [int]$Port = 48731)
$ErrorActionPreference = "Stop"
$root = $Workspace
$data = Join-Path $root "agent-data"
$env:ARIANE_ENGINE_TCP_PORT = "$Port"
$env:ARIANE_ENGINE_TOKEN = (Get-Content (Join-Path $data "token.txt") -Raw).Trim()
$ctl = Join-Path $root ".venv-agent\Scripts\arianectl.exe"
$common = @("--state-dir", (Join-Path $data "state"), "--discovery-dir", (Join-Path $data "discovery"))
function actl { & $ctl @common @args | Out-String }

if (-not $NoSave) {
    $r = actl checkpoint save $Checkpoint
    if ($r -notmatch '"object_count"') { Write-Output "checkpoint save failed:`n$r"; exit 1 }
    Write-Output "saved checkpoint $Checkpoint"
}
Get-Process | Where-Object { $_.ProcessName -like "ariane*" } | Stop-Process -Force -Confirm:$false
Start-Sleep -Seconds 2
Start-Process -FilePath (Join-Path $root "run_ariane_samp.bat") -WorkingDirectory $root -WindowStyle Normal
$ready = $false
for ($i = 0; $i -lt 90; $i++) {
    Start-Sleep -Seconds 3
    if ((actl capabilities) -match 'session_begin') { $ready = $true; break }
}
if (-not $ready) { Write-Output "Ariane did not come up (see ariane.log / crash-*.txt)"; exit 1 }
& powershell -NoProfile -File (Join-Path $PSScriptRoot "show_ariane.ps1")   # minimised = 0x0 captures
$scene = Join-Path $data "samp_layer_agent.ipl"
actl scene samp_layer.ipl $scene | Out-Null
actl session begin | Out-Null
$r = actl checkpoint restore $Checkpoint
$m = [regex]::Match($r, '"restored_count":\s*(\d+)')
$s = [regex]::Match($r, '"reapplied":\s*(\d+)')
actl session commit | Out-Null
$a = [regex]::Match($r, '"already_or_missing":\s*(\d+)')
Write-Output ("restored {0} objects; world removals: {1} re-applied, {2} already applied from the server file" -f $m.Groups[1].Value, $s.Groups[1].Value, $a.Groups[1].Value)
