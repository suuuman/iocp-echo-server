<#
    관문 검증.

    서버를 띄우고 검증 항목을 차례로 돌린 뒤 정상 종료까지 확인한다.
    한 항목이라도 실패하면 0 이 아닌 코드로 끝난다 - 자동화에서 이 값을 본다.

    사용:
        powershell -File scripts\verify.ps1
        powershell -File scripts\verify.ps1 -SkipDb          DB 없이 네트워크 계층만
        powershell -File scripts\verify.ps1 -Port 9500 -LoadSeconds 10
#>
param(
    [int]$Port = 9600,
    [string]$Config = "",
    [string]$BuildDir = "build",
    [int]$LoadSeconds = 5,
    [int]$LoadConnections = 64,
    [int]$ChurnRounds = 50,
    # DB 가 없는 환경에서는 DB 왕복 검증을 건너뛴다
    [switch]$SkipDb,
    # 정상 종료 대신 곧바로 끝낸다. 종료 경로를 따로 볼 때만 쓴다
    [switch]$SkipShutdown
)

$ErrorActionPreference = "Stop"

$root   = Split-Path -Parent $PSScriptRoot
$binDir = Join-Path $root "$BuildDir\bin"
$server = Join-Path $binDir "echo_server.exe"
$bot    = Join-Path $binDir "echo_bot.exe"
$tests  = Join-Path $binDir "echo_tests.exe"

$work = Join-Path $env:TEMP ("echo_verify_" + $PID)
New-Item -ItemType Directory -Path $work -Force | Out-Null

$results = @()
function Add-Result([string]$name, [bool]$ok, [string]$detail) {
    $script:results += [pscustomobject]@{ Name = $name; Ok = $ok; Detail = $detail }
    $mark = if ($ok) { "PASS" } else { "FAIL" }
    "{0}  {1,-22} {2}" -f $mark, $name, $detail
}

# ---------------------------------------------------------------
#  산출물 확인
# ---------------------------------------------------------------
foreach ($path in @($server, $bot, $tests)) {
    if (-not (Test-Path $path)) {
        Write-Output "빌드 산출물이 없다: $path"
        Write-Output "먼저 빌드한다: cmake --build $BuildDir"
        exit 1
    }
}

# ---------------------------------------------------------------
#  단위 테스트 - 서버를 띄우기 전에 본다
# ---------------------------------------------------------------
$unitOut = & $tests 2>&1
$unitOk  = ($LASTEXITCODE -eq 0)
$unitTail = ($unitOut | Select-Object -Last 1)
Add-Result "단위 테스트" $unitOk "$unitTail"

# ---------------------------------------------------------------
#  서버 기동
# ---------------------------------------------------------------
Push-Location $root
try {
    $srvErr = Join-Path $work "server.err"
    $srvOut = Join-Path $work "server.out"

    $srvArgs = @("--port", "$Port")
    if ($Config -ne "") { $srvArgs = @("--config", $Config) + $srvArgs }

    $srv = Start-Process -FilePath $server -ArgumentList $srvArgs -PassThru -WindowStyle Hidden `
           -RedirectStandardError $srvErr -RedirectStandardOutput $srvOut

    # 청취를 시작할 때까지 기다린다. 고정 대기로 두면 느린 장비에서 흔들린다
    $ready = $false
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 250
        if ($srv.HasExited) { break }
        if ((Get-Content $srvErr -ErrorAction SilentlyContinue) -match "echo server ready") {
            $ready = $true
            break
        }
    }
    if (-not $ready) {
        Add-Result "서버 기동" $false "청취 시작을 확인하지 못했다"
        if (-not $srv.HasExited) { Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue }
        exit 1
    }
    Add-Result "서버 기동" $true "port=$Port"

    # -----------------------------------------------------------
    #  검증 항목
    # -----------------------------------------------------------
    function Invoke-Bot([string]$label, [string[]]$botArgs, [string]$expect) {
        $out = & $bot @botArgs 2>&1
        $code = $LASTEXITCODE
        $text = ($out | Out-String)

        $errors = if ($text -match 'errors=(\d+)') { [int]$matches[1] } else { 0 }
        $ok = ($code -eq 0) -and ($errors -eq 0)
        if ($expect -ne "" -and $text -notmatch [regex]::Escape($expect)) { $ok = $false }

        $detail = ($out | Where-Object { $_ -match 'PASS|FAIL|errors=|throughput' } | Select-Object -Last 1)
        Add-Result $label $ok "$detail"
    }

    # 관리 포트로 GET 하나를 보낸다.
    # 상태 줄과 본문을 그대로 돌려준다 - 응답이 없으면 빈 문자열이다.
    #
    # 서버가 요청을 한 번만 읽으므로 한 번에 다 보내고, HTTP/1.0 으로 물어
    # 연결 종료가 곧 본문의 끝이 되게 한다
    function Invoke-Admin([int]$adminPort, [string]$path) {
        $client = $null
        try {
            $client = New-Object System.Net.Sockets.TcpClient
            $client.SendTimeout    = 5000
            $client.ReceiveTimeout = 5000
            $client.Connect("127.0.0.1", $adminPort)

            $stream  = $client.GetStream()
            $request = "GET $path HTTP/1.0`r`nHost: 127.0.0.1`r`n`r`n"
            $raw     = [System.Text.Encoding]::ASCII.GetBytes($request)
            $stream.Write($raw, 0, $raw.Length)
            $stream.Flush()

            $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::UTF8)
            return $reader.ReadToEnd()
        }
        catch { return "" }
        finally { if ($null -ne $client) { $client.Close() } }
    }

    Invoke-Bot "부하 (Echo)" @("--port","$Port","--connections","$LoadConnections","--seconds","$LoadSeconds") ""
    Invoke-Bot "접속·종료 반복" @("--port","$Port","--connections","20","--churn","$ChurnRounds") ""
    # 채팅 서비스가 자기 목록만으로 전원에게 보내는지, 그리고 게임 서비스가
    # 발급한 세션 키가 서비스 간 전달로 넘어왔는지를 함께 본다
    Invoke-Bot "브로드캐스트"   @("--port","$Port","--mode","chatverify") "PASS"

    if (-not $SkipDb) {
        Invoke-Bot "저장·조회 순서" @("--port","$Port","--mode","verify") "PASS"
        Invoke-Bot "멱등성"        @("--port","$Port","--mode","idem")   "PASS"
        Invoke-Bot "부하 (Save)"   @("--port","$Port","--connections","16","--seconds","$LoadSeconds","--message","save") ""
    }

    # -----------------------------------------------------------
    #  관리 포트 - 감시 도구가 보는 지점
    #
    #  여기가 죽어도 서버는 계속 돈다. 관문이 보지 않으면
    #  지표를 못 읽는 상태로 도는 것을 눈치챌 방법이 없다.
    #  부하를 돌린 뒤에 본다 - 값이 채워진 상태여야 껍데기와 구분된다
    # -----------------------------------------------------------
    $adminPort = 0
    $readyLine = Get-Content $srvErr -ErrorAction SilentlyContinue |
                 Select-String "echo server ready" | Select-Object -First 1
    if ($readyLine -match 'admin=(\d+)') { $adminPort = [int]$matches[1] }

    if ($adminPort -eq 0) {
        Add-Result "관리 포트 /health"  $false "관리 포트를 확인하지 못했다"
        Add-Result "관리 포트 /metrics" $false "관리 포트를 확인하지 못했다"
    }
    else {
        $healthText = Invoke-Admin $adminPort "/health"
        $healthCode = if ($healthText -match 'HTTP/1\.[01] (\d{3})') { [int]$matches[1] } else { 0 }

        # 200 은 정상, 503 은 "지금 트래픽을 받지 않는다" 는 정상 응답이다.
        # DB 없이 돌리면 후자가 나온다 - 응답이 없는 것과는 다르다
        $healthOk = if ($SkipDb) { $healthCode -eq 200 -or $healthCode -eq 503 }
                    else         { $healthCode -eq 200 }
        $healthDetail = if ($healthCode -eq 0) { "응답 없음 (admin=$adminPort)" }
                        else { "admin=$adminPort $healthCode" }
        Add-Result "관리 포트 /health" $healthOk $healthDetail

        $metricsText = Invoke-Admin $adminPort "/metrics"
        $metricsCode = if ($metricsText -match 'HTTP/1\.[01] (\d{3})') { [int]$matches[1] } else { 0 }

        # 이름만 나오면 껍데기다. 부하를 돌렸으니 값이 실제로 올라 있어야 한다
        $needNames = @("echo_uptime_seconds", "echo_connections",
                       "echo_messages_handled_total", "echo_send_issues_total")
        $missingNames = @($needNames | Where-Object { $metricsText -notmatch [regex]::Escape($_) })

        $handled = if ($metricsText -match '(?m)^echo_messages_handled_total (\d+)') { [int64]$matches[1] } else { 0 }

        $metricsOk = ($metricsCode -eq 200) -and ($missingNames.Count -eq 0) -and ($handled -gt 0)
        $metricsDetail =
            if ($metricsCode -eq 0)            { "응답 없음 (admin=$adminPort)" }
            elseif ($missingNames.Count -gt 0) { "누락: " + ($missingNames -join ", ") }
            else                               { "$metricsCode handled=$handled" }
        Add-Result "관리 포트 /metrics" $metricsOk $metricsDetail
    }

    # -----------------------------------------------------------
    #  정상 종료 - 받아 둔 일을 마치고 내려가는지 본다
    # -----------------------------------------------------------
    if ($SkipShutdown) {
        Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue
    } else {
        $signal = Join-Path $PSScriptRoot "signal-shutdown.ps1"
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $signal -TargetPid $srv.Id | Out-Null

        $exited = $srv.WaitForExit(30000)
        if (-not $exited) {
            Add-Result "정상 종료" $false "기한 안에 내려가지 않았다"
            Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue
        } else {
            $log = Get-Content $srvErr -ErrorAction SilentlyContinue
            $stages  = @("db drain done", "service drain done", "send drain done")
            $missing = @($stages | Where-Object { -not ($log -match [regex]::Escape($_)) })
            $timeout = @($log | Select-String "drain timeout")

            $ok = ($missing.Count -eq 0) -and ($timeout.Count -eq 0)
            $detail = if ($ok) { "드레인 3단계 완료" }
                      elseif ($timeout.Count -gt 0) { "드레인 기한 초과" }
                      else { "미완료: " + ($missing -join ", ") }
            Add-Result "정상 종료" $ok $detail
        }
    }

    # 처리 도중 예외로 끊긴 연결이 있으면 관문을 통과시키지 않는다
    $log = Get-Content $srvErr -ErrorAction SilentlyContinue
    $faults = 0
    foreach ($line in ($log | Select-String 'faults=(\d+)')) {
        if ($line -match 'faults=(\d+)') { $faults += [int]$matches[1] }
    }
    Add-Result "처리 중 예외" ($faults -eq 0) "faults=$faults"
}
finally {
    Pop-Location
}

# ---------------------------------------------------------------
#  요약
# ---------------------------------------------------------------
$failed = @($results | Where-Object { -not $_.Ok })
""
"검증 {0}건 중 {1}건 통과" -f $results.Count, ($results.Count - $failed.Count)

if ($failed.Count -gt 0) {
    "실패: " + (($failed | ForEach-Object { $_.Name }) -join ", ")
    "서버 로그: $work"
    exit 1
}

Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
exit 0
