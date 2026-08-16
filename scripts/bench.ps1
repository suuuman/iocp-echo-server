<#
    측정.

    docs\benchmark.md 의 표를 만드는 데 쓴다.
    한 항목마다 서버를 새로 띄우고 내린다 - 앞 측정의 상태가 남지 않게 하기 위해서다.

    사용:
        powershell -File scripts\bench.ps1 -Suite echo
        powershell -File scripts\bench.ps1 -Suite broadcast
        powershell -File scripts\bench.ps1 -Suite dispatch
        powershell -File scripts\bench.ps1 -Suite all

    측정값은 장비 상태에 따라 회차마다 흔들린다.
    변경 전후를 비교할 때는 같은 시점에 양쪽을 번갈아 재는 편이 낫다.
#>
param(
    [ValidateSet("echo", "save", "dispatch", "lanes", "broadcast", "all")]
    [string]$Suite = "echo",
    [string]$ConnList = "",
    [int]$Seconds = 8,
    [int]$Port = 9700,
    [string]$BuildDir = "build",
    [string]$Config = "config\db.ini"
)

$ErrorActionPreference = "Stop"

$root   = Split-Path -Parent $PSScriptRoot
$binDir = Join-Path $root "$BuildDir\bin"
$server = Join-Path $binDir "echo_server.exe"
$bot    = Join-Path $binDir "echo_bot.exe"
$signal = Join-Path $PSScriptRoot "signal-shutdown.ps1"

$baseConfig = Join-Path $root $Config
if (-not (Test-Path $server) -or -not (Test-Path $bot)) {
    Write-Output "빌드 산출물이 없다. 먼저 빌드한다: cmake --build $BuildDir"
    exit 1
}

$work = Join-Path $env:TEMP ("echo_bench_" + $PID)
New-Item -ItemType Directory -Path $work -Force | Out-Null
$script:port = $Port

# 기준 설정에서 몇 개 키만 바꾼 임시 설정을 만든다
function New-Config([hashtable]$overrides) {
    if (-not (Test-Path $baseConfig)) { return "" }

    # 설정 파일은 UTF-8 이다. 인코딩을 지정하지 않으면 시스템 코드페이지로 읽혀
    # 한글 주석이 깨지고, 그 과정에서 뒤따르는 줄이 주석에 붙어 사라진다
    $text = Get-Content $baseConfig -Raw -Encoding UTF8
    foreach ($key in $overrides.Keys) {
        $value = $overrides[$key]
        $text = [regex]::Replace($text, "(?m)^\s*$key\s*=.*$", "$key = $value")
    }
    $path = Join-Path $work ("cfg_" + [System.Guid]::NewGuid().ToString("N").Substring(0, 8) + ".ini")
    [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding $true))
    return $path
}

# 한 번 재고 결과를 돌려준다.
# $chat 을 주면 채팅 접속을 함께 붙인다 - 브로드캐스트가 도는 동안의 값을 잰다
function Measure-Run([hashtable]$config, [int]$connections, [string]$message, [int]$seconds,
                     [hashtable]$chat = $null) {
    $script:port++
    $cfgPath = New-Config $config
    $srvErr  = Join-Path $work "srv_$script:port.err"

    $srvArgs = @("--port", "$script:port")
    if ($cfgPath -ne "") { $srvArgs = @("--config", $cfgPath) + $srvArgs }

    Push-Location $root
    try {
        $srv = Start-Process -FilePath $server -ArgumentList $srvArgs -PassThru -WindowStyle Hidden `
               -RedirectStandardError $srvErr -RedirectStandardOutput (Join-Path $work "srv.out")

        $ready = $false
        for ($i = 0; $i -lt 60; $i++) {
            Start-Sleep -Milliseconds 250
            if ($srv.HasExited) { break }
            if ((Get-Content $srvErr -ErrorAction SilentlyContinue) -match "echo server ready") {
                $ready = $true; break
            }
        }
        if (-not $ready) { return $null }

        $botOut = Join-Path $work "bot.out"
        $botArgs = @("--port", "$script:port", "--connections", "$connections",
                     "--seconds", "$seconds", "--message", $message)
        if ($null -ne $chat) {
            $botArgs += @("--chat", "$($chat.members)",
                          "--talkers", "$($chat.talkers)",
                          "--chat-period", "$($chat.period)")
        }
        $proc = Start-Process -FilePath $bot -ArgumentList $botArgs -PassThru -WindowStyle Hidden `
                -RedirectStandardOutput $botOut -RedirectStandardError (Join-Path $work "bot.err")

        # 부하가 도는 동안의 자원 사용량을 본다. 끝난 뒤에 재면 이미 줄어 있다
        $mem = 0; $threads = 0
        for ($i = 0; $i -lt 240; $i++) {
            Start-Sleep -Milliseconds 500
            if ($proc.HasExited) { break }
            try {
                $p = Get-Process -Id $srv.Id -ErrorAction Stop
                $mem = $p.WorkingSet64; $threads = $p.Threads.Count
            } catch { }
        }
        $proc.WaitForExit(120000) | Out-Null

        $text = Get-Content $botOut -Raw
        $result = [pscustomobject]@{
            Throughput = if ($text -match 'throughput = (\d+) msg/s') { [int]$matches[1] } else { 0 }
            P50        = if ($text -match 'rtt p50=(\d+)us') { [int]$matches[1] } else { 0 }
            P95        = if ($text -match 'p95=(\d+)us') { [int]$matches[1] } else { 0 }
            P99        = if ($text -match 'p99=(\d+)us') { [int]$matches[1] } else { 0 }
            Errors     = if ($text -match 'errors=(\d+)') { [int]$matches[1] } else { -1 }
            Connected  = if ($text -match 'connected=(\d+)') { [int]$matches[1] } else { 0 }
            MemMB      = [math]::Round($mem / 1MB, 1)
            Threads    = $threads
            Proc       = 0.0
            Handled    = @()
            # 채팅 - 보낸 채팅 1건이 실제 송신 몇 번이 됐는지와 그 지연
            ChatSent   = if ($text -match 'chat .*sent=(\d+)') { [int]$matches[1] } else { 0 }
            ChatPushed = if ($text -match 'pushed=(\d+)') { [int]$matches[1] } else { 0 }
            Fanout     = if ($text -match 'chat fanout = ([\d.]+)') { [double]$matches[1] } else { 0.0 }
            PushRate   = if ($text -match 'broadcast = (\d+) push/s') { [int]$matches[1] } else { 0 }
            ChatP50    = if ($text -match 'chat latency p50=(\d+)us') { [int]$matches[1] } else { 0 }
            ChatP99    = if ($text -match 'chat latency p50=\d+us p95=\d+us p99=(\d+)us') { [int]$matches[1] } else { 0 }
            RelayP50   = 0
            RelayP99   = 0
            # 브로드캐스트 한 번에 든 시간. 채팅 서비스가 기록에 남긴다
            BcastP50   = 0
            BcastP99   = 0
        }

        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $signal -TargetPid $srv.Id | Out-Null
        if (-not $srv.WaitForExit(30000)) { Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue }

        # 서버가 남긴 내부 계측을 거둔다
        # 게임 서비스의 값만 거둔다. 채팅 한 건은 접속 수만큼의 송신이라
        # 두 서비스를 섞어 평균 내면 "메시지 하나를 처리한 시간" 이 아니게 된다
        $log = Get-Content $srvErr -ErrorAction SilentlyContinue
        $procs = @()
        foreach ($line in ($log | Select-String 'game lane=\d+ .*proc=([\d.]+)us/msg')) {
            if ($line -match 'proc=([\d.]+)us/msg') { $procs += [double]$matches[1] }
        }
        if ($procs.Count -gt 0) {
            $result.Proc = [math]::Round(($procs | Measure-Object -Average).Average, 2)
        }
        foreach ($line in ($log | Select-String 'db worker (\d+) handled=(\d+)')) {
            if ($line -match 'db worker (\d+) handled=(\d+)') { $result.Handled += [int]$matches[2] }
        }

        # 서비스 간 전달 지연은 서버만 알 수 있다. 받는 쪽 서비스가 기록에 남긴다
        $relays = @()
        foreach ($line in ($log | Select-String 'relay n=\d+ p50/p95/p99=(\d+)/\d+/(\d+)us')) {
            if ($line -match 'relay n=\d+ p50/p95/p99=(\d+)/\d+/(\d+)us') {
                $relays += ,@([int]$matches[1], [int]$matches[2])
            }
        }
        if ($relays.Count -gt 0) {
            $result.RelayP50 = ($relays | ForEach-Object { $_[0] } | Measure-Object -Maximum).Maximum
            $result.RelayP99 = ($relays | ForEach-Object { $_[1] } | Measure-Object -Maximum).Maximum
        }

        # 서버가 잰 브로드캐스트 한 번의 시간. 봇이 재는 지연은 여기에 큐 대기와
        # 왕복이 더해진 값이라, 둘을 나란히 두어야 어디에 시간이 갔는지가 보인다
        $bcasts = @()
        foreach ($line in ($log | Select-String 'broadcast p50/p95/p99=(\d+)/\d+/(\d+)us')) {
            if ($line -match 'broadcast p50/p95/p99=(\d+)/\d+/(\d+)us') {
                $bcasts += ,@([int]$matches[1], [int]$matches[2])
            }
        }
        if ($bcasts.Count -gt 0) {
            $result.BcastP50 = ($bcasts | ForEach-Object { $_[0] } | Measure-Object -Maximum).Maximum
            $result.BcastP99 = ($bcasts | ForEach-Object { $_[1] } | Measure-Object -Maximum).Maximum
        }
        return $result
    }
    finally { Pop-Location }
}

function Show-Row([string]$label, $r) {
    if ($null -eq $r) { "{0,-24} 측정 실패" -f $label; return }

    $line = "{0,-24} {1,8} msg/s  p50={2,8} p95={3,8} p99={4,8}  err={5}  mem={6}MB  thr={7}  proc={8}us" -f `
            $label, $r.Throughput, $r.P50, $r.P95, $r.P99, $r.Errors, $r.MemMB, $r.Threads, $r.Proc
    if ($r.Handled.Count -gt 0) {
        $idle = @($r.Handled | Where-Object { $_ -eq 0 }).Count
        $line += "  워커=[" + ($r.Handled -join "/") + "] 노는워커=$idle"
    }
    $line
}

# ---------------------------------------------------------------
#  스위트
# ---------------------------------------------------------------
function Suite-Echo {
    "== Echo - DB 미접촉 =="
    $list = if ($ConnList -ne "") { $ConnList } else { "8,64,256,1000,2000" }
    foreach ($c in ($list -split ',')) {
        $n = [int]$c.Trim()
        Show-Row "접속 $n" (Measure-Run @{} $n "echo" $Seconds)
    }
    ""
}

function Suite-Save {
    "== Save - DB 워커 수별 (64 접속) =="
    foreach ($w in 4, 8, 16, 24) {
        Show-Row "워커 $w" (Measure-Run @{ worker_count = $w } 64 "save" $Seconds)
    }
    ""
}

function Suite-Dispatch {
    "== 워커 배정 방식 (Save, DB 워커 4) =="
    $list = if ($ConnList -ne "") { $ConnList } else { "4,8,16,64" }
    foreach ($c in ($list -split ',')) {
        $n = [int]$c.Trim()
        foreach ($mode in "static", "dynamic") {
            Show-Row "$mode 접속 $n" (Measure-Run @{ dispatch = $mode; worker_count = 4 } $n "save" $Seconds)
        }
    }
    ""
}

function Suite-Lanes {
    "== 게임 서비스 레인 수별 (1,000 접속) =="
    foreach ($s in 1, 2, 4) {
        Show-Row "레인 $s" (Measure-Run @{ game_lane_count = $s } 1000 "echo" $Seconds)
    }
    ""
}

# 브로드캐스트 한 줄. 처리량표와 항목이 달라 따로 찍는다
function Show-Chat([string]$label, $r) {
    if ($null -eq $r) { "{0,-24} 측정 실패" -f $label; return }

    "{0,-24} 채팅 {1,6}건 → 송신 {2,9}건  확산={3,7:N1}  {4,8} push/s  왕복 p50={5,7} p99={6,7}us  서버 p50={7,6} p99={8,6}us  전달 p50={9,4} p99={10,5}us  err={11}" -f `
        $label, $r.ChatSent, $r.ChatPushed, $r.Fanout, $r.PushRate,
        $r.ChatP50, $r.ChatP99, $r.BcastP50, $r.BcastP99,
        $r.RelayP50, $r.RelayP99, $r.Errors
}

function Suite-Broadcast {
    # 접속이 많으면 느린 연결이 송신 누적 상한에 먼저 걸린다.
    # 그 상태로 재면 큐가 아니라 그 상한을 재게 되므로 넉넉히 둔다
    $wide = @{ send_buffer_limit_kb = 4096; max_connections = 8192 }

    "== 브로드캐스트 - 접속 수별 (말하는 접속 1, 10ms 간격) =="
    $list = if ($ConnList -ne "") { $ConnList } else { "64,256,1000" }
    foreach ($c in ($list -split ',')) {
        $n = [int]$c.Trim()
        Show-Chat "접속 $n" (Measure-Run $wide 0 "echo" $Seconds @{ members = $n; talkers = 1; period = 10 })
    }
    ""

    # 이 절이 축을 기능으로 나눈 값어치를 보여 준다.
    # 채팅이 도는 동안 Echo 처리량이 유지되면 두 기능이 서로를 막지 않는 것이다
    "== 기능 축 - 채팅이 게임 처리를 막는가 (Echo 256 접속) =="
    Show-Row  "채팅 없음"       (Measure-Run $wide 256 "echo" $Seconds)
    $mixed = Measure-Run $wide 256 "echo" $Seconds @{ members = 256; talkers = 4; period = 10 }
    Show-Row  "채팅 256 동시"   $mixed
    Show-Chat "  같은 회차 채팅" $mixed
    ""
}

switch ($Suite) {
    "echo"      { Suite-Echo }
    "save"      { Suite-Save }
    "dispatch"  { Suite-Dispatch }
    "lanes"     { Suite-Lanes }
    "broadcast" { Suite-Broadcast }
    "all"       { Suite-Echo; Suite-Lanes; Suite-Broadcast; Suite-Save; Suite-Dispatch }
}

Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
