[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9A-Fa-f:.]+$')]
    [string]$BoardIp,

    [ValidateRange(10, 86400)]
    [int]$DurationSeconds = 1800,

    [ValidateRange(1, 120)]
    [int]$ExpectedFps = 30,

    [string]$OutputDirectory = ""
)

$ErrorActionPreference = 'Stop'

if (-not (Get-Command ffmpeg.exe -ErrorAction SilentlyContinue)) {
    throw 'ffmpeg.exe was not found. Install FFmpeg and add its bin directory to PATH.'
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputDirectory = Join-Path $env:TEMP "rv1126-tcp-$stamp"
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$stopPing = Join-Path $OutputDirectory 'ping.stop'
$pingCsv = Join-Path $OutputDirectory 'ping.csv'
Remove-Item -Force -ErrorAction SilentlyContinue $stopPing

function Quote-Argument([string]$Value) {
    return '"' + $Value.Replace('"', '\"') + '"'
}

function Start-RtspStream([string]$Name) {
    $progress = Join-Path $OutputDirectory "$Name.progress"
    $stdout = Join-Path $OutputDirectory "$Name.stdout.log"
    $stderr = Join-Path $OutputDirectory "$Name.stderr.log"
    $url = "rtsp://${BoardIp}:8554/live/${Name}_stream"

    foreach ($path in @($progress, $stdout, $stderr)) {
        New-Item -ItemType File -Force -Path $path | Out-Null
    }

    $arguments = @(
        '-hide_banner', '-loglevel', 'warning', '-nostats',
        '-progress', (Quote-Argument $progress),
        '-rtsp_transport', 'tcp',
        '-i', (Quote-Argument $url),
        '-map', '0:v:0', '-an',
        '-t', $DurationSeconds,
        '-f', 'null', 'NUL'
    ) -join ' '

    $process = Start-Process -FilePath 'ffmpeg.exe' `
        -ArgumentList $arguments `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr `
        -NoNewWindow -PassThru

    return [pscustomobject]@{
        Name = $Name
        Process = $process
        Progress = $progress
        Stderr = $stderr
        Started = Get-Date
    }
}

function Wait-FirstFrame($Stream) {
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline) {
        $Stream.Process.Refresh()
        if ($Stream.Process.HasExited) {
            return $false
        }
        if ((Get-Content $Stream.Progress -ErrorAction SilentlyContinue) -match '^frame=[1-9][0-9]*$') {
            return $true
        }
        Start-Sleep -Milliseconds 200
    }
    return $false
}

function Last-ProgressValue([string]$Path, [string]$Key) {
    $line = Get-Content $Path -ErrorAction SilentlyContinue |
        Where-Object { $_ -match "^$([regex]::Escape($Key))=" } |
        Select-Object -Last 1
    if (-not $line) {
        return 0
    }
    return [int64](($line -split '=', 2)[1].Trim())
}

$pingJob = Start-Job -ArgumentList $BoardIp, $pingCsv, $stopPing -ScriptBlock {
    param($Address, $Csv, $StopFile)
    'host_time,reachable' | Set-Content -Encoding ascii $Csv
    while (-not (Test-Path $StopFile)) {
        & ping.exe -n 1 -w 1000 $Address *> $null
        $reachable = if ($LASTEXITCODE -eq 0) { 1 } else { 0 }
        ('{0},{1}' -f (Get-Date -Format o), $reachable) |
            Add-Content -Encoding ascii $Csv
        Start-Sleep -Seconds 5
    }
}

$streams = @()
$timedOut = $false
try {
    Write-Host "RV1126 Windows-host RTSP/TCP test"
    Write-Host "board=$BoardIp duration=${DurationSeconds}s target=${ExpectedFps}fps"
    Write-Host "results=$OutputDirectory"

    $main = Start-RtspStream 'main'
    $streams += $main
    if (-not (Wait-FirstFrame $main)) {
        throw 'Main stream did not produce a frame within 15 seconds.'
    }

    $sub = Start-RtspStream 'sub'
    $streams += $sub
    if (-not (Wait-FirstFrame $sub)) {
        throw 'Sub stream did not produce a frame within 15 seconds.'
    }

    Write-Host 'Both streams are ready.'
    $deadline = (Get-Date).AddSeconds($DurationSeconds + 60)
    do {
        Start-Sleep -Seconds 1
        $running = @($streams | Where-Object {
            $_.Process.Refresh()
            -not $_.Process.HasExited
        })
    } while ($running.Count -gt 0 -and (Get-Date) -lt $deadline)

    if ($running.Count -gt 0) {
        $timedOut = $true
        foreach ($stream in $running) {
            Stop-Process -Id $stream.Process.Id -Force -ErrorAction SilentlyContinue
        }
    }
}
finally {
    foreach ($stream in $streams) {
        $stream.Process.Refresh()
        if (-not $stream.Process.HasExited) {
            Stop-Process -Id $stream.Process.Id -Force -ErrorAction SilentlyContinue
        }
    }
    New-Item -ItemType File -Force -Path $stopPing | Out-Null
    Wait-Job $pingJob -Timeout 10 | Out-Null
    Stop-Job $pingJob -ErrorAction SilentlyContinue
    Remove-Job $pingJob -Force -ErrorAction SilentlyContinue
}

$expectedFrames = $DurationSeconds * $ExpectedFps
$minimumFrames = $expectedFrames - 2
$failurePattern = 'corrupt|error while decoding|Invalid NAL|non-existing PPS|RTP: missed|max delay reached|Invalid data|timed out|Connection refused|CSeq .*expected|non.?monotonically increasing dts'
$failed = $timedOut
$summary = @()

foreach ($stream in $streams) {
    $stream.Process.Refresh()
    $frames = Last-ProgressValue $stream.Progress 'frame'
    $fps = [math]::Round($frames / $DurationSeconds, 2)
    $exitCode = if ($stream.Process.HasExited) { $stream.Process.ExitCode } else { -1 }
    $errors = Select-String -Path $stream.Stderr -Pattern $failurePattern -ErrorAction SilentlyContinue
    if ($frames -lt $minimumFrames -or $exitCode -ne 0 -or $errors) {
        $failed = $true
    }
    $summary += [pscustomobject]@{
        stream = $stream.Name
        frames = $frames
        average_fps = $fps
        expected_frames = $expectedFrames
        exit_code = $exitCode
        transport_errors = @($errors).Count
    }
}

$pingRows = @(Import-Csv $pingCsv -ErrorAction SilentlyContinue)
$pingFailures = @($pingRows | Where-Object { $_.reachable -ne '1' }).Count
if ($pingFailures -gt 0) {
    $failed = $true
}

$summary | Format-Table -AutoSize
$summary | Export-Csv -NoTypeInformation -Encoding UTF8 `
    (Join-Path $OutputDirectory 'summary.csv')
Write-Host "ping_samples=$($pingRows.Count) ping_failures=$pingFailures timed_out=$timedOut"

if ($failed) {
    Write-Host 'FAIL: Windows-host TCP dual-stream test failed.' -ForegroundColor Red
    Write-Host "Logs: $OutputDirectory"
    exit 1
}

Write-Host 'PASS: Windows-host TCP dual-stream test passed.' -ForegroundColor Green
Write-Host "Logs: $OutputDirectory"
exit 0
