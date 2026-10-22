<#
  Lembas for Windows - one-click install (and repair, and update: running it again is always safe).

  As you:   fonts, cursors, sounds, icons, Terminal colours, Start-menu shortcuts, and Lembas Desktop
            (the gently animated scene and clock behind your desktop icons).
  As admin: Windhawk (installed or upgraded silently) with every Lembas mod and style, and the
            "Lembas Sun" task that switches between the four looks through the day.
  Windows asks for admin permission once. Everything is backed up first; "Uninstall Lembas"
  puts it all back.
#>
param(
    [double]$Latitude = [double]::NaN,   # optional: your location, for sunrise and sunset
    [double]$Longitude = [double]::NaN,
    [switch]$NoPause,                    # close the window at the end without waiting
    [switch]$Quiet                       # automatic update: no window, no prompts
)

$ErrorActionPreference = 'Stop'
$Tools = $PSScriptRoot
$Payload = Split-Path -Parent $Tools
$Dest = Join-Path $env:LOCALAPPDATA 'Lembas'

function Say($text, $color = 'Gray') { if (-not $Quiet) { Write-Host $text -ForegroundColor $color } }
function Step($text) { if (-not $Quiet) { Write-Host ("  * " + $text) -ForegroundColor DarkYellow } }
function Done($text) { if (-not $Quiet) { Write-Host ("    " + $text) -ForegroundColor DarkGray } }
function Ensure-Key([string]$path) { if (-not (Test-Path $path)) { New-Item -Path $path -Force | Out-Null } }
function Get-Value([string]$path, [string]$name) { try { (Get-ItemProperty -Path $path -Name $name -ErrorAction Stop).$name } catch { $null } }
function Finish($code) { if (-not $NoPause -and -not $Quiet) { Say ""; Read-Host "  Press Enter to close" | Out-Null }; exit $code }

Say ""
Say "  LEMBAS FOR WINDOWS" 'Yellow'
Say "  One small bite is enough." 'DarkGray'
Say ""
if (-not (Test-Path (Join-Path $Payload 'looks.json'))) { Say "  Unzip the whole Lembas folder first, then run Install Lembas again." 'Red'; Finish 1 }

Add-Type -Namespace Lembas -Name Native -MemberDefinition @'
[DllImport("user32.dll", EntryPoint = "SystemParametersInfoW", SetLastError = true)]
public static extern bool SpiPtr(uint action, uint param, IntPtr value, uint flags);
[DllImport("gdi32.dll", EntryPoint = "AddFontResourceW", CharSet = CharSet.Unicode)]
public static extern int AddFontResource(string file);
[DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")]
public static extern IntPtr SendMessageTimeoutPtr(IntPtr hwnd, uint msg, UIntPtr wParam, IntPtr lParam, uint flags, uint timeout, out UIntPtr result);
[DllImport("shell32.dll")]
public static extern void SHChangeNotify(int eventId, uint flags, IntPtr item1, IntPtr item2);
'@

# ------------------------------------------------------------------ 1. files
$legacy = (Test-Path (Join-Path $Dest 'backup.json')) -and -not (Test-Path (Join-Path $Dest 'lembas.json'))   # Lembas 1 is here
$samePlace = (Resolve-Path $Payload).Path.TrimEnd('\') -ieq $Dest.TrimEnd('\')
# Lembas Desktop holds its files open while it runs: stop it for the copy (it's started again below)
$desktopExe = Join-Path $Dest 'Living\LembasDesktop.exe'
if (Get-Process LembasDesktop -ErrorAction SilentlyContinue) {
    if (Test-Path $desktopExe) { Start-Process $desktopExe -ArgumentList '--quit' -Wait }
    Get-Process LembasDesktop -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
if (-not $samePlace) {
    Step "Copying Lembas to $Dest"
    New-Item -ItemType Directory -Path $Dest -Force | Out-Null
    $root = (Resolve-Path $Payload).Path.TrimEnd('\')
    Get-ChildItem -Path $root -Recurse -File | Unblock-File -ErrorAction SilentlyContinue
    foreach ($f in Get-ChildItem -Path $root -Recurse -File) {
        $target = Join-Path $Dest $f.FullName.Substring($root.Length + 1)
        New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
        try { Copy-Item -LiteralPath $f.FullName -Destination $target -Force -ErrorAction Stop }
        catch {
            # Windows may hold a sound or cursor open; an unchanged one doesn't matter
            if (-not ((Test-Path $target) -and ((Get-FileHash $target).Hash -eq (Get-FileHash $f.FullName).Hash))) {
                Done "In use, updates after a restart: $($f.Name)"
            }
        }
    }
    # tidy away what Lembas 1 left behind
    foreach ($old in 'Apply Windhawk styles.cmd', 'Apply-WindhawkStyles.ps1', 'Desktop', 'Lembas-Lively.zip', 'Lembas.ddw', 'Lembas-WinDynamicDesktop.zip', 'Windhawk\lembas-styles.json', 'Windhawk\10-explorer-font.yaml') {
        Remove-Item (Join-Path $Dest $old) -Recurse -Force -ErrorAction SilentlyContinue
    }
}
Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Microsoft\Windows\Themes') -Filter 'Lembas - *.theme' -ErrorAction SilentlyContinue | Remove-Item -Force

# Fill in this PC's install path in the Windhawk styles
foreach ($f in Get-ChildItem (Join-Path $Dest 'Windhawk') -Recurse -Include *.yaml, *.json) {
    $text = [IO.File]::ReadAllText($f.FullName)
    if ($text.Contains('{{LEMBAS}}')) {
        $path = if ($f.Extension -eq '.json') { $Dest.Replace('\', '\\') } else { $Dest }
        [IO.File]::WriteAllText($f.FullName, $text.Replace('{{LEMBAS}}', $path), (New-Object Text.UTF8Encoding $false))
    }
}

# ------------------------------------------------------------------ 2. back up (first install only)
$cursorNames = 'Arrow','Help','AppStarting','Wait','Crosshair','IBeam','NWPen','No','SizeNS','SizeWE','SizeNWSE','SizeNESW','SizeAll','UpArrow','Hand','Pin','Person'
$soundEvents = [ordered]@{
    '.Default' = 'Lembas_Chime.wav'; 'SystemAsterisk' = 'Lembas_Chime.wav'; 'SystemExclamation' = 'Lembas_Warning.wav'
    'SystemHand' = 'Lembas_Error.wav'; 'SystemNotification' = 'Lembas_Notify.wav'; 'Notification.Default' = 'Lembas_Notify.wav'
    'Notification.IM' = 'Lembas_Notify.wav'; 'Notification.Mail' = 'Lembas_Notify.wav'; 'Notification.Reminder' = 'Lembas_Chime.wav'
    'MailBeep' = 'Lembas_Notify.wav'; 'DeviceConnect' = 'Lembas_Connect.wav'; 'DeviceDisconnect' = 'Lembas_Disconnect.wav'
    'DeviceFail' = 'Lembas_Error.wav'; 'WindowsUAC' = 'Lembas_Chime.wav'; 'WindowsLogon' = 'Lembas_Logon.wav'
}
$curKey = 'HKCU:\Control Panel\Cursors'
$evKey = 'HKCU:\AppEvents\Schemes\Apps\.Default'
$pers = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
$accentKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Accent'
$dwmKey = 'HKCU:\Software\Microsoft\Windows\DWM'
$backupFile = Join-Path $Dest 'backup.json'
if (-not (Test-Path $backupFile)) {
    Step "Backing up your cursors, sounds, colours and wallpaper"
    $b = [ordered]@{ cursors = [ordered]@{}; sounds = [ordered]@{}; misc = [ordered]@{} }
    $b.cursors['(default)'] = Get-Value $curKey '(default)'
    $b.cursors['Scheme Source'] = Get-Value $curKey 'Scheme Source'
    foreach ($n in $cursorNames) { $b.cursors[$n] = Get-Value $curKey $n }
    $b.sounds['(scheme)'] = Get-Value 'HKCU:\AppEvents\Schemes' '(default)'
    foreach ($e in $soundEvents.Keys) { $b.sounds[$e] = Get-Value "$evKey\$e\.Current" '(default)' }
    $b.misc['AutoColorization'] = Get-Value 'HKCU:\Control Panel\Desktop' 'AutoColorization'
    $b.misc['DwmColorPrevalence'] = Get-Value $dwmKey 'ColorPrevalence'
    $b.misc['Wallpaper'] = Get-Value 'HKCU:\Control Panel\Desktop' 'WallPaper'
    $b.misc['WallpaperStyle'] = Get-Value 'HKCU:\Control Panel\Desktop' 'WallpaperStyle'
    $b.misc['AppsUseLightTheme'] = Get-Value $pers 'AppsUseLightTheme'
    $b.misc['SystemUsesLightTheme'] = Get-Value $pers 'SystemUsesLightTheme'
    $b | ConvertTo-Json -Depth 4 | Set-Content -Path $backupFile -Encoding UTF8
}
$accentBackup = Join-Path $Dest 'backup-accent.json'
if (-not (Test-Path $accentBackup)) {
    $v = Get-Value $accentKey 'AccentPalette'
    $palText = $null
    if ($v) { $palText = [string][Convert]::ToBase64String([byte[]]$v) }   # one plain string
    [ordered]@{
        AutoColorization = Get-Value 'HKCU:\Control Panel\Desktop' 'AutoColorization'
        AccentPalette = $palText
        AccentColorMenu = Get-Value $accentKey 'AccentColorMenu'; StartColorMenu = Get-Value $accentKey 'StartColorMenu'
        AccentColor = Get-Value $dwmKey 'AccentColor'; ColorizationColor = Get-Value $dwmKey 'ColorizationColor'
        ColorizationAfterglow = Get-Value $dwmKey 'ColorizationAfterglow'
    } | ConvertTo-Json | Set-Content -Path $accentBackup -Encoding UTF8
}

# ------------------------------------------------------------------ 3. fonts (just for you)
Step "Fonts"
$userFonts = Join-Path $env:LOCALAPPDATA 'Microsoft\Windows\Fonts'
New-Item -ItemType Directory -Path $userFonts -Force | Out-Null
$fontReg = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Fonts'
Ensure-Key $fontReg
foreach ($f in Get-ChildItem (Join-Path $Dest 'Fonts') -Filter *.ttf) {
    $target = Join-Path $userFonts ("Lembas-" + $f.Name)
    if (-not (Test-Path $target)) { Copy-Item $f.FullName $target -Force }
    Set-ItemProperty -Path $fontReg -Name ("Lembas " + $f.BaseName + " (TrueType)") -Value $target
    [void][Lembas.Native]::AddFontResource($target)
}
$r = [UIntPtr]::Zero
[void][Lembas.Native]::SendMessageTimeoutPtr([IntPtr]0xffff, 0x001D, [UIntPtr]::Zero, [IntPtr]::Zero, 2, 3000, [ref]$r)

# ------------------------------------------------------------------ 4. cursors
Step "Cursors (the busy cursor is the One Ring)"
$cd = Join-Path $Dest 'Cursors'
$cursorFiles = @{
    Arrow = 'Lembas_Arrow.cur'; Help = 'Lembas_Help.cur'; AppStarting = 'Lembas_Working.ani'; Wait = 'Lembas_Busy.ani'
    Crosshair = 'Lembas_Precision.cur'; IBeam = 'Lembas_Text.cur'; NWPen = 'Lembas_Handwriting.cur'; No = 'Lembas_Unavailable.cur'
    SizeNS = 'Lembas_SizeNS.cur'; SizeWE = 'Lembas_SizeWE.cur'; SizeNWSE = 'Lembas_SizeNWSE.cur'; SizeNESW = 'Lembas_SizeNESW.cur'
    SizeAll = 'Lembas_Move.cur'; UpArrow = 'Lembas_Alternate.cur'; Hand = 'Lembas_Link.cur'; Pin = ''; Person = ''
}
$schemeParts = foreach ($n in $cursorNames) {
    $path = if ($cursorFiles[$n]) { Join-Path $cd $cursorFiles[$n] } else { '' }
    Set-ItemProperty -Path $curKey -Name $n -Value $path
    $path
}
Set-ItemProperty -Path $curKey -Name '(default)' -Value 'Lembas'
Set-ItemProperty -Path $curKey -Name 'Scheme Source' -Value 1 -Type DWord
Ensure-Key "$curKey\Schemes"
Set-ItemProperty -Path "$curKey\Schemes" -Name 'Lembas' -Value ($schemeParts -join ',')
[void][Lembas.Native]::SpiPtr(0x57, 0, [IntPtr]::Zero, 3)

# ------------------------------------------------------------------ 5. sounds
Step "Sounds (harp, bells and a low horn)"
$sd = Join-Path $Dest 'Sounds'
Ensure-Key 'HKCU:\AppEvents\Schemes\Names\Lembas'
Set-Item -Path 'HKCU:\AppEvents\Schemes\Names\Lembas' -Value 'Lembas'
foreach ($e in $soundEvents.Keys) {
    foreach ($sub in 'Lembas', '.Current') {
        Ensure-Key "$evKey\$e\$sub"
        Set-Item -Path "$evKey\$e\$sub" -Value (Join-Path $sd $soundEvents[$e])
    }
}
Set-Item -Path 'HKCU:\AppEvents\Schemes' -Value 'Lembas'

# ------------------------------------------------------------------ 6. desktop and drive icons
Step "Icons for This PC, the Recycle Bin, Network, your folder and your drives"
$id = Join-Path $Dest 'Icons'
$desktopIcons = [ordered]@{
    '{20D04FE0-3AEA-1069-A2D8-08002B30309D}' = @{ '(default)' = "$id\this-pc.ico" }
    '{59031a47-3f72-44a7-89c5-5595fe6b30ee}' = @{ '(default)' = "$id\user.ico" }
    '{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}' = @{ '(default)' = "$id\network.ico" }
    '{645FF040-5081-101B-9F08-00AA002F954E}' = @{ '(default)' = "$id\recycle-empty.ico"; 'empty' = "$id\recycle-empty.ico"; 'full' = "$id\recycle-full.ico" }
}
$clsidRoot = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\CLSID'
$iconBackup = Join-Path $Dest 'backup-icons.json'
if (-not (Test-Path $iconBackup)) {
    $ib = [ordered]@{}
    foreach ($c in $desktopIcons.Keys) {
        $k = "$clsidRoot\$c\DefaultIcon"
        $ib[$c] = if (Test-Path $k) { $vals = [ordered]@{}; foreach ($n in $desktopIcons[$c].Keys) { $vals[$n] = Get-Value $k $n }; $vals } else { $null }
    }
    $ib | ConvertTo-Json -Depth 4 | Set-Content -Path $iconBackup -Encoding UTF8
}
foreach ($c in $desktopIcons.Keys) {
    $k = "$clsidRoot\$c\DefaultIcon"
    Ensure-Key $k
    foreach ($n in $desktopIcons[$c].Keys) {
        if ($n -eq '(default)') { Set-Item -Path $k -Value $desktopIcons[$c][$n] } else { Set-ItemProperty -Path $k -Name $n -Value $desktopIcons[$c][$n] }
    }
}
$driveRoot = 'HKCU:\Software\Classes\Applications\Explorer.exe\Drives'
$driveList = Join-Path $Dest 'drive-icons.txt'
$sysDrive = $env:SystemDrive.TrimEnd(':')
$new = @()
foreach ($d in Get-CimInstance Win32_LogicalDisk -ErrorAction SilentlyContinue) {
    $letter = $d.DeviceID.TrimEnd(':')
    $icon = switch ($d.DriveType) { 3 { if ($letter -eq $sysDrive) { 'drive-windows.ico' } else { 'drive.ico' } } 2 { 'removable.ico' } 5 { 'disc.ico' } default { $null } }
    if (-not $icon) { continue }
    $k = "$driveRoot\$letter\DefaultIcon"
    if (-not (Test-Path $k)) { $new += $letter }
    Ensure-Key $k
    Set-Item -Path $k -Value (Join-Path $id $icon)
}
if ($new) { Add-Content -Path $driveList -Value $new }
[Lembas.Native]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)

# ------------------------------------------------------------------ 7. Windows Terminal
Step "Windows Terminal colours"
$wtFrag = Join-Path $env:LOCALAPPDATA 'Microsoft\Windows Terminal\Fragments\Lembas'
New-Item -ItemType Directory -Path $wtFrag -Force | Out-Null
Copy-Item (Join-Path $Dest 'Terminal\lembas.json') (Join-Path $wtFrag 'lembas.json') -Force

# ------------------------------------------------------------------ 8. settings: your location and chosen look
$configFile = Join-Path $Dest 'lembas.json'
$config = if (Test-Path $configFile) { Get-Content $configFile -Raw | ConvertFrom-Json } else { [pscustomobject]@{} }
if (-not $config.PSObject.Properties['look']) { $config | Add-Member -NotePropertyName look -NotePropertyValue 'adaptive' }
if (-not $config.PSObject.Properties['autoUpdate']) { $config | Add-Member -NotePropertyName autoUpdate -NotePropertyValue $true }
# Where updates come from (the public Lembas for Windows releases)
$config | Add-Member -NotePropertyName updateUrl -NotePropertyValue 'https://github.com/gbesh/lembas-windows/releases/latest/download/Lembas-for-Windows.zip' -Force
$config | Add-Member -NotePropertyName releaseApi -NotePropertyValue 'https://api.github.com/repos/gbesh/lembas-windows/releases/latest' -Force
if (-not [double]::IsNaN($Latitude) -and -not [double]::IsNaN($Longitude)) {
    $config | Add-Member -NotePropertyName latitude -NotePropertyValue $Latitude -Force
    $config | Add-Member -NotePropertyName longitude -NotePropertyValue $Longitude -Force
    $config | Add-Member -NotePropertyName locationSource -NotePropertyValue 'manual' -Force
} elseif (-not $config.PSObject.Properties['latitude']) {
    # The main city of your time zone: sunrise and sunset to within a few minutes, no location access needed
    $cities = @{
        'AUS Eastern Standard Time' = @(-33.87, 151.21); 'E. Australia Standard Time' = @(-27.47, 153.03)
        'Cen. Australia Standard Time' = @(-34.93, 138.60); 'AUS Central Standard Time' = @(-12.46, 130.84)
        'W. Australia Standard Time' = @(-31.95, 115.86); 'Tasmania Standard Time' = @(-42.88, 147.33)
        'New Zealand Standard Time' = @(-36.85, 174.76); 'GMT Standard Time' = @(51.51, -0.13)
        'W. Europe Standard Time' = @(52.52, 13.40); 'Romance Standard Time' = @(48.86, 2.35)
        'Eastern Standard Time' = @(40.71, -74.01); 'Central Standard Time' = @(41.88, -87.63)
        'Mountain Standard Time' = @(39.74, -104.99); 'Pacific Standard Time' = @(34.05, -118.24)
        'Tokyo Standard Time' = @(35.68, 139.69); 'Singapore Standard Time' = @(1.35, 103.82)
        'India Standard Time' = @(19.08, 72.88); 'China Standard Time' = @(31.23, 121.47)
    }
    $c = $cities[[TimeZoneInfo]::Local.Id]
    if (-not $c) { $c = @(0, [TimeZoneInfo]::Local.BaseUtcOffset.TotalMinutes / 4) }
    $config | Add-Member -NotePropertyName latitude -NotePropertyValue $c[0]
    $config | Add-Member -NotePropertyName longitude -NotePropertyValue $c[1]
}
$config | Add-Member -NotePropertyName version -NotePropertyValue ((Get-Content (Join-Path $Dest 'version.txt') -ErrorAction SilentlyContinue) -join '') -Force
if ($legacy) { $config | Add-Member -NotePropertyName legacyApps -NotePropertyValue $true -Force }

# ------------------------------------------------------------------ 8b. Lembas 1's helper apps (WinDynamicDesktop, Lively), now replaced by Lembas Sun
if ($config.PSObject.Properties['legacyApps'] -and $config.legacyApps) {
    $left = $false
    foreach ($app in @(@{ name = 'WinDynamicDesktop'; proc = 'WinDynamicDesktop' }, @{ name = 'Lively Wallpaper'; proc = 'Lively' })) {
        $entry = foreach ($root in 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall') {
            Get-ChildItem $root -ErrorAction SilentlyContinue | Get-ItemProperty -ErrorAction SilentlyContinue | Where-Object { $_.DisplayName -like "$($app.name)*" }
        }
        $entry = $entry | Select-Object -First 1
        if (-not $entry) { continue }
        Step "Removing $($app.name) (Lembas Sun does its job now)"
        Get-Process | Where-Object { $_.ProcessName -like "$($app.proc)*" } | Stop-Process -Force -ErrorAction SilentlyContinue
        $cmd = if ($entry.QuietUninstallString) { $entry.QuietUninstallString } else { $entry.UninstallString }
        if ($cmd -match '^\s*"([^"]+)"\s*(.*)$') { $exe = $Matches[1]; $rest = $Matches[2] } else { $exe = ($cmd -split ' ')[0]; $rest = ($cmd -split ' ', 2)[1] }
        if (-not $entry.QuietUninstallString) {
            $rest = if ($exe -match 'unins\d+\.exe$') { "$rest /VERYSILENT /SUPPRESSMSGBOXES /NORESTART" } elseif ($exe -match 'msiexec') { "$rest /qn" } else { "$rest /S" }
        }
        try { Start-Process $exe -ArgumentList $rest -Wait -ErrorAction Stop } catch { $left = $true; Done "Couldn't remove it automatically; uninstall it from Settings > Apps." }
    }
    if (-not $left) { $config.PSObject.Properties.Remove('legacyApps') }
}
$config | ConvertTo-Json | Set-Content -Path $configFile -Encoding UTF8

# ------------------------------------------------------------------ 9. Start-menu shortcuts
Step "Start menu: Lembas > Preview, Change look, Update, Uninstall"
$menu = Join-Path ([Environment]::GetFolderPath('Programs')) 'Lembas'
New-Item -ItemType Directory -Path $menu -Force | Out-Null
$shell = New-Object -ComObject WScript.Shell
$ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
foreach ($s in @(
        @{ name = 'Lembas - Preview the four looks'; script = 'Preview-Lembas.ps1'; icon = 'Icons\favorites.ico' },
        @{ name = 'Lembas - Change look'; script = 'Change-Look.ps1'; icon = 'Icons\pictures.ico' },
        @{ name = 'Lembas - Update'; script = 'Update-Lembas.ps1'; icon = 'Icons\downloads.ico' },
        @{ name = 'Lembas - Uninstall'; script = 'Uninstall-Lembas.ps1'; icon = 'Icons\recycle-empty.ico' })) {
    $lnk = $shell.CreateShortcut((Join-Path $menu ($s.name + '.lnk')))
    $lnk.TargetPath = $ps
    $lnk.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $Dest ('Tools\' + $s.script))`""
    $lnk.IconLocation = Join-Path $Dest $s.icon
    $lnk.WorkingDirectory = $Dest
    $lnk.Save()
}

# ------------------------------------------------------------------ 9b. Lembas Desktop
if (-not $config.PSObject.Properties['livingDesktop']) { $config | Add-Member -NotePropertyName livingDesktop -NotePropertyValue $true }
if (-not $config.PSObject.Properties['desktopClock']) { $config | Add-Member -NotePropertyName desktopClock -NotePropertyValue $true }
$config | ConvertTo-Json | Set-Content -Path $configFile -Encoding UTF8
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$isArm = ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') -or ($env:PROCESSOR_ARCHITEW6432 -eq 'ARM64') -or
         ((Get-CimInstance Win32_Processor -ErrorAction SilentlyContinue | Select-Object -First 1).Architecture -eq 12)
$exeBuild = Join-Path $Dest ('Living\LembasDesktop-' + $(if ($isArm) { 'arm64' } else { 'x64' }) + '.exe')
if ($config.livingDesktop -and (Test-Path $exeBuild)) {
    Step "Lembas Desktop: the scene, gently moving, with the time and the Shire date behind your icons"
    Copy-Item $exeBuild $desktopExe -Force
    Set-ItemProperty -Path $runKey -Name 'Lembas Desktop' -Value "`"$desktopExe`""
    # Start it as you (not as admin, even when an automatic update runs elevated)
    Start-Process explorer.exe -ArgumentList "`"$desktopExe`""
} else {
    Remove-ItemProperty -Path $runKey -Name 'Lembas Desktop' -ErrorAction SilentlyContinue
}

# ------------------------------------------------------------------ 10. the admin part: Windhawk, its mods, the sun task
Step "Windhawk, its Lembas mods and the Lembas Sun task (Windows will ask for permission once)"
$admin = Join-Path $Dest 'Tools\Lembas-Admin.ps1'
$adminArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$admin`"", '-Mode', 'Install',
               '-User', "`"$([Security.Principal.WindowsIdentity]::GetCurrent().Name)`"", '-Dest', "`"$Dest`"")
if ($legacy) { $adminArgs += '-Legacy' }
$ok = $true
try {
    $p = if ($Quiet) { Start-Process powershell.exe -Verb RunAs -ArgumentList $adminArgs -Wait -PassThru -WindowStyle Hidden }
         else { Start-Process powershell.exe -Verb RunAs -ArgumentList $adminArgs -Wait -PassThru }
    if ($p.ExitCode -ne 0) { $ok = $false; Done "The admin part reported a problem; see $Dest\install.log" }
} catch {
    $ok = $false
    Done "Skipped (permission wasn't given). Run Install Lembas again to add Windhawk and the sun task."
    # Still switch to the right look now, without the Windhawk parts
    & (Join-Path $Dest 'Tools\LembasSun.ps1') -Force -NoWindhawk
}

Say ""
if ($ok) {
    Say "  Lembas is installed." 'Yellow'
    Say "  It follows the sun: Shire Morning from sunrise, Red Book from midday, Starlit Night from" 'Gray'
    Say "  sunset and Elven Silver from 11pm. To see all four now: Start > Lembas > Preview the four looks." 'Gray'
} else {
    Say "  Lembas is partly installed (see above)." 'Yellow'
}
Finish 0
