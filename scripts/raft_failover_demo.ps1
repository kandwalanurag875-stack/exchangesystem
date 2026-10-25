# raft_failover_demo.ps1 — Milestone 7 live demo (Windows).
#
# Launches a 3-node Raft-replicated matching-engine cluster, lets it elect a
# leader and replicate orders, then KILLS the leader and shows a follower take
# over at a new term with an identical applied-state hash (zero data loss).
#
#   .\scripts\raft_failover_demo.ps1
#
# Linux/macOS: run three raft_node processes by hand (see docs/replication.md).
param([int]$RunSeconds = 4, [int]$RecoverSeconds = 5, [int]$Rate = 20)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe  = Join-Path $root "build\raft_node.exe"
if (-not (Test-Path $exe)) { Write-Host "build first:  .\build.ps1 raft_node" -ForegroundColor Red; exit 1 }
$logdir = Join-Path $env:TEMP "raft_demo"; New-Item -ItemType Directory -Force $logdir | Out-Null
$peers = "0:127.0.0.1:9000,1:127.0.0.1:9001,2:127.0.0.1:9002"

$procs = @{}
foreach ($id in 0..2) {
  $out = Join-Path $logdir "node$id.log"; Remove-Item $out -ErrorAction SilentlyContinue
  $procs[$id] = Start-Process -FilePath $exe `
    -ArgumentList @("--id","$id","--peers",$peers,"--rate","$Rate") `
    -RedirectStandardOutput $out -PassThru -WindowStyle Hidden
}
Write-Host "started 3-node cluster (logs in $logdir); running ${RunSeconds}s..." -ForegroundColor Cyan
Start-Sleep -Seconds $RunSeconds

function LastLine($id) { (Get-Content (Join-Path $logdir "node$id.log") | Select-String "term=" | Select-Object -Last 1).Line }

Write-Host "`n===== cluster running =====" -ForegroundColor Green
$leader = -1
foreach ($id in 0..2) { $l = LastLine $id; Write-Host $l; if ($l -cmatch "LEADER") { $leader = $id } }
if ($leader -lt 0) { Write-Host "no leader yet; try a longer -RunSeconds" -ForegroundColor Yellow }

Write-Host "`n*** KILLING leader node $leader ***" -ForegroundColor Red
Stop-Process -Id $procs[$leader].Id -Force
Start-Sleep -Seconds $RecoverSeconds

Write-Host "`n===== after leader crash =====" -ForegroundColor Green
$newLeader = -1
foreach ($id in 0..2) {
  if ($id -eq $leader) { Write-Host "[node $id] (killed leader)"; continue }
  $l = LastLine $id; Write-Host $l; if ($l -cmatch "LEADER") { $newLeader = $id }
}
Write-Host "`n--> node $newLeader took over (was node $leader). Matching 'hash=' across" -ForegroundColor Cyan
Write-Host "    survivors proves the replicated state machines are identical."

foreach ($id in 0..2) { if ($id -ne $leader) { Stop-Process -Id $procs[$id].Id -Force -ErrorAction SilentlyContinue } }
Write-Host "(cleaned up)"
