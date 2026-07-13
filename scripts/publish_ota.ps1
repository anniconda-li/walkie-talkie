[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$Notes,

    [ValidateNotNullOrEmpty()]
    [string]$Server = "139.129.17.67",

    [ValidateNotNullOrEmpty()]
    [string]$User = "root",

    [switch]$SkipBuild,

    [switch]$DryRun
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

function Write-Step {
    param([string]$Message)
    Write-Host ("`n==> {0}" -f $Message) -ForegroundColor Cyan
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
    $semVerPattern = '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'
    if ($version -notmatch $semVerPattern) {
        throw "PROJECT_VER '$version' is not a strict x.y.z SemVer value."
    }
    return $version
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
    param(
        [object[]]$Arguments
    )

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
        return $null
    }
    try {
        return ([System.IO.File]::ReadAllText($DescriptionPath) | ConvertFrom-Json)
    }
    catch {
        throw "Cannot parse build metadata: $DescriptionPath"
    }
}

try {
    Assert-SafeEndpoint -ServerValue $Server -UserValue $User
    if ($Notes.IndexOf([char]0) -ge 0) {
        throw "Notes must not contain a NUL character."
    }

    $projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
    $cmakePath = Join-Path $projectRoot "CMakeLists.txt"
    $firmwarePath = Join-Path $projectRoot "build\walkie-talkiev1.bin"
    $descriptionPath = Join-Path $projectRoot "build\project_description.json"

    $hardware = "walkie-v1"
    $channel = "stable"
    $deployDir = "/root/wkt-deploy"
    $version = Get-ProjectVersion -CMakePath $cmakePath
    $remoteFileName = "{0}-{1}.bin" -f $hardware, $version
    $remoteIncomingPath = "{0}/data/ota/incoming/{1}" -f $deployDir, $remoteFileName
    $containerFilePath = "/app/data/incoming/{0}" -f $remoteFileName
    $sshTarget = "{0}@{1}" -f $User, $Server
    $scpTarget = "{0}:{1}" -f $sshTarget, $remoteIncomingPath

    $notesBytes = [System.Text.Encoding]::UTF8.GetBytes($Notes)
    $notesBase64 = [System.Convert]::ToBase64String($notesBytes)
    $remoteTemplate = @'
cd __DEPLOY_DIR__ && NOTES_B64='__NOTES_B64__' && NOTES="$(printf '%s' "$NOTES_B64" | base64 -d)" && docker compose --env-file .env -f compose.yaml exec -T ota python -m app.cli publish --hardware __HARDWARE__ --channel __CHANNEL__ --version __VERSION__ --file __CONTAINER_FILE__ --notes "$NOTES"
'@
    $remoteCommand = $remoteTemplate.Trim()
    $remoteCommand = $remoteCommand.Replace('__DEPLOY_DIR__', $deployDir)
    $remoteCommand = $remoteCommand.Replace('__NOTES_B64__', $notesBase64)
    $remoteCommand = $remoteCommand.Replace('__HARDWARE__', $hardware)
    $remoteCommand = $remoteCommand.Replace('__CHANNEL__', $channel)
    $remoteCommand = $remoteCommand.Replace('__VERSION__', $version)
    $remoteCommand = $remoteCommand.Replace('__CONTAINER_FILE__', $containerFilePath)

    Write-Step "Release configuration"
    Write-Host ("Project root : {0}" -f $projectRoot)
    Write-Host ("Hardware     : {0}" -f $hardware)
    Write-Host ("Channel      : {0}" -f $channel)
    Write-Host ("Version      : {0}" -f $version)
    Write-Host ("Server       : {0}" -f $sshTarget)
    Write-Host ("Remote file  : {0}" -f $remoteIncomingPath)
    Write-Host ("Notes        : {0}" -f $Notes)
    Write-Host "Notes transport: UTF-8 Base64 (raw notes are not inserted into the remote shell command)"

    if ($SkipBuild) {
        Write-Step "Build skipped by -SkipBuild"
    }
    elseif ($DryRun) {
        Write-Step "DryRun build plan"
        Write-Host ("[DryRun] cd {0}" -f $projectRoot)
        Write-Host "[DryRun] idf.py build"
    }
    else {
        Write-Step "Building firmware"
        Push-Location $projectRoot
        try {
            Invoke-NativeChecked -Command "idf.py" -Arguments @("build") -Operation "idf.py build"
        }
        finally {
            Pop-Location
        }
    }

    $firmwareExists = Test-Path -LiteralPath $firmwarePath -PathType Leaf
    if (-not $firmwareExists) {
        if ($DryRun -and -not $SkipBuild) {
            Write-Warning "Firmware does not exist yet because DryRun did not execute the planned build."
        }
        else {
            throw "Firmware not found: $firmwarePath"
        }
    }

    $buildDescription = Get-BuildDescription -DescriptionPath $descriptionPath
    if ($null -ne $buildDescription) {
        $builtVersion = [string]$buildDescription.project_version
        $builtAppBin = [string]$buildDescription.app_bin
        $metadataMatches = ($builtVersion -eq $version -and $builtAppBin -eq "walkie-talkiev1.bin")
        if (-not $metadataMatches) {
            $message = "Build metadata mismatch: PROJECT_VER=$version, built version=$builtVersion, app_bin=$builtAppBin."
            if ($DryRun) {
                Write-Warning $message
            }
            else {
                throw $message
            }
        }
    }
    elseif (-not $DryRun) {
        throw "Build metadata not found: $descriptionPath"
    }

    $firmwareSize = $null
    $firmwareSha256 = $null
    if ($firmwareExists) {
        $firmware = Get-Item -LiteralPath $firmwarePath
        if ($firmware.Length -le 0) {
            throw "Firmware is empty: $firmwarePath"
        }
        $firmwareSize = [Int64]$firmware.Length
        $firmwareSha256 = (Get-FileHash -LiteralPath $firmwarePath -Algorithm SHA256).Hash.ToLowerInvariant()
        Write-Step "Local firmware"
        Write-Host ("Path          : {0}" -f $firmwarePath)
        Write-Host ("Size          : {0}" -f $firmwareSize)
        Write-Host ("SHA-256       : {0}" -f $firmwareSha256)
    }

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
