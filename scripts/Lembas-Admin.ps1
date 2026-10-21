<#
  Lembas for Windows - the part that needs admin rights. Started by Install-Lembas.ps1 and
  Uninstall-Lembas.ps1 (one permission prompt each); not meant to be run by hand.

  Install:   installs or upgrades Windhawk to 2.0 (silent), installs the Lembas mods with its
             command line, writes the current look's styles, and registers the "Lembas Sun" task.
  Uninstall: removes the task and the Lembas mods, and Windhawk itself if Lembas installed it and
             nothing else uses it.
#>
param(
    [ValidateSet('Install', 'Uninstall')][string]$Mode = 'Install',
    [string]$User,
    [string]$Dest,
    [switch]$Legacy     # upgrading from Lembas 1 (which used WinDynamicDesktop and Lively)
)

$ErrorActionPreference = 'Continue'
$log = Join-Path $Dest 'install.log'
function Log($text) { $line = "[{0:HH:mm:ss}] {1}" -f (Get-Date), $text; Write-Host "  $text" -ForegroundColor Gray; Add-Content -Path $log -Value $line -ErrorAction SilentlyContinue }
$stateFile = Join-Path $Dest 'state.json'
$state = if (Test-Path $stateFile) { Get-Content $stateFile -Raw | ConvertFrom-Json } else { [pscustomobject]@{} }
function Save-State { $state | ConvertTo-Json | Set-Content -Path $stateFile -Encoding UTF8 }
function Set-StateValue($name, $value) { $state | Add-Member -NotePropertyName $name -NotePropertyValue $value -Force; Save-State }

$whDir = Join-Path $env:ProgramFiles 'Windhawk'
$cli = Join-Path $whDir 'windhawk-cli.exe'
$taskPath = '\Lembas\'
$taskName = 'Lembas Sun'
$x64 = $env:PROCESSOR_ARCHITECTURE -eq 'AMD64' -and -not $env:PROCESSOR_ARCHITEW6432 -or $env:PROCESSOR_ARCHITEW6432 -eq 'AMD64'
$arm = $env:PROCESSOR_ARCHITECTURE -eq 'ARM64' -or $env:PROCESSOR_ARCHITEW6432 -eq 'ARM64'

function Windhawk-Major {
    $exe = Join-Path $whDir 'windhawk.exe'
    if (-not (Test-Path $exe)) { return 0 }
    try { return [int]((Get-Item $exe).VersionInfo.ProductVersion -split '[.\-]')[0] } catch { return 1 }
}
function Installed-Mods {
    if (-not (Test-Path $cli)) { return @() }
    try {
        $j = ((& $cli --json mod list 2>$null) -join "`n") | ConvertFrom-Json
        return @($j.data.mods | ForEach-Object { $_.id })
    } catch {
        # plain list: one mod per line, its id first
        return @(& $cli mod list 2>$null | ForEach-Object { ($_ -split '\s+')[0] } | Where-Object { $_ })
    }
}
function Restart-Explorer {
    Get-Process explorer -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 2
    $local = Split-Path $Dest   # ...\AppData\Local
    Get-ChildItem (Join-Path $local 'Microsoft\Windows\Explorer') -Filter 'iconcache_*.db' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $local 'IconCache.db') -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process explorer.exe }
}

Write-Host ""
Write-Host "  LEMBAS - $Mode (admin part)" -ForegroundColor Yellow
Write-Host ""

if ($Mode -eq 'Install') {
    # ---------------------------------------------------------- Windhawk 2.0 (it has the command line Lembas uses)
    # Install whenever the command line is missing: a half-removed Windhawk can leave windhawk.exe behind
    $registered = [bool](Get-ItemProperty 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*', 'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*' -ErrorAction SilentlyContinue |
                         Where-Object { $_.DisplayName -like 'Windhawk*' })
    if (-not (Test-Path $cli)) {
        $major = Windhawk-Major
        if ($major -eq 0 -or -not $registered) { Set-StateValue 'windhawkByLembas' $true }   # nothing (or only leftovers) was there
        elseif ($major -lt 2 -and $Legacy) { Set-StateValue 'windhawkByLembas' $true }       # Lembas 1 installed it
        Log "Installing Windhawk 2.0 (silently)..."
        try {
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
            $rel = Invoke-RestMethod 'https://api.github.com/repos/ramensoftware/windhawk/releases?per_page=30' -Headers @{ 'User-Agent' = 'Lembas' }
            $pick = $rel | Where-Object { $_.tag_name -match '^v?2\.' -and ($_.assets | Where-Object name -eq 'windhawk_setup.exe') } | Select-Object -First 1
            if (-not $pick) { throw "no Windhawk 2 release found" }
            $url = ($pick.assets | Where-Object name -eq 'windhawk_setup.exe').browser_download_url
            $setup = Join-Path $env:TEMP 'windhawk_setup.exe'
            Invoke-WebRequest $url -OutFile $setup -UseBasicParsing -Headers @{ 'User-Agent' = 'Lembas' }
            Log "Windhawk $($pick.tag_name)"
            $p = Start-Process $setup -ArgumentList '/S' -Wait -PassThru
            Remove-Item $setup -Force -ErrorAction SilentlyContinue
            for ($i = 0; $i -lt 30 -and -not (Test-Path $cli); $i++) { Start-Sleep -Seconds 2 }
        } catch { Log "Couldn't install Windhawk: $_" }
    }
    if (-not (Test-Path $cli)) {
        Log "Windhawk's command line isn't available, so the taskbar, Start and Explorer styles were skipped."
    } else {
        # ------------------------------------------------------ the mods
        $mods = Get-Content (Join-Path $Dest 'Windhawk\mods.json') -Raw | ConvertFrom-Json
        $have = Installed-Mods
        foreach ($old in $mods.retire) { if ($have -contains $old) { Log "Removing $old (no longer used)"; & $cli --yes --quiet mod remove $old | Out-Null } }
        foreach ($m in $mods.install) {
            $wanted = $m.arch -eq 'any' -or ($m.arch -eq 'x64' -and -not $arm)
            if (-not $wanted) {
                if ($have -contains $m.mod) { Log "Removing $($m.name) (it can't work on this PC's processor)"; & $cli --yes --quiet mod remove $m.mod | Out-Null }
                continue
            }
            if ($have -contains $m.mod) { & $cli --quiet mod enable $m.mod | Out-Null; continue }
            Log "Installing $($m.name)"
            & $cli --yes --quiet mod install $m.mod | Out-Null
            if ($LASTEXITCODE -ne 0) { & $cli --yes --quiet mod install $m.mod --no-precompiled | Out-Null }
            if ($LASTEXITCODE -ne 0) { Log "  couldn't install $($m.name)" }
        }
        Set-StateValue 'mods' @($mods.install | ForEach-Object { $_.mod })
    }

    # ---------------------------------------------------------- the sun task
    Log "Registering the Lembas Sun task"
    $sun = Join-Path $Dest 'Tools\LembasSun.ps1'
    $conhost = Join-Path $env:SystemRoot 'System32\conhost.exe'
    $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $action = New-ScheduledTaskAction -Execute $conhost -Argument "--headless `"$ps`" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$sun`""
    $triggers = @(
        (New-ScheduledTaskTrigger -AtLogOn -User $User),
        (New-ScheduledTaskTrigger -Once -At (Get-Date).Date -RepetitionInterval (New-TimeSpan -Minutes 10) -RepetitionDuration (New-TimeSpan -Days 3650))
    )
    $unlockClass = Get-CimClass -Namespace Root/Microsoft/Windows/TaskScheduler -ClassName MSFT_TaskSessionStateChangeTrigger
    $unlock = New-CimInstance -CimClass $unlockClass -ClientOnly -Property @{ StateChange = 8; UserId = $User; Enabled = $true }
    $triggers += $unlock
    $principal = New-ScheduledTaskPrincipal -UserId $User -LogonType Interactive -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable `
        -ExecutionTimeLimit (New-TimeSpan -Minutes 20) -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskPath $taskPath -TaskName $taskName -Action $action -Trigger $triggers -Principal $principal `
        -Settings $settings -Description 'Lembas: switches Windows between Shire Morning, Red Book, Starlit Night and Elven Silver with the sun.' -Force | Out-Null

    # ---------------------------------------------------------- apply the look for right now, then refresh Explorer
    Log "Applying the look for right now"
    & $sun -Force
    Restart-Explorer
    Log "Done."
    exit 0
}

if ($Mode -eq 'Uninstall') {
    Unregister-ScheduledTask -TaskPath $taskPath -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    if (Test-Path $cli) {
        $mods = Get-Content (Join-Path $Dest 'Windhawk\mods.json') -Raw -ErrorAction SilentlyContinue | ConvertFrom-Json
        $ours = @($mods.install | ForEach-Object { $_.mod }) + @($mods.retire)
        $have = Installed-Mods
        foreach ($m in $ours) { if ($have -contains $m) { Log "Removing $m"; & $cli --yes --quiet mod remove $m | Out-Null } }
        $left = Installed-Mods | Where-Object { $_ -notin $ours }
        if ($state.windhawkByLembas -and -not $left) {
            $un = Join-Path $whDir 'uninstall.exe'
            if (Test-Path $un) {
                Log "Removing Windhawk (Lembas installed it)"
                # Stop it first so nothing stays locked, run the uninstaller in place so we really wait
                # for it, then clear anything left behind (a leftover windhawk.exe fooled the next install)
                Get-Process windhawk* -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
                Get-Service -Name 'Windhawk*' -ErrorAction SilentlyContinue | Stop-Service -Force -ErrorAction SilentlyContinue
                Start-Process $un -ArgumentList @('/S', "_?=$whDir") -Wait
                Start-Sleep -Seconds 1
                Remove-Item $whDir -Recurse -Force -ErrorAction SilentlyContinue
            }
        } elseif ($left) {
            Log "Keeping Windhawk: you have other mods in it."
        }
    }
    Restart-Explorer
    exit 0
}
