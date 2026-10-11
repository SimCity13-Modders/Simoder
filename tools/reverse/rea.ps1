param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('setup', 'analyze-javascript', 'analyze-dotnet', 'analyze-native', 'doctor')]
    [string]$Command,
    [Parameter(Position = 1)]
    [string]$Target,
    [string]$Provider = '',
    [switch]$Json,
    [switch]$Latest
)

# /** @summary Runs REA through npx without adding it to the runtime or vendoring its sources. */
$ErrorActionPreference = 'Stop'
$package = if ($Latest) { 'rea-agents@latest' } else { 'rea-agents' }
$arguments = @($package)
switch ($Command) {
    'setup' { $arguments += 'setup' }
    'doctor' { $arguments += 'doctor' }
    'analyze-javascript' {
        if (!$Target) { throw 'analyze-javascript requires an application directory or ASAR path.' }
        $arguments += 'analyze-javascript-application'; $arguments += (Resolve-Path -LiteralPath $Target).Path
    }
    'analyze-dotnet' {
        if (!$Target) { throw 'analyze-dotnet requires an assembly path.' }
        $arguments += 'analyze'; $arguments += (Resolve-Path -LiteralPath $Target).Path
    }
    'analyze-native' {
        if (!$Target) { throw 'analyze-native requires a PE path.' }
        $arguments += 'analyze'; $arguments += (Resolve-Path -LiteralPath $Target).Path
        if ($Provider) { $arguments += '--provider'; $arguments += $Provider }
    }
}
if ($Json) { $arguments += '--json' }
Write-Host "[Simoder] REA command: npx -y $($arguments -join ' ')"
& npx -y @arguments
exit $LASTEXITCODE
