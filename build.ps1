[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_/-]*$')]
    [string]$Example = 'factory_standard',

    [ValidateSet('standard', 'rect', 'round')]
    [string]$HardwareVersion = 'standard',

    [ValidateSet('build', 'flash', 'monitor', 'flash-monitor', 'menuconfig', 'reconfigure', 'fullclean')]
    [string]$Action = 'build',

    [string]$Port = '',

    [switch]$OnlineDependencies
)

$ErrorActionPreference = 'Stop'
$HardwareVersion = $HardwareVersion.ToLowerInvariant()
$Example = $Example.ToLowerInvariant().Replace('\', '/')
$exampleSegments = $Example -split '/'
if ($exampleSegments | Where-Object { $_ -in @('', '.', '..') }) {
    throw "Invalid example path '$Example'. Use a directory below main/examples."
}
$exampleCMake = Join-Path $PSScriptRoot "main/examples/$Example/CMakeLists.txt"
if (-not (Test-Path -LiteralPath $exampleCMake -PathType Leaf)) {
    $exampleRoot = Join-Path $PSScriptRoot 'main/examples'
    $available = Get-ChildItem -LiteralPath $exampleRoot -Directory -Recurse |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'CMakeLists.txt') } |
        ForEach-Object {
            $_.FullName.Substring($exampleRoot.Length + 1).Replace('\', '/')
        }
    throw "Unknown example '$Example'. Available examples: $($available -join ', ')"
}

$buildDirectory = Join-Path $PSScriptRoot "build/$HardwareVersion-$($Example.Replace('/', '-'))"
$buildAlreadyConfigured = Test-Path -LiteralPath (Join-Path $buildDirectory 'CMakeCache.txt') -PathType Leaf

# Use the local managed components when they are available. A fresh checkout
# may not contain them yet; allow the ESP-IDF component manager to fetch the
# locked dependencies during the first configure instead of passing missing
# directories to COMPONENT_DIRS.
$managedComponentNames = @('jgromes__radiolib', 'lvgl__lvgl')
$managedComponentRoot = Join-Path $PSScriptRoot 'managed_components'
$missingManagedComponents = @()
if ($Example -ne 'lcd/ili9882') {
    $missingManagedComponents = @($managedComponentNames | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $managedComponentRoot $_) -PathType Container)
    })
}
# A new variant needs the component manager to resolve registry package names
# (for example, `espressif/esp_audio_codec` -> `esp_audio_codec`). Once CMake
# has configured the variant, keep the existing offline behavior for repeat
# builds unless the caller explicitly requests an online refresh.
if ($OnlineDependencies -or -not $buildAlreadyConfigured) {
    $env:IDF_COMPONENT_MANAGER = '1'
} elseif ($missingManagedComponents.Count -gt 0) {
    Write-Warning ((
        "Managed components are missing ({0}); enabling the ESP-IDF component " +
        "manager for this configure. Dependencies are locked by dependencies.lock."
    ) -f ($missingManagedComponents -join ', '))
    $env:IDF_COMPONENT_MANAGER = '1'
} else {
    $env:IDF_COMPONENT_MANAGER = '0'
}

if (-not $env:IDF_PATH) {
    throw 'IDF_PATH is not set. Run this script from an exported ESP-IDF shell.'
}
$idfScript = Join-Path $env:IDF_PATH 'tools/idf.py'
if (-not (Test-Path -LiteralPath $idfScript -PathType Leaf)) {
    throw "idf.py was not found at '$idfScript'."
}

$pythonExecutable = $null
$pythonCandidates = @()
if ($env:IDF_PYTHON_ENV_PATH) {
    $pythonCandidates += Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts/python.exe'
}
if ($env:IDF_TOOLS_PATH) {
    $pythonCandidates += Get-ChildItem -LiteralPath (Join-Path $env:IDF_TOOLS_PATH 'python_env') -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'Scripts/python.exe' }
}
$pythonCommand = Get-Command python -ErrorAction SilentlyContinue
if ($null -ne $pythonCommand) {
    $pythonCandidates += $pythonCommand.Source
}
foreach ($candidate in ($pythonCandidates | Select-Object -Unique)) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        & $candidate -c 'import esp_idf_monitor' 2>$null
        if ($LASTEXITCODE -eq 0) {
            $pythonExecutable = $candidate
            break
        }
    }
}
if (-not $pythonExecutable) {
    throw 'An ESP-IDF Python environment was not found. Run export.ps1 from an ESP-IDF terminal.'
}
$env:IDF_PYTHON_ENV_PATH = Split-Path (Split-Path $pythonExecutable -Parent) -Parent

$idfArguments = @(
    '-B', $buildDirectory,
    '-D', "T_PANEL_P4_HARDWARE_VERSION=$HardwareVersion",
    '-D', "T_PANEL_P4_EXAMPLE=$Example"
)
$ninjaCommand = Get-Command ninja -ErrorAction SilentlyContinue
if ($null -ne $ninjaCommand) {
    $idfArguments += @('-D', "CMAKE_MAKE_PROGRAM=$($ninjaCommand.Source)")
}
if ($Port) {
    $idfArguments += @('-p', $Port)
}

if ($Action -eq 'flash-monitor') {
    $idfArguments += @('flash', 'monitor')
} else {
    $idfArguments += $Action
}

Write-Host "Hardware:  $HardwareVersion"
Write-Host "Example:   $Example"
Write-Host "Build dir: $buildDirectory"

Push-Location $PSScriptRoot
try {
    & $pythonExecutable $idfScript @idfArguments
    if ($LASTEXITCODE -ne 0) {
        throw "idf.py failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

