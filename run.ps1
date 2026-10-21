# 开发用：运行重编译版并把日志写到 build/run.err
param([int]$Seconds = 20, [string[]]$Extra = @())
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$args2 = @('--game', "$root\game", '--save', "$root\build\save") + $Extra
$p = Start-Process -FilePath "$root\build\win\flyingsb.exe" -ArgumentList $args2 -NoNewWindow -PassThru `
    -RedirectStandardError "$root\build\run.err" -RedirectStandardOutput "$root\build\run.out"
if (-not $p.WaitForExit($Seconds * 1000)) { $p.Kill(); "已超时终止 (${Seconds}s)" }
"退出码: $($p.ExitCode)"
