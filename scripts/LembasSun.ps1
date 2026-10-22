<#
  Lembas Sun - puts Windows in the right Lembas look for the time of day.

  Run by the "Lembas Sun" scheduled task at sign-in, on unlock and every 10 minutes. It does
  nothing unless the look should change, so it costs a fraction of a second. On a change it sets
  the wallpaper, lock screen, light/dark mode, accent colour and the Windhawk styles together.

    Shire Morning   sunrise -> midday    light   sage and green ink
    Red Book        midday  -> sunset    light   parchment and red ink
    Starlit Night   sunset  -> 11pm      dark    midnight and gold   (and 4am -> sunrise)
    Elven Silver    11pm    -> 4am       dark    black-blue and silver
#>
param(
    [ValidateSet('', 'shire', 'redbook', 'starlit', 'elven')][string]$Look = '',
    [switch]$Force,
    [switch]$NoWindhawk
)

$ErrorActionPreference = 'Continue'
$Dest = Split-Path -Parent $PSScriptRoot
$logFile = Join-Path $Dest 'sun.log'
function Log($text) {
    if ((Test-Path $logFile) -and (Get-Item $logFile).Length -gt 200KB) { Remove-Item $logFile -Force }
    Add-Content -Path $logFile -Value ("[{0:yyyy-MM-dd HH:mm}] {1}" -f (Get-Date), $text) -ErrorAction SilentlyContinue
}

$config = Get-Content (Join-Path $Dest 'lembas.json') -Raw | ConvertFrom-Json
$looks = Get-Content (Join-Path $Dest 'looks.json') -Raw | ConvertFrom-Json
$stateFile = Join-Path $Dest 'sun-state.json'
$state = if (Test-Path $stateFile) { Get-Content $stateFile -Raw | ConvertFrom-Json } else { [pscustomobject]@{ phase = '' } }

# ------------------------------------------------------------------ which look
function Get-SunTimes([datetime]$day, [double]$lat, [double]$lon) {
    $g = 2 * [math]::PI / 365 * ($day.DayOfYear - 1)
    $eot = 229.18 * (0.000075 + 0.001868 * [math]::Cos($g) - 0.032077 * [math]::Sin($g) - 0.014615 * [math]::Cos(2 * $g) - 0.040849 * [math]::Sin(2 * $g))
    $dec = 0.006918 - 0.399912 * [math]::Cos($g) + 0.070257 * [math]::Sin($g) - 0.006758 * [math]::Cos(2 * $g) + 0.000907 * [math]::Sin(2 * $g) - 0.002697 * [math]::Cos(3 * $g) + 0.00148 * [math]::Sin(3 * $g)
    $offset = [TimeZoneInfo]::Local.GetUtcOffset($day.Date.AddHours(12)).TotalMinutes
    $noon = 720 - 4 * $lon - $eot + $offset
    $latR = $lat * [math]::PI / 180
    $cosHa = [math]::Cos(90.833 * [math]::PI / 180) / ([math]::Cos($latR) * [math]::Cos($dec)) - [math]::Tan($latR) * [math]::Tan($dec)
    $ha = if ($cosHa -ge 1) { 0 } elseif ($cosHa -le -1) { 180 } else { [math]::Acos($cosHa) * 180 / [math]::PI }
    # whole minutes, as the phone and the watch use
    @{ rise = [math]::Floor($noon - 4 * $ha); noon = [math]::Floor($noon); set = [math]::Floor($noon + 4 * $ha) }
}
$phase = $Look
$preview = Join-Path $Dest 'preview.json'
if (-not $phase -and (Test-Path $preview)) {
    $pv = Get-Content $preview -Raw | ConvertFrom-Json
    if ([datetime]$pv.until -gt (Get-Date)) { $phase = $pv.look } else { Remove-Item $preview -Force }
}
if (-not $phase -and $config.look -and $config.look -ne 'adaptive') { $phase = $config.look }
# Where the sun is worked out from: your own coordinates if you gave them at install, otherwise the
# main city of the current time zone (the same cities the phone and the watch use), so Lembas
# follows you when the time zone changes.
$zoneCities = @{
    'AUS Eastern Standard Time' = @(-33.87, 151.21); 'E. Australia Standard Time' = @(-27.47, 153.03)
    'Cen. Australia Standard Time' = @(-34.93, 138.60); 'AUS Central Standard Time' = @(-12.46, 130.84)
    'W. Australia Standard Time' = @(-31.95, 115.86); 'Tasmania Standard Time' = @(-42.88, 147.33)
    'Lord Howe Standard Time' = @(-31.55, 159.08); 'New Zealand Standard Time' = @(-36.85, 174.76)
    'Fiji Standard Time' = @(-18.14, 178.44); 'Tokyo Standard Time' = @(35.68, 139.69)
    'Korea Standard Time' = @(37.57, 126.98); 'China Standard Time' = @(31.23, 121.47)
    'Singapore Standard Time' = @(1.35, 103.82); 'India Standard Time' = @(19.08, 72.88)
    'Arabian Standard Time' = @(25.20, 55.27); 'SE Asia Standard Time' = @(13.76, 100.50)
    'GMT Standard Time' = @(51.51, -0.13); 'W. Europe Standard Time' = @(52.52, 13.40)
    'Romance Standard Time' = @(48.86, 2.35); 'GTB Standard Time' = @(37.98, 23.73)
    'Russian Standard Time' = @(55.76, 37.62); 'South Africa Standard Time' = @(-26.20, 28.05)
    'Egypt Standard Time' = @(30.04, 31.24); 'W. Central Africa Standard Time' = @(6.52, 3.38)
    'E. Africa Standard Time' = @(-1.29, 36.82); 'Eastern Standard Time' = @(40.71, -74.01)
    'Central Standard Time' = @(41.88, -87.63); 'Mountain Standard Time' = @(39.74, -104.99)
    'US Mountain Standard Time' = @(33.45, -112.07); 'Pacific Standard Time' = @(34.05, -118.24)
    'Central Standard Time (Mexico)' = @(19.43, -99.13); 'E. South America Standard Time' = @(-23.55, -46.63)
    'Argentina Standard Time' = @(-34.60, -58.38); 'Pacific SA Standard Time' = @(-33.45, -70.67)
    'SA Pacific Standard Time' = @(4.71, -74.07); 'Hawaiian Standard Time' = @(21.31, -157.86)
    'Alaskan Standard Time' = @(61.22, -149.90)
}
if (-not ($config.PSObject.Properties['locationSource'] -and $config.locationSource -eq 'manual')) {
    $c = $zoneCities[[TimeZoneInfo]::Local.Id]
    if (-not $c) { $c = @(0, [TimeZoneInfo]::Local.GetUtcOffset((Get-Date).Date.AddHours(12)).TotalMinutes / 4) }
    if ([double]$config.latitude -ne $c[0] -or [double]$config.longitude -ne $c[1]) {
        $config | Add-Member -NotePropertyName latitude -NotePropertyValue $c[0] -Force
        $config | Add-Member -NotePropertyName longitude -NotePropertyValue $c[1] -Force
        $config | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $Dest 'lembas.json') -Encoding UTF8
    }
}
if (-not $phase) {
    $now = Get-Date
    $sun = Get-SunTimes $now ([double]$config.latitude) ([double]$config.longitude)
    $m = $now.Hour * 60 + $now.Minute
    $phase = if ($m -ge $sun.rise -and $m -lt $sun.noon) { 'shire' }
        elseif ($m -ge $sun.noon -and $m -lt $sun.set) { 'redbook' }
        elseif ($m -ge 23 * 60 -or $m -lt 4 * 60) { 'elven' }
        else { 'starlit' }
}
# ------------------------------------------------------------------ once a day: quietly update Lembas itself
function Test-Update {
    if ($config.PSObject.Properties['autoUpdate'] -and -not $config.autoUpdate) { return }
    $checkFile = Join-Path $Dest 'update-check.txt'
    if ((Test-Path $checkFile) -and ((Get-Date) - (Get-Item $checkFile).LastWriteTime).TotalHours -lt 24) { return }
    Set-Content -Path $checkFile -Value (Get-Date).ToString('s')
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $rel = Invoke-RestMethod $config.releaseApi -Headers @{ 'User-Agent' = 'Lembas' } -TimeoutSec 20
        $latest = $rel.tag_name -replace '^v', ''
        if (-not $latest -or $latest -eq $config.version) { return }
        $asset = $rel.assets | Where-Object name -eq 'Lembas-for-Windows.zip' | Select-Object -First 1
        if (-not $asset) { return }
        Log "Updating Lembas $($config.version) -> $latest"
        $work = Join-Path $env:TEMP 'lembas-autoupdate'
        Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Path $work -Force | Out-Null
        Invoke-WebRequest $asset.browser_download_url -OutFile (Join-Path $work 'lembas.zip') -UseBasicParsing -TimeoutSec 300
        Expand-Archive (Join-Path $work 'lembas.zip') -DestinationPath $work -Force
        $installer = Get-ChildItem $work -Recurse -Filter 'Install-Lembas.ps1' | Select-Object -First 1
        if ($installer) {
            # Runs on its own, so this task can finish; the installer re-registers this task when done
            Start-Process (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') -WindowStyle Hidden `
                -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden', '-File', "`"$($installer.FullName)`"", '-NoPause', '-Quiet')
            exit 0
        }
    } catch { Log "  update check: $($_.Exception.Message)" }
}
if (-not $Look -and -not (Test-Path (Join-Path $Dest 'preview.json')) -and $config.PSObject.Properties['releaseApi']) { Test-Update }

# Keep Lembas Desktop running (if it's switched on): it starts at sign-in, this covers a crash.
# Started through Explorer so it runs as you, not as admin.
$desktopExe = Join-Path $Dest 'Living\LembasDesktop.exe'
if (-not ($config.PSObject.Properties['livingDesktop'] -and -not $config.livingDesktop) -and (Test-Path $desktopExe) -and
    -not (Get-Process LembasDesktop -ErrorAction SilentlyContinue) -and (Get-Process explorer -ErrorAction SilentlyContinue)) {
    Log "Starting Lembas Desktop"
    Start-Process explorer.exe -ArgumentList "`"$desktopExe`""
}

$l = $looks.$phase
$wallpaper = Join-Path $Dest ("Wallpapers\" + $l.wallpaper)
$currentWall = (Get-ItemProperty 'HKCU:\Control Panel\Desktop' -ErrorAction SilentlyContinue).WallPaper
if (-not $Force -and $state.phase -eq $phase -and $currentWall -eq $wallpaper) { exit 0 }

Log "Switching to $($l.name)"
Add-Type -Namespace LembasSun -Name Native -MemberDefinition @'
[DllImport("user32.dll", EntryPoint = "SystemParametersInfoW", CharSet = CharSet.Unicode)]
public static extern bool SpiString(uint action, uint param, string value, uint flags);
[DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)]
public static extern IntPtr SendMessageTimeoutString(IntPtr hwnd, uint msg, UIntPtr wParam, string lParam, uint flags, uint timeout, out UIntPtr result);
'@
function Broadcast([string]$area) { $r = [UIntPtr]::Zero; [void][LembasSun.Native]::SendMessageTimeoutString([IntPtr]0xffff, 0x001A, [UIntPtr]::Zero, $area, 2, 3000, [ref]$r) }

# ------------------------------------------------------------------ Windhawk styles first (they follow light/dark mode)
$cli = Join-Path $env:ProgramFiles 'Windhawk\windhawk-cli.exe'
function Get-JsonData($text) {
    # windhawk-cli --json wraps results as {"schemaVersion":..,"success":..,"data":{...}}
    try { $j = $text | ConvertFrom-Json } catch { return $null }
    if ($j.PSObject.Properties['data']) { $j = $j.data }
    return $j
}
if (-not $NoWindhawk -and (Test-Path $cli)) {
    $sets = Get-Content (Join-Path $Dest "Windhawk\phases\$phase.json") -Raw -Encoding UTF8 | ConvertFrom-Json
    $list = Get-JsonData ((& $cli --json mod list 2>$null) -join "`n")
    $have = @($list.mods | ForEach-Object { $_.id })
    $tmp = Join-Path $env:TEMP 'lembas-style.json'
    foreach ($s in $sets) {
        if ($have -notcontains $s.mod) { continue }
        # Windhawk wants every declared setting: start from the mod's defaults, drop default list
        # entries for lists Lembas sets itself, then lay the Lembas settings over the top.
        $ours = [ordered]@{}
        foreach ($p in $s.settings.PSObject.Properties) { $ours[$p.Name] = $p.Value }
        $roots = @($ours.Keys | ForEach-Object { if ($_ -match '^([^\[]+)\[') { $Matches[1] + '[' } } | Select-Object -Unique)
        $merged = [ordered]@{}
        $def = Get-JsonData ((& $cli --json mod settings defaults $s.mod 2>$null) -join "`n")
        $defSettings = if ($def -and $def.PSObject.Properties['settings']) { $def.settings } else { $def }
        if ($defSettings) {
            foreach ($p in $defSettings.PSObject.Properties) {
                $inOurList = $false
                foreach ($r in $roots) { if ($p.Name.StartsWith($r)) { $inOurList = $true; break } }
                if (-not $inOurList) { $merged[$p.Name] = $p.Value }
            }
        }
        foreach ($k in $ours.Keys) { $merged[$k] = $ours[$k] }
        [IO.File]::WriteAllText($tmp, ($merged | ConvertTo-Json -Depth 3), (New-Object Text.UTF8Encoding $false))
        $out = (& $cli mod settings write $s.mod $tmp 2>&1) -join ' '
        if ($LASTEXITCODE -ne 0) { Log "  Windhawk didn't take the $($s.mod) style: $($out.Substring(0, [Math]::Min(300, $out.Length)))" }
    }
    Remove-Item $tmp -Force -ErrorAction SilentlyContinue
}

# ------------------------------------------------------------------ accent colour
$accentKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Accent'
$dwmKey = 'HKCU:\Software\Microsoft\Windows\DWM'
foreach ($k in $accentKey, $dwmKey) { if (-not (Test-Path $k)) { New-Item -Path $k -Force | Out-Null } }
$palette = New-Object byte[] 32
for ($i = 0; $i -lt 8; $i++) {
    $hex = $l.accent[$i]
    $palette[$i * 4] = [Convert]::ToByte($hex.Substring(0, 2), 16)
    $palette[$i * 4 + 1] = [Convert]::ToByte($hex.Substring(2, 2), 16)
    $palette[$i * 4 + 2] = [Convert]::ToByte($hex.Substring(4, 2), 16)
}
function Abgr([string]$hex) { [BitConverter]::ToInt32([byte[]]@([Convert]::ToByte($hex.Substring(0, 2), 16), [Convert]::ToByte($hex.Substring(2, 2), 16), [Convert]::ToByte($hex.Substring(4, 2), 16), 0xFF), 0) }
function Argb([string]$hex) { [BitConverter]::ToInt32([byte[]]@([Convert]::ToByte($hex.Substring(4, 2), 16), [Convert]::ToByte($hex.Substring(2, 2), 16), [Convert]::ToByte($hex.Substring(0, 2), 16), 0xC4), 0) }
$main = $l.accent[4]
Set-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name 'AutoColorization' -Value 0 -Type DWord
Set-ItemProperty -Path $accentKey -Name 'AccentPalette' -Value $palette -Type Binary
Set-ItemProperty -Path $accentKey -Name 'AccentColorMenu' -Value (Abgr $main) -Type DWord
Set-ItemProperty -Path $accentKey -Name 'StartColorMenu' -Value (Abgr $l.accent[5]) -Type DWord
Set-ItemProperty -Path $dwmKey -Name 'AccentColor' -Value (Abgr $main) -Type DWord
Set-ItemProperty -Path $dwmKey -Name 'ColorizationColor' -Value (Argb $main) -Type DWord
Set-ItemProperty -Path $dwmKey -Name 'ColorizationAfterglow' -Value (Argb $main) -Type DWord
Set-ItemProperty -Path $dwmKey -Name 'ColorPrevalence' -Value 1 -Type DWord

# ------------------------------------------------------------------ light or dark, and the wallpaper
$pers = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
Set-ItemProperty -Path $pers -Name 'AppsUseLightTheme' -Value ([int]$l.light) -Type DWord
Set-ItemProperty -Path $pers -Name 'SystemUsesLightTheme' -Value ([int]$l.light) -Type DWord
Set-ItemProperty -Path $pers -Name 'EnableTransparency' -Value 1 -Type DWord
Set-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name 'WallpaperStyle' -Value '10'
Set-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name 'TileWallpaper' -Value '0'
[void][LembasSun.Native]::SpiString(0x0014, 0, $wallpaper, 3)
Broadcast 'ImmersiveColorSet'
Broadcast 'WindowsThemeElement'

# ------------------------------------------------------------------ lock screen (skipped while previewing)
if (-not (Test-Path $preview)) {
    try {
        Add-Type -AssemblyName System.Runtime.WindowsRuntime
        $asTaskOp = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' } | Select-Object -First 1
        $asTaskAct = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction' } | Select-Object -First 1
        [void][Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
        [void][Windows.System.UserProfile.LockScreen, Windows.System.UserProfile, ContentType = WindowsRuntime]
        $task = $asTaskOp.MakeGenericMethod([Windows.Storage.StorageFile]).Invoke($null, @([Windows.Storage.StorageFile]::GetFileFromPathAsync($wallpaper)))
        $task.Wait(-1) | Out-Null
        $asTaskAct.Invoke($null, @([Windows.System.UserProfile.LockScreen]::SetImageFileAsync($task.Result))).Wait(-1) | Out-Null
    } catch { Log "  lock screen: $_" }
}

[pscustomobject]@{ phase = $phase; at = (Get-Date).ToString('s') } | ConvertTo-Json | Set-Content -Path $stateFile -Encoding UTF8
