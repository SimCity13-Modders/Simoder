param([string]$Root, [string]$GameRoot, [switch]$Elevated)
$ErrorActionPreference = 'Stop'

# /** @summary Computes the exact binary identity without relying on optional PowerShell modules. */
function Get-BinaryHash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '') }
    finally { $stream.Dispose(); $hasher.Dispose() }
}

try {
    # /** @summary Resolves a development checkout or an installation with recorded source provenance. */
    $Root = [IO.Path]::GetFullPath($Root)
    if (Test-Path -LiteralPath (Join-Path $Root 'CMakeLists.txt')) {
        $sourceRoot = $Root
        if (!$GameRoot) { $GameRoot = 'C:\Program Files\EA Games\SimCity' }
    } else {
        $GameRoot = $Root
        $marker = Join-Path $Root 'simoder\developer-source.txt'
        if (!(Test-Path -LiteralPath $marker)) { Write-Host '[Simoder] Installed distribution: no development update source.'; exit 0 }
        $sourceRoot = [IO.File]::ReadAllText($marker).Trim()
    }
    $buildRoot = Join-Path $sourceRoot 'build\vs2026'
    $binaryRoot = Join-Path $buildRoot 'bin\Release'
    if (!(Test-Path -LiteralPath (Join-Path $sourceRoot 'CMakeLists.txt'))) { throw 'Recorded source checkout is unavailable.' }
    $changed = $false
    foreach ($name in @('sc13modloader.dll', 'simoder.exe')) {
        $source = Join-Path $binaryRoot $name
        $target = Join-Path $gameRoot ('simoder\' + $name)
        if (!(Test-Path -LiteralPath $source)) { throw "Missing build artifact: $source. Build Release first." }
        if (!(Test-Path -LiteralPath $target) -or (Get-BinaryHash $source) -ne (Get-BinaryHash $target)) { $changed = $true }
    }
    if (!$changed) { Write-Host '[Simoder] Loader and launcher match the local Release build.'; exit 0 }
    # /** @summary Refuses replacement while any game or watcher can still hold the previous binaries. */
    if (Get-Process -Name SimCity,simoder -ErrorAction SilentlyContinue) { throw 'Close SimCity and the Simoder watcher before updating. No running process was stopped.' }
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        $args = '-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -Root "' + $Root.TrimEnd('\') + '" -GameRoot "' + $GameRoot.TrimEnd('\') + '" -Elevated'
        $child = Start-Process powershell.exe -ArgumentList $args -Verb RunAs -WindowStyle Hidden -Wait -PassThru
        if ($child.ExitCode -ne 0) { throw "Update failed or was cancelled (exit $($child.ExitCode))." }
        exit 0
    }
    # /** @summary Installs the built distribution while the existing installer preserves user configuration. */
    & cmake --install $buildRoot --config Release --prefix $gameRoot
    if ($LASTEXITCODE -ne 0) { throw 'CMake installation failed; game launch is cancelled.' }
    foreach ($name in @('sc13modloader.dll', 'simoder.exe')) {
        if ((Get-BinaryHash (Join-Path $binaryRoot $name)) -ne (Get-BinaryHash (Join-Path $gameRoot ('simoder\' + $name)))) { throw "Installed binary verification failed: $name" }
    }
    Write-Host '[Simoder] Installed binaries verified against Release build.'
    exit 0
} catch { Write-Error $_ -ErrorAction Continue; exit 1 }
