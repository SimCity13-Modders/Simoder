# /** @summary Observes module loads without injection; optionally requests a normal EA launch. */
param(
    [string]$GameRoot = 'C:\Program Files\EA Games\SimCity',
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [ValidateSet('clean', 'proxy')][string]$Mode = 'clean',
    [switch]$Launch,
    [int]$TimeoutSeconds = 180
)
$ErrorActionPreference = 'Stop'
# /** @summary Use the x86 observer because a 64-bit Process.Modules snapshot can omit WOW64 modules. */
if ([Environment]::Is64BitProcess) {
    $observer = Join-Path $env:WINDIR 'SysWOW64\WindowsPowerShell\v1.0\powershell.exe'
    $forward = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath,
        '-GameRoot', $GameRoot, '-OutputPath', $OutputPath, '-Mode', $Mode, '-TimeoutSeconds', $TimeoutSeconds)
    if ($Launch) { $forward += '-Launch' }
    & $observer @forward
    exit $LASTEXITCODE
}
$gamePath = [IO.Path]::GetFullPath((Join-Path $GameRoot 'SimCity\SimCity.exe'))
$proxyPath = Join-Path $GameRoot 'SimCity\d3d9.dll'
if (Get-Process simoder -ErrorAction SilentlyContinue) { throw 'Close the Simoder watcher before observing startup.' }
if ($Launch -and (Get-Process SimCity -ErrorAction SilentlyContinue)) { throw 'SimCity is already running; no second launch was requested.' }
if ($Mode -eq 'clean' -and (Test-Path -LiteralPath $proxyPath)) { throw 'Clean observation requires the game-local d3d9 proxy to be absent.' }
$output = [IO.Path]::GetFullPath($OutputPath)
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($output)) | Out-Null
$writer = [IO.StreamWriter]::new($output, $false, [Text.UTF8Encoding]::new($false))
$timer = [Diagnostics.Stopwatch]::StartNew()
$seen = @{}
$firstD3d = $null
$readErrorWritten = $false
try {
    $writer.WriteLine((@{ event = 'observation'; mode = $Mode; executable = $gamePath; utc = [DateTime]::UtcNow.ToString('o'); method = '250ms module snapshots; timestamps are first observation, not exact load events' } | ConvertTo-Json -Compress))
    $writer.Flush()
    if ($Launch) {
        $uri = 'origin2://game/launch/?offerIds=71654,71480,71630,71650,71573,71631,71652,71572,71632,1004769,1004768,1004771,1004770,1008749,1008760,1008761,1008762,1008763,1008764,1008776,1008777,1008778,1008779,1015233,1015232,1015226&title=SimCity%u2122%u003a%u0020Complete%u0020Edition&cmdParams='
        # /** @summary Starts the existing interactive EA URI handler without the watcher or BAT. */
        $start = [Diagnostics.ProcessStartInfo]::new($uri)
        $start.UseShellExecute = $true
        [Diagnostics.Process]::Start($start) | Out-Null
        Write-Output 'EA launch requested without a Simoder watcher. Complete UAC/login manually if shown.'
    }
    while ($timer.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        foreach ($gameProcess in @(Get-Process SimCity -ErrorAction SilentlyContinue)) {
            try {
                if (!$gameProcess.Path) { throw 'Cannot read the game process path. Run this observer elevated if SimCity is elevated.' }
                if (![string]::Equals($gameProcess.Path, $gamePath, [StringComparison]::OrdinalIgnoreCase)) { continue }
                foreach ($module in $gameProcess.Modules) {
                    $key = "$($gameProcess.Id):$($module.FileName)"
                    if ($seen.ContainsKey($key)) { continue }
                    $seen[$key] = $true
                    $writer.WriteLine((@{ event = 'module'; pid = $gameProcess.Id; elapsedMs = $timer.ElapsedMilliseconds; path = $module.FileName; base = $module.BaseAddress.ToInt64(); size = $module.ModuleMemorySize } | ConvertTo-Json -Compress))
                    if ($module.ModuleName -ieq 'd3d9.dll') {
                        Write-Output "Observed d3d9: $($module.FileName) PID=$($gameProcess.Id)"
                        if ($null -eq $firstD3d) { $firstD3d = $timer.Elapsed.TotalSeconds }
                    }
                    if ($Mode -eq 'clean' -and $module.ModuleName -ieq 'sc13modloader.dll') { throw 'Clean startup was contaminated by Simoder injection.' }
                }
                $writer.Flush()
            } catch {
                if ($_.Exception.Message -like '*contaminated*') { throw }
                if (!$readErrorWritten) { Write-Output "Module observation error: $($_.Exception.Message)"; $readErrorWritten = $true }
            } finally { $gameProcess.Dispose() }
        }
        if ($null -ne $firstD3d -and $timer.Elapsed.TotalSeconds - $firstD3d -ge 12) { break }
        Start-Sleep -Milliseconds 250
    }
    if ($null -eq $firstD3d) { throw 'No d3d9 module observed before timeout; inspect EA/login and observation permissions.' }
    Write-Output "Observation saved: $output"
} finally { $writer.Dispose() }
