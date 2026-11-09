# Runs recomp\buriki.exe headless and saves its log.
#   powershell -ExecutionPolicy Bypass -File D:\Buriki\tools\run_recomp.ps1 [-Frames 120] [-Log run.log] [-Extra '--trace-io']
param([int]$Frames = 120, [string]$Log = 'run.log', [string]$Extra = '', [int]$TimeoutSec = 120)
Set-Location D:\Buriki\recomp
$args_ = @('--frames', "$Frames", '--headless') + ($Extra -split ' ' | Where-Object { $_ })
$p = Start-Process -FilePath .\buriki.exe -ArgumentList $args_ -RedirectStandardError $Log -RedirectStandardOutput "$Log.out" -NoNewWindow -PassThru
$null = $p.Handle   # keeps the exit code readable after the process ends
if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill(); "timed out after $TimeoutSec s" } else { "exit code $($p.ExitCode)" }
