param(
    [string]$OutputDirectory = (Join-Path $env:TEMP ("rv1126-optical-" + (Get-Date -Format "yyyyMMdd-HHmmss"))),
    [ValidateRange(1, 10000)][int]$Samples = 120,
    [ValidateRange(100, 60000)][int]$IntervalMs = 500
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$screen = [System.Windows.Forms.SystemInformation]::VirtualScreen
$timer = [System.Diagnostics.Stopwatch]::StartNew()

Write-Host "Capturing $Samples desktop samples every $IntervalMs ms"
Write-Host "Output: $OutputDirectory"
for ($index = 0; $index -lt $Samples; $index++) {
    $targetMs = $index * $IntervalMs
    $remaining = $targetMs - $timer.ElapsedMilliseconds
    if ($remaining -gt 0) { Start-Sleep -Milliseconds $remaining }
    $bitmap = New-Object System.Drawing.Bitmap($screen.Width, $screen.Height)
    try {
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.CopyFromScreen($screen.Left, $screen.Top, 0, 0, $bitmap.Size)
        } finally { $graphics.Dispose() }
        $name = "sample-{0:D4}-{1}.png" -f ($index + 1), (Get-Date -Format "HHmmssfff")
        $bitmap.Save((Join-Path $OutputDirectory $name), [System.Drawing.Imaging.ImageFormat]::Png)
    } finally { $bitmap.Dispose() }
}
Write-Host "Done. Open scripts\optical_latency_analyzer.html and select all PNG files."
