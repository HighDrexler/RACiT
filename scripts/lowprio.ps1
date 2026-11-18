# Run a Git Bash command line at BelowNormal priority (children inherit it), so
# a lift or build never makes the owner's PC crawl.
#   powershell -NoProfile -File scripts/lowprio.ps1 "<bash command>" <logfile>
param([Parameter(Mandatory)][string]$Command, [Parameter(Mandatory)][string]$Log)
$bash = 'C:\Program Files\Git\bin\bash.exe'
$root = Split-Path -Parent $PSScriptRoot
$p = Start-Process -FilePath $bash -ArgumentList '-c', "`"cd '$($root -replace '\\','/')' && ( $Command ) > '$($Log -replace '\\','/')' 2>&1`"" `
    -PassThru -WindowStyle Hidden
try { $p.PriorityClass = 'BelowNormal' } catch {}
Write-Output "pid $($p.Id) priority $($p.PriorityClass)"
$p.WaitForExit()
Write-Output "exit $($p.ExitCode)"
exit $p.ExitCode
