[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$Notes,

    [ValidateNotNullOrEmpty()]
    [string]$Server = "139.129.17.67",

    [ValidateNotNullOrEmpty()]
    [string]$User = "root",

    [switch]$DryRun
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

function Write-Step {
    param([string]$Message)
    Write-Host ("`n==> {0}" -f $Message) -ForegroundColor Cyan
}

function Get-SingleDefineString {
    param(
        [string]$HeaderPath,
        [string]$DefineName
    )

    $content = [System.IO.File]::ReadAllText($HeaderPath)
    $escapedName = [System.Text.RegularExpressions.Regex]::Escape($DefineName)
    $pattern = '(?m)^\s*#define\s+' + $escapedName + '\s+"([^"\r\n]+)"\s*(?:(?://.*)|(?:/\*.*\*/))?\s*$'
    $matches = [System.Text.RegularExpressions.Regex]::Matches($content, $pattern)
    if ($matches.Count -ne 1) {
        throw "Expected exactly one string definition for $DefineName in $HeaderPath."
    }
    return $matches[0].Groups[1].Value
}

function Get-ProjectVersion {
    param([string]$CMakePath)

    $content = [System.IO.File]::ReadAllText($CMakePath)
    $pattern = '(?m)^\s*set\s*\(\s*PROJECT_VER\s+"([^"]+)"\s*\)\s*(?:#.*)?$'
    $matches = [System.Text.RegularExpressions.Regex]::Matches($content, $pattern)
    if ($matches.Count -ne 1) {
        throw "Expected exactly one set(PROJECT_VER `"...`") entry in $CMakePath."
    }

    $version = $matches[0].Groups[1].Value
    if ($version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
        throw "PROJECT_VER '$version' is not a strict x.y.z SemVer value."
    }
    return $version
}

function Get-CurrentBranch {
    param([string]$ProjectRoot)

    $output = @(& git -C $ProjectRoot branch --show-current 2>&1)
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0 -or $output.Count -ne 1 -or [string]::IsNullOrWhiteSpace([string]$output[0])) {
        throw "Cannot determine the current Git branch for $ProjectRoot."
    }
    return ([string]$output[0]).Trim()
}

function Get-BranchBinding {
    param([string]$Branch)

    switch ($Branch) {
        "device/001" {
            return [PSCustomObject]@{
                DeviceId = "walkie-01"
                Hardware = "walkie-v1-rev-1"
            }
        }
        "device/002" {
            return [PSCustomObject]@{
                DeviceId = "walkie-02"
                Hardware = "walkie-v1-rev-2"
            }
        }
        default {
            throw "Current branch '$Branch' is not device/001 or device/002; refusing to guess OTA hardware."
        }
    }
}

function Assert-SafeEndpoint {
    param(
        [string]$ServerValue,
        [string]$UserValue
    )

    if ($ServerValue -notmatch '^[A-Za-z0-9][A-Za-z0-9.-]*$' -or
        $ServerValue.Contains('..')) {
        throw "Server must be an IPv4 address or a simple DNS hostname."
    }
    if ($UserValue -notmatch '^[A-Za-z_][A-Za-z0-9._-]*$') {
        throw "User contains unsupported characters."
    }
}

function Invoke-NativeChecked {
    param(
        [string]$Command,
        [object[]]$Arguments,
        [string]$Operation
    )

    & $Command @Arguments
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "$Operation failed with exit code $exitCode."
    }
}

function Invoke-SshPublish {
    param([object[]]$Arguments)

    $savedErrorAction = $ErrorActionPreference
    $output = @()
    $exitCode = -1
    try {
        $ErrorActionPreference = "Continue"
        $output = @(& ssh @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedErrorAction
    }

    foreach ($line in $output) {
        Write-Host $line
    }
    if ($exitCode -ne 0) {
        throw "Server publish command failed with exit code $exitCode."
    }
    return (($output | Out-String).Trim())
}

function Get-BuildDescription {
    param([string]$DescriptionPath)

    if (-not (Test-Path -LiteralPath $DescriptionPath -PathType Leaf)) {
        throw "Build metadata not found: $DescriptionPath"
    }
    try {
        return ([System.IO.File]::ReadAllText($DescriptionPath) | ConvertFrom-Json)
    }
    catch {
        throw "Cannot parse build metadata: $DescriptionPath"
    }
}

function Test-FirmwareHardwareMarker {
    param(
        [string]$FirmwarePath,
        [string]$ExpectedHardware
    )

    $allowedHardware = @("walkie-v1-rev-1", "walkie-v1-rev-2")
    if ($allowedHardware -cnotcontains $ExpectedHardware) {
        throw "Unsupported OTA hardware '$ExpectedHardware'."
    }

    $bytes = [System.IO.File]::ReadAllBytes($FirmwarePath)
    $firmwareText = [System.Text.Encoding]::ASCII.GetString($bytes)
    $hasExpected = $firmwareText.IndexOf($ExpectedHardware, [System.StringComparison]::Ordinal) -ge 0
    foreach ($candidate in $allowedHardware) {
        if ($candidate -cne $ExpectedHardware -and
            $firmwareText.IndexOf($candidate, [System.StringComparison]::Ordinal) -ge 0) {
            return $false
        }
    }
    return $hasExpected
}

try {
    Assert-SafeEndpoint -ServerValue $Server -UserValue $User
    if ($Notes.IndexOf([char]0) -ge 0) {
        throw "Notes must not contain a NUL character."
    }

    $projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
    $cmakePath = Join-Path $projectRoot "CMakeLists.txt"
    $appConfigPath = Join-Path $projectRoot "components\app\inc\app_config.h"
    $firmwarePath = Join-Path $projectRoot "build\walkie-talkiev1.bin"
    $descriptionPath = Join-Path $projectRoot "build\project_description.json"

    $branch = Get-CurrentBranch -ProjectRoot $projectRoot
    $binding = Get-BranchBinding -Branch $branch
    $deviceId = Get-SingleDefineString -HeaderPath $appConfigPath -DefineName "APP_DEVICE_ID"
    $hardware = Get-SingleDefineString -HeaderPath $appConfigPath -DefineName "APP_OTA_HARDWARE"
    $allowedHardware = @("walkie-v1-rev-1", "walkie-v1-rev-2")
    if ($allowedHardware -cnotcontains $hardware) {
        throw "APP_OTA_HARDWARE '$hardware' is not an allowed hardware value."
    }
    if ($deviceId -cne $binding.DeviceId -or $hardware -cne $binding.Hardware) {
        throw ("Branch/source binding mismatch. Branch {0} requires device_id={1}, hardware={2}; source has device_id={3}, hardware={4}." -f
            $branch, $binding.DeviceId, $binding.Hardware, $deviceId, $hardware)
    }

    $version = Get-ProjectVersion -CMakePath $cmakePath
    $remoteFileName = "{0}-{1}.bin" -f $hardware, $version

    Write-Step "Release configuration"
    Write-Host ("Git branch      : {0}" -f $branch)
    Write-Host ("Device ID       : {0} (concrete device identity; independent from hardware)" -f $deviceId)
    Write-Host ("Hardware        : {0}" -f $hardware)
    Write-Host ("Version         : {0}" -f $version)
    Write-Host ("Local bin       : {0}" -f $firmwarePath)
    Write-Host ("Remote filename : {0}" -f $remoteFileName)
    Write-Host ("Notes           : {0}" -f $Notes)
    Write-Host "Notes transport : UTF-8 Base64 (raw notes are not inserted into the remote shell command)"

    if (-not (Test-Path -LiteralPath $firmwarePath -PathType Leaf)) {
        throw "Firmware not found: $firmwarePath. Build it manually in the ESP-IDF environment before publishing."
    }
    $firmware = Get-Item -LiteralPath $firmwarePath
    if ($firmware.Length -le 0) {
        throw "Firmware is empty: $firmwarePath. Build it manually in the ESP-IDF environment before publishing."
    }

    $buildDescription = Get-BuildDescription -DescriptionPath $descriptionPath
    $builtVersion = [string]$buildDescription.project_version
    $builtAppBin = [string]$buildDescription.app_bin
    if ($builtVersion -cne $version -or $builtAppBin -cne "walkie-talkiev1.bin") {
        throw "Build metadata mismatch: PROJECT_VER=$version, built version=$builtVersion, app_bin=$builtAppBin. Build manually in the ESP-IDF environment before publishing."
    }

    $hardwareMarkerFound = Test-FirmwareHardwareMarker -FirmwarePath $firmwarePath -ExpectedHardware $hardware
    Write-Host ("Bin hardware marker detected: {0}" -f $hardwareMarkerFound)
    if (-not $hardwareMarkerFound) {
        throw "Firmware hardware marker mismatch: expected '$hardware'. The bin may be old or built for another revision; rebuild it manually in the ESP-IDF environment."
    }

    $firmwareSize = [Int64]$firmware.Length
    $firmwareSha256 = (Get-FileHash -LiteralPath $firmwarePath -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Step "Validated local firmware"
    Write-Host ("Size            : {0}" -f $firmwareSize)
    Write-Host ("SHA-256         : {0}" -f $firmwareSha256)

    $channel = "stable"
    $deployDir = "/root/wkt-deploy"
    $remoteIncomingPath = "{0}/data/ota/incoming/{1}" -f $deployDir, $remoteFileName
    $containerFilePath = "/app/data/incoming/{0}" -f $remoteFileName
    $sshTarget = "{0}@{1}" -f $User, $Server
    $scpTarget = "{0}:{1}" -f $sshTarget, $remoteIncomingPath

    $notesBytes = [System.Text.Encoding]::UTF8.GetBytes($Notes)
    $notesBase64 = [System.Convert]::ToBase64String($notesBytes)
    $publishScriptTemplate = @'
cd __DEPLOY_DIR__ && NOTES_B64='__NOTES_B64__' && NOTES="$(printf '%s' "$NOTES_B64" | base64 -d)" && docker compose --env-file .env -f compose.yaml exec -T ota python -m app.cli publish --hardware __HARDWARE__ --channel __CHANNEL__ --version __VERSION__ --file __CONTAINER_FILE__ --notes "$NOTES"
'@
    $publishScript = $publishScriptTemplate.Trim()
    $publishScript = $publishScript.Replace('__DEPLOY_DIR__', $deployDir)
    $publishScript = $publishScript.Replace('__NOTES_B64__', $notesBase64)
    $publishScript = $publishScript.Replace('__HARDWARE__', $hardware)
    $publishScript = $publishScript.Replace('__CHANNEL__', $channel)
    $publishScript = $publishScript.Replace('__VERSION__', $version)
    $publishScript = $publishScript.Replace('__CONTAINER_FILE__', $containerFilePath)

    # Encode the complete script so Windows native argument handling cannot strip
    # quotes that protect the decoded Notes value on the remote shell.
    $publishScriptBytes = [System.Text.Encoding]::UTF8.GetBytes($publishScript)
    $publishScriptBase64 = [System.Convert]::ToBase64String($publishScriptBytes)
    $remoteCommand = "printf %s {0} | base64 -d | sh" -f $publishScriptBase64
    $scpArguments = @("-o", "BatchMode=yes", "--", $firmwarePath, $scpTarget)
    $sshArguments = @("-o", "BatchMode=yes", "--", $sshTarget, $remoteCommand)

    if ($DryRun) {
        Write-Step "DryRun remote plan"
        Write-Host ("[DryRun] scp -o BatchMode=yes -- `"{0}`" `"{1}`"" -f $firmwarePath, $scpTarget)
        Write-Host ("[DryRun] ssh -o BatchMode=yes -- `"{0}`" `"{1}`"" -f $sshTarget, $remoteCommand)
        Write-Host "[DryRun] No scp or ssh process was started."
        Write-Host "Server publish result: not executed (DryRun)"
        return
    }

    Write-Step "Uploading firmware"
    Invoke-NativeChecked -Command "scp" -Arguments $scpArguments -Operation "scp upload"

    Write-Step "Publishing OTA release"
    $publishResult = Invoke-SshPublish -Arguments $sshArguments

    Write-Step "OTA publish succeeded"
    Write-Host ("Hardware      : {0}" -f $hardware)
    Write-Host ("Version       : {0}" -f $version)
    Write-Host ("Local SHA-256 : {0}" -f $firmwareSha256)
    Write-Host "Server publish result:"
    Write-Host $publishResult
}
catch {
    Write-Error ("OTA publish failed: {0}" -f $_.Exception.Message) -ErrorAction Continue
    exit 1
}
