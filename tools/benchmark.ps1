# Standard benchmark suite (Windows). Same cases and output as tools/benchmark.sh (macOS), so the
# results of both machines can be put side by side.
#
#   powershell -ExecutionPolicy Bypass -File tools\benchmark.ps1 [-Exe path\to\FloatingIsland.exe] [-Rounds 3]
#
# Runs every case -Rounds times, interleaved (case 1, 2, ..., then again), so slow drifts such as
# the GPU heating up hit all cases alike, and reports the median of the per-run means.
# Writes a Markdown table to the console and to benchmark-<computer>.md next to the executable.
# Close any other FloatingIsland window first: it shares the GPU and skews the numbers.
param(
    [string]$Exe = "",
    [int]$Rounds = 3
)
$ErrorActionPreference = "Stop"
# The tables contain Chinese text: write UTF-8 to the console and the file.
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

if ($Exe -eq "") {
    # Ninja puts the executable in the build directory, the Visual Studio generator in Release\.
    foreach ($candidate in @("build-release\FloatingIsland.exe", "build-release\Release\FloatingIsland.exe")) {
        if (Test-Path $candidate) { $Exe = $candidate; break }
    }
    if ($Exe -eq "") { throw "FloatingIsland.exe not found; build Release first (see docs/windows.md)" }
}
$Exe = (Resolve-Path $Exe).Path
$frames = 1500

$cases = @(
    @{ Name = "默认（草密度 1）"; Flags = @() },
    @{ Name = "草密度 4"; Flags = @("--grass-density", "4") },
    @{ Name = "草密度 4，关闭剔除 / LOD、草投射阴影"; Flags = @("--grass-density", "4", "--no-culling", "--no-lod", "--grass-shadows") },
    @{ Name = "不画云海"; Flags = @("--no-clouds") },
    @{ Name = "IBL 每帧重做"; Flags = @("--ibl-every-frame") }
)

if (Get-Process -Name FloatingIsland -ErrorAction SilentlyContinue) {
    throw "Another FloatingIsland is running; close it first."
}

$gpuLine = & $Exe --frames 1 2>$null | Select-String -Pattern "^GPU: (.*)$" | Select-Object -First 1
$gpu = if ($gpuLine) { $gpuLine.Matches[0].Groups[1].Value } else { "unknown" }

# results[case][row] = list of means, one per round
$rows = @("GPU scene (sh+main)", "GPU shadow pass", "GPU main pass", "GPU IBL update", "CPU frame")
$results = @{}
for ($i = 0; $i -lt $cases.Count; $i++) {
    $results[$i] = @{}
    foreach ($row in $rows) { $results[$i][$row] = New-Object System.Collections.Generic.List[double] }
}

for ($r = 1; $r -le $Rounds; $r++) {
    for ($i = 0; $i -lt $cases.Count; $i++) {
        Write-Host "round $r/$($Rounds): $($cases[$i].Name)"
        $arguments = @("--benchmark", "$frames", "--test-spheres") + $cases[$i].Flags
        $output = & $Exe @arguments 2>&1 | ForEach-Object { "$_" }
        foreach ($row in $rows) {
            $line = $output | Where-Object { $_.StartsWith($row) } | Select-Object -First 1
            if ($line) {
                # "<row>  mean  p50  p95  p99": the mean is the 4th number from the end.
                $fields = $line -split "\s+" | Where-Object { $_ -ne "" }
                $results[$i][$row].Add([double]::Parse($fields[$fields.Count - 4], [Globalization.CultureInfo]::InvariantCulture))
            }
        }
    }
}

function Median([System.Collections.Generic.List[double]]$values) {
    if ($values.Count -eq 0) { return "-" }
    $sorted = $values | Sort-Object
    $n = $sorted.Count
    $m = if ($n % 2 -eq 1) { $sorted[($n - 1) / 2] } else { ($sorted[$n / 2 - 1] + $sorted[$n / 2]) / 2 }
    return $m.ToString("0.000", [Globalization.CultureInfo]::InvariantCulture)
}

$lines = @()
$lines += "GPU: $gpu  "
$lines += "$(Get-Date -Format 'yyyy-MM-dd HH:mm')，每项 $Rounds 次取中位数，--benchmark $frames（场景每帧画 16 遍，报告单遍）"
$lines += ""
$lines += "| 测试 | GPU 场景（阴影 + 主） | 阴影 Pass | 主 Pass | IBL 更新 | CPU 帧 |"
$lines += "| --- | --- | --- | --- | --- | --- |"
for ($i = 0; $i -lt $cases.Count; $i++) {
    $cells = $rows | ForEach-Object { Median $results[$i][$_] }
    $lines += "| $($cases[$i].Name) | $($cells -join ' | ') |"
}
$lines += ""
$lines += "单位 ms。CPU 帧包含 16 遍场景的提交，并且不受垂直同步限制，只用来看趋势。"

$lines | ForEach-Object { Write-Host $_ }
$out = Join-Path (Split-Path $Exe) "benchmark-$env:COMPUTERNAME.md"
[System.IO.File]::WriteAllLines($out, $lines, (New-Object System.Text.UTF8Encoding $false))
Write-Host "Saved to $out"
