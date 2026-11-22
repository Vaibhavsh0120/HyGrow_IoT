Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Checked([string]$Command, [string[]]$Arguments) {
    if (-not (Get-Command $Command -ErrorAction SilentlyContinue)) { throw "Missing tool: $Command. Run -Action Doctor for prerequisites." }
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Command failed (exit $LASTEXITCODE)." }
}

function Invoke-InDirectory([string]$Directory, [scriptblock]$Operation) {
    Push-Location -LiteralPath $Directory
    try { & $Operation } finally { Pop-Location }
}

function Find-PlatformIO {
    foreach ($name in @('pio','platformio')) {
        $command = Get-Command $name -ErrorAction SilentlyContinue
        if ($command) { return $command.Source }
    }
    $candidate = Join-Path ([Environment]::GetFolderPath('UserProfile')) '.platformio/penv/Scripts/platformio.exe'
    if (Test-Path -LiteralPath $candidate) { return $candidate }
    throw 'PlatformIO missing. Install the PlatformIO IDE extension or python -m pip install platformio==6.1.18.'
}

function Show-Prerequisites {
    Write-Host ('PowerShell '+$PSVersionTable.PSVersion)
    foreach ($name in @('git','python','node','npm','flutter','dart','java','docker','age','age-keygen','gh','g++')) {
        $command = Get-Command $name -ErrorAction SilentlyContinue
        if ($command) { Write-Host ('[FOUND] '+$name+' '+$command.Source) }
        else { Write-Host ('[MISSING] '+$name) -ForegroundColor Yellow }
    }
    try { Write-Host ('[FOUND] PlatformIO '+(Find-PlatformIO)) } catch { Write-Host $_.Exception.Message }
    $wrapper = Get-Command agel -ErrorAction SilentlyContinue
    if ($wrapper) { Write-Host ('agel detected as '+$wrapper.CommandType+'; manifest operations use official age with explicit paths.') }
    Write-Host 'Android: flutter doctor -v. iOS device builds run on macOS CI. Docker Desktop must be running for the local stack.'
}

function Initialize-LocalEnvironment([string]$Root) {
    $destination = Join-Path $Root '.env'
    if ((Test-Path -LiteralPath $destination) -and ((Get-Item -LiteralPath $destination -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Refusing linked local environment file.' }
    $contents = if (Test-Path -LiteralPath $destination) { [IO.File]::ReadAllText($destination) } else { '' }
    $changed = $false
    foreach ($name in @('LOCAL_DATABASE_PASSWORD','LOCAL_REDIS_PASSWORD','LOCAL_AUTH_OTP_PEPPER','LOCAL_AUTH_ENCRYPTION_KEY')) {
        if ($contents -match ('(?m)^'+$name+'=')) { continue }
        $bytes = New-Object byte[] 32
        $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
        try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
        $password = [BitConverter]::ToString($bytes).Replace('-','').ToLowerInvariant()
        $contents = $contents.TrimEnd()+"`n$name=$password`n"
        $changed = $true
    }
    if (-not $changed) { return }
    [IO.File]::WriteAllText($destination,$contents.TrimStart(),(New-Object Text.UTF8Encoding($false)))
    if ($env:OS -eq 'Windows_NT') {
        $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User
        & icacls $destination '/inheritance:r' '/grant:r' ('*'+$sid.Value+':F') | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Could not restrict local environment permissions.' }
    }
    Write-Host 'Initialized ignored local .env with random development credentials; existing values preserved.'
}

function Invoke-SecretAction([string]$Root,[string]$Operation,[bool]$Unattended,[bool]$Replace) {
    $arguments = @((Join-Path $Root 'tools/project_secrets.py'),$Operation)
    if ($Unattended) { $arguments += '--non-interactive' }
    if ($Replace) { $arguments += '--replace' }
    Invoke-Checked 'python' $arguments
}

function Invoke-LocalCompose([string]$Root,[string[]]$Arguments) {
    if (-not (Test-Path (Join-Path $Root '.env'))) { throw 'Local .env missing. Run -Action Setup or StartLocal first.' }
    Invoke-Checked 'docker' @('info','--format','{{.ServerVersion}}')
    Invoke-Checked 'docker' (@('compose','--project-name','hygrow-local','--env-file',(Join-Path $Root '.env'),'-f',(Join-Path $Root 'infra/compose.yaml')) + $Arguments)
}

function Assert-HyGrowApi([string]$BaseUrl) {
    $parsed = $null
    if (-not [Uri]::TryCreate($BaseUrl,[UriKind]::Absolute,[ref]$parsed) -or $parsed.UserInfo -or $parsed.Query -or $parsed.Fragment -or ($parsed.Scheme -ne 'https' -and -not ($parsed.Scheme -eq 'http' -and $parsed.IsLoopback))) { throw 'API URL must be HTTPS, or HTTP loopback for local development.' }
    $health = Invoke-RestMethod -Uri ($BaseUrl.TrimEnd('/')+'/health/ready') -TimeoutSec 15
    if ($health.service -ne 'hygrow-api' -or $health.status -ne 'ready') { throw 'Endpoint did not report HyGrow API readiness.' }
}

function Invoke-Mobile([string]$Root,[string]$BaseUrl,[string]$Target,[bool]$Build) {
    $directory = Join-Path $Root 'app'
    if (-not (Test-Path (Join-Path $directory 'pubspec.yaml'))) { throw 'Flutter app is not yet implemented in this milestone.' }
    Invoke-InDirectory $directory {
        if ($Build) { Invoke-Checked 'flutter' @('build','apk','--release',('--dart-define=API_BASE_URL='+$BaseUrl)) }
        else {
            $arguments = @('run',('--dart-define=API_BASE_URL='+$BaseUrl))
            if ($Target) { $arguments += @('-d',$Target) }
            Invoke-Checked 'flutter' $arguments
        }
    }
}

function Clear-ProjectCaches([string]$Root) {
    $resolvedRoot = [IO.Path]::GetFullPath($Root).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
    foreach ($relative in @('firmware/.pio/build','app/build','app/.dart_tool')) {
        $target = Join-Path $Root $relative
        if (-not (Test-Path -LiteralPath $target)) { continue }
        $resolved = (Resolve-Path -LiteralPath $target).Path
        $item = Get-Item -LiteralPath $target -Force
        if (-not $resolved.StartsWith($resolvedRoot,[StringComparison]::OrdinalIgnoreCase) -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Refusing cleanup outside project or through a linked directory.' }
        # Reject linked ancestors as well, before a recursive removal.
        $parent = $item.Parent
        while ($parent -and $parent.FullName.StartsWith($resolvedRoot,[StringComparison]::OrdinalIgnoreCase)) {
            if ($parent.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked cache ancestor; cleanup refused.' }
            $parent = $parent.Parent
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force
        Write-Host ('Removed cache '+$relative)
    }
}

function Invoke-ApiTests([string]$Root) {
    $composeArgs = @('compose','-p','hygrow-tests','-f',(Join-Path $Root 'infra/compose.test.yaml'))
    $oldUrl = $env:HYGROW_TEST_DATABASE_URL
    $oldRedis = $env:HYGROW_TEST_REDIS_URL
    $oldMailpit = $env:HYGROW_TEST_MAILPIT_URL
    try {
        Invoke-Checked 'docker' ($composeArgs + @('up','-d','--wait','--wait-timeout','90'))
        $env:HYGROW_TEST_DATABASE_URL = 'postgres://hygrow_test:local-disposable-tests-only@127.0.0.1:5549/hygrow_test'
        $env:HYGROW_TEST_REDIS_URL = 'redis://127.0.0.1:6389'
        $env:HYGROW_TEST_MAILPIT_URL = 'http://127.0.0.1:8026'
        Invoke-Checked 'npm' @('--prefix',(Join-Path $Root 'services/api'),'test')
    } finally {
        $env:HYGROW_TEST_DATABASE_URL = $oldUrl
        $env:HYGROW_TEST_REDIS_URL = $oldRedis
        $env:HYGROW_TEST_MAILPIT_URL = $oldMailpit
        # This project contains only the disposable tmpfs test database.
        Invoke-Checked 'docker' ($composeArgs + @('down'))
    }
}

Export-ModuleMember -Function *
