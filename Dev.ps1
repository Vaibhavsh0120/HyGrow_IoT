#requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('Menu','Help','Doctor','Diagnostics','Setup','Encrypt','Decrypt','StartLocal','StopLocal','Status','Test','BuildFirmware','BuildAndroid','RunMobileLive','RunWebLive','CI','Health','Clean')]
    [string]$Action = 'Menu',
    [switch]$Help,
    [switch]$NonInteractive,
    [switch]$SkipSecrets,
    [switch]$SkipContainers,
    [switch]$ReplaceSecrets,
    [string]$Device = '',
    [string]$ApiBaseUrl = 'https://hygrow-india.duckdns.org'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'tools/Developer.psm1') -Force -DisableNameChecking
$projectRoot = $PSScriptRoot

function Show-Help {
    Write-Host @'
HyGrow Developer Manager (PowerShell 5.1 or 7)
Usage: .\Dev.ps1 -Action <name> [-NonInteractive]
Actions: Doctor, Diagnostics, Setup, Encrypt, Decrypt, StartLocal, StopLocal, Status,
         Test, BuildFirmware, BuildAndroid, RunMobileLive, RunWebLive,
         CI, Health, Clean, Help
Setup: -SkipSecrets keeps existing secrets; -SkipContainers omits Docker startup.
Decrypt: hidden terminal identity prompt; -ReplaceSecrets preserves a backup.
StartLocal: isolated local PostgreSQL/API/Redis; data survives StopLocal.
RunMobileLive / BuildAndroid: -Device <flutter device ID>, -ApiBaseUrl <https URL>.
CI shows build links. Successful versioned builds publish signed APKs and unsigned
iOS device-release IPAs with source metadata and checksums in GitHub Releases.
Clean removes only generated build caches; never source, secrets or DB volumes.
'@
}

function Invoke-Action([string]$Selected) {
    switch ($Selected) {
        'Help' { Show-Help }
        'Doctor' { Show-Prerequisites }
        'Diagnostics' { Invoke-Checked 'python' @((Join-Path $projectRoot 'tools/diagnostics.py')) }
        'Encrypt' { Invoke-SecretAction $projectRoot 'encrypt' $NonInteractive $ReplaceSecrets }
        'Decrypt' { Invoke-SecretAction $projectRoot 'decrypt' $NonInteractive $ReplaceSecrets }
        'Setup' {
            Show-Prerequisites
            if (-not $SkipSecrets) {
                $manifest = Get-Content -LiteralPath (Join-Path $projectRoot 'config/secrets.manifest.json') -Raw | ConvertFrom-Json
                $missing = @($manifest.files | Where-Object { $_.required -and -not (Test-Path -LiteralPath (Join-Path $projectRoot $_.plaintext)) })
                if ($missing.Count -gt 0) { Invoke-SecretAction $projectRoot 'decrypt' $NonInteractive $ReplaceSecrets }
                else { Write-Host 'Required local secrets already exist; kept existing files. Use Decrypt explicitly to replace.' }
            }
            Initialize-LocalEnvironment $projectRoot
            Invoke-Checked 'npm' @('ci','--prefix',(Join-Path $projectRoot 'services/api'))
            if (Test-Path (Join-Path $projectRoot 'app/pubspec.yaml')) {
                Invoke-InDirectory (Join-Path $projectRoot 'app') { Invoke-Checked 'flutter' @('pub','get') }
            }
            if (-not $SkipContainers) { Invoke-LocalCompose $projectRoot @('up','-d','--build','--wait','--wait-timeout','120') }
            Invoke-Checked 'python' @((Join-Path $projectRoot 'tools/project_secrets.py'),'check')
        }
        'StartLocal' {
            Initialize-LocalEnvironment $projectRoot
            Invoke-LocalCompose $projectRoot @('up','-d','--build','--wait','--wait-timeout','120')
            Write-Host 'Local API: http://localhost:8088 (isolated development data). Website activates when its milestone is implemented.'
            if ($Device) { Invoke-Mobile $projectRoot 'http://localhost:8088' $Device $false }
        }
        'StopLocal' { Invoke-LocalCompose $projectRoot @('stop'); Write-Host 'Local services stopped. Database volume retained.' }
        'Status' { Invoke-LocalCompose $projectRoot @('ps') }
        'BuildFirmware' { Invoke-InDirectory (Join-Path $projectRoot 'firmware') { Invoke-Checked (Find-PlatformIO) @('run','-e','esp32-s3-n16r8') } }
        'Test' {
            Invoke-InDirectory $projectRoot {
                Invoke-Checked 'python' @('firmware/tools/test-all.py')
                Invoke-Checked 'python' @('-m','unittest','discover','-s','tools/tests','-v')
                if (Test-Path 'services/api/package.json') { Invoke-ApiTests $projectRoot }
                if (Test-Path 'app/pubspec.yaml') {
                    Invoke-InDirectory (Join-Path $projectRoot 'app') {
                        Invoke-Checked 'dart' @('format','--output=none','--set-exit-if-changed','lib','test')
                        Invoke-Checked 'flutter' @('analyze')
                        Invoke-Checked 'flutter' @('test')
                    }
                }
            }
        }
        'Health' { Assert-HyGrowApi $ApiBaseUrl; Write-Host ('Verified HyGrow API readiness: '+$ApiBaseUrl) }
        'RunWebLive' {
            Assert-HyGrowApi $ApiBaseUrl
            Write-Host ('DEVELOPMENT FRONTEND -> LIVE BACKEND: '+$ApiBaseUrl)
            if ($NonInteractive) { Write-Host $ApiBaseUrl } else { Start-Process $ApiBaseUrl }
        }
        'RunMobileLive' {
            Assert-HyGrowApi $ApiBaseUrl
            Write-Host ('DEVELOPMENT FRONTEND -> LIVE BACKEND: '+$ApiBaseUrl)
            Invoke-Mobile $projectRoot $ApiBaseUrl $Device $false
        }
        'BuildAndroid' { Assert-HyGrowApi $ApiBaseUrl; Invoke-Mobile $projectRoot $ApiBaseUrl $Device $true }
        'CI' { Write-Host 'https://github.com/Vaibhavsh0120/HyGrow_IoT/actions'; Write-Host 'Open a successful build -> Artifacts. Verify version, SHA, signing label and checksum.' }
        'Clean' { Clear-ProjectCaches $projectRoot }
        default { throw 'Specify a supported -Action. Use -Help for available actions.' }
    }
}

try {
    if ($Help) { Show-Help; exit 0 }
    if ($Action -ne 'Menu') { Invoke-Action $Action; exit 0 }
    if ($NonInteractive) { throw 'Specify -Action when using -NonInteractive.' }
    $choices = @('RunMobileLive','RunWebLive','StartLocal','Status','Setup','Encrypt','Decrypt','Test','BuildFirmware','BuildAndroid','CI','Health','Clean','StopLocal','Doctor','Diagnostics')
    while ($true) {
        Write-Host "`n================ HyGrow Developer Manager ================" -ForegroundColor Cyan
        for ($index=0; $index -lt $choices.Count; $index++) { Write-Host ('{0,2}  {1}' -f ($index+1),$choices[$index]) }
        Write-Host ' 0  Exit'
        $answer = Read-Host 'Choose an action'
        if ($null -eq $answer -or $answer -eq '0') { break }
        $number = 0
        if ([int]::TryParse($answer,[ref]$number) -and $number -ge 1 -and $number -le $choices.Count) {
            try { Invoke-Action $choices[$number-1]; Write-Host 'Action completed (exit 0).' -ForegroundColor Green }
            catch { Write-Host ('Action failed: '+$_.Exception.Message) -ForegroundColor Red }
        } else { Write-Host 'Enter one of the displayed numbers.' }
    }
    exit 0
} catch {
    [Console]::Error.WriteLine('HyGrow action failed: '+$_.Exception.Message)
    exit 1
}
