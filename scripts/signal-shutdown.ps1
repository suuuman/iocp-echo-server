<#
    서버에 종료 신호를 보낸다.

    강제 종료로는 정상 종료 경로(드레인)를 지날 수 없다.
    콘솔 제어 이벤트를 보내려면 대상의 콘솔에 붙어야 하고,
    붙는 순간 이 프로세스의 콘솔이 바뀐다. 그래서 이 작업만 별도 프로세스로 떼어 둔다.

    사용: powershell -File scripts\signal-shutdown.ps1 -TargetPid <서버 PID>
#>
param(
    [Parameter(Mandatory = $true)]
    [int]$TargetPid
)

$source = @"
using System;
using System.Runtime.InteropServices;

public static class ConsoleSignal
{
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool AttachConsole(uint processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool FreeConsole();

    [DllImport("kernel32.dll")]
    public static extern bool SetConsoleCtrlHandler(IntPtr handler, bool add);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool GenerateConsoleCtrlEvent(uint ctrlEvent, uint processGroupId);
}
"@
Add-Type -TypeDefinition $source

[ConsoleSignal]::FreeConsole() | Out-Null

if (-not [ConsoleSignal]::AttachConsole([uint32]$TargetPid)) {
    Write-Output "콘솔에 붙지 못했다 - pid=$TargetPid"
    exit 1
}

# 보낸 신호를 자기 자신이 받아 먼저 죽지 않게 한다
[ConsoleSignal]::SetConsoleCtrlHandler([IntPtr]::Zero, $true) | Out-Null

$sent = [ConsoleSignal]::GenerateConsoleCtrlEvent(0, 0)   # 0 = CTRL_C_EVENT
[ConsoleSignal]::FreeConsole() | Out-Null

if ($sent) { exit 0 } else { exit 2 }
