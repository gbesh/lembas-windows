<#
  Lembas for Windows - one-click uninstall. Removes the Lembas Sun task, the Lembas mods (and
  Windhawk, if Lembas installed it and nothing else uses it), then puts back your cursors, sounds,
  colours, icons and wallpaper, removes the fonts, Terminal colours and shortcuts, and deletes
  %LOCALAPPDATA%\Lembas. Windows asks for admin permission once.
#>
param([switch]$NoPause)
$ErrorActionPreference = 'Continue'
$Dest = Join-Path $env:LOCALAPPDATA 'Lembas'
# the Lembas screensaver, if it was chosen
$saver = (Get-ItemProperty 'HKCU:\Control Panel\Desktop' -Name 'SCRNSAVE.EXE' -ErrorAction SilentlyContinue).'SCRNSAVE.EXE'
if ($saver -and $saver -like '*\Lembas\Living\Lembas.scr') {
    Remove-ItemProperty 'HKCU:\Control Panel\Desktop' -Name 'SCRNSAVE.EXE' -ErrorAction SilentlyContinue
    Set-ItemProperty 'HKCU:\Control Panel\Desktop' -Name 'ScreenSaveActive' -Value '0' -ErrorAction SilentlyContinue
}

Add-Type -Namespace LembasU -Name Native -MemberDefinition @'
[DllImport("user32.dll", EntryPoint = "SystemParametersInfoW", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool SpiString(uint action, uint param, string value, uint flags);
[DllImport("user32.dll", EntryPoint = "SystemParametersInfoW", SetLastError = true)]
public static extern bool SpiPtr(uint action, uint param, IntPtr value, uint flags);
[DllImport("gdi32.dll", EntryPoint = "RemoveFontResourceW", CharSet = CharSet.Unicode)]
public static extern bool RemoveFontResource(string file);
[DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)]
public static extern IntPtr SendMessageTimeoutString(IntPtr hwnd, uint msg, UIntPtr wParam, string lParam, uint flags, uint timeout, out UIntPtr result);
'@

function Step($text) { Write-Host ("  * " + $text) -ForegroundColor DarkYellow }
function Restore($path, $name, $value, $type = 'String') {
    try {
        if ($null -eq $value) { Remove-ItemProperty -Path $path -Name $name -ErrorAction SilentlyContinue; return }
        if ($type -eq 'DWord') {
            # colours are saved as 0..4294967295; the registry wants a signed 32-bit number
            $n = [int64]$value
            if ($n -gt [int32]::MaxValue) { $n -= 4294967296 }
            $value = [int32]$n
        }
        if (-not (Test-Path $path)) { New-Item -Path $path -Force | Out-Null }
        Set-ItemProperty -Path $path -Name $name -Value $value -Type $type -ErrorAction Stop
    } catch { Write-Host "    Couldn't put back $name ($($_.Exception.Message))" -ForegroundColor DarkGray }
}

Write-Host ""
Write-Host "  Removing Lembas" -ForegroundColor Yellow
if (-not (Test-Path $Dest)) { Write-Host "  Lembas isn't installed." -ForegroundColor Gray; if (-not $NoPause) { Read-Host "  Press Enter to close" | Out-Null }; exit 0 }

Step "Lembas Desktop"
$desktopExe = Join-Path $Dest 'Living\LembasDesktop.exe'
if (Test-Path $desktopExe) { Start-Process $desktopExe -ArgumentList '--quit' -Wait }
Get-Process LembasDesktop -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'Lembas Desktop' -ErrorAction SilentlyContinue

Step "Windhawk mods and the Lembas Sun task (Windows will ask for permission)"
$admin = Join-Path $Dest 'Tools\Lembas-Admin.ps1'
if (Test-Path $admin) {
    try {
        Start-Process powershell.exe -Verb RunAs -Wait -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$admin`"", '-Mode', 'Uninstall', '-Dest', "`"$Dest`"")
    } catch { Write-Host "    Skipped (permission wasn't given): the mods stay in Windhawk." -ForegroundColor DarkGray }
}
Remove-Item (Join-Path ([Environment]::GetFolderPath('Programs')) 'Lembas') -Recurse -Force -ErrorAction SilentlyContinue
$backupFile = Join-Path $Dest 'backup.json'
$b = if (Test-Path $backupFile) { Get-Content $backupFile -Raw | ConvertFrom-Json } else { $null }

Step "Cursors"
$curKey = 'HKCU:\Control Panel\Cursors'
if ($b) {
    foreach ($p in $b.cursors.PSObject.Properties) {
        if ($p.Name -eq 'Scheme Source') { Restore $curKey $p.Name $p.Value 'DWord' }
        elseif ($p.Name -eq '(default)') { Set-ItemProperty -Path $curKey -Name '(default)' -Value $(if ($p.Value) { $p.Value } else { 'Windows Default' }) }
        else { Set-ItemProperty -Path $curKey -Name $p.Name -Value $(if ($p.Value) { $p.Value } else { '' }) }
    }
} else {
    Get-Item $curKey | Select-Object -ExpandProperty Property | Where-Object { $_ -notin '(default)', 'Scheme Source', 'CursorBaseSize' } |
        ForEach-Object { Set-ItemProperty -Path $curKey -Name $_ -Value '' }
    Set-ItemProperty -Path $curKey -Name '(default)' -Value 'Windows Default'
}
Remove-ItemProperty -Path "$curKey\Schemes" -Name 'Lembas' -ErrorAction SilentlyContinue
[void][LembasU.Native]::SpiPtr(0x57, 0, [IntPtr]::Zero, 3)

Step "Sounds"
$evKey = 'HKCU:\AppEvents\Schemes\Apps\.Default'
if ($b) {
    foreach ($p in $b.sounds.PSObject.Properties) {
        if ($p.Name -eq '(scheme)') { Set-Item -Path 'HKCU:\AppEvents\Schemes' -Value $(if ($p.Value) { $p.Value } else { '.Default' }) }
        elseif (Test-Path "$evKey\$($p.Name)\.Current") { Set-Item -Path "$evKey\$($p.Name)\.Current" -Value $(if ($p.Value) { $p.Value } else { '' }) }
    }
} else {
    Set-Item -Path 'HKCU:\AppEvents\Schemes' -Value '.Default'
}
Get-ChildItem $evKey -ErrorAction SilentlyContinue | ForEach-Object {
    $k = Join-Path $_.PSPath 'Lembas'
    if (Test-Path $k) { Remove-Item $k -Recurse -Force }
}
Remove-Item 'HKCU:\AppEvents\Schemes\Names\Lembas' -Recurse -Force -ErrorAction SilentlyContinue

Step "Colours and wallpaper"
if ($b) {
    Restore 'HKCU:\Control Panel\Desktop' 'AutoColorization' $b.misc.AutoColorization 'DWord'
    Restore 'HKCU:\Software\Microsoft\Windows\DWM' 'ColorPrevalence' $b.misc.DwmColorPrevalence 'DWord'
    $pers = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
    Restore $pers 'AppsUseLightTheme' $b.misc.AppsUseLightTheme 'DWord'
    Restore $pers 'SystemUsesLightTheme' $b.misc.SystemUsesLightTheme 'DWord'
    if ($b.misc.WallpaperStyle) { Set-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name 'WallpaperStyle' -Value $b.misc.WallpaperStyle }
    if ($b.misc.Wallpaper -and (Test-Path $b.misc.Wallpaper)) { [void][LembasU.Native]::SpiString(0x0014, 0, $b.misc.Wallpaper, 3) }
}
$accentBackup = Join-Path $Dest 'backup-accent.json'
if (Test-Path $accentBackup) {
    $ab = Get-Content $accentBackup -Raw | ConvertFrom-Json
    $accentKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Accent'
    $dwmKey = 'HKCU:\Software\Microsoft\Windows\DWM'
    Restore 'HKCU:\Control Panel\Desktop' 'AutoColorization' $ab.AutoColorization 'DWord'
    # Saved as base64 text (older installs saved the raw numbers, sometimes with the text after them)
    $pal = $null
    foreach ($item in @($ab.AccentPalette)) { if ($item -is [string]) { try { $pal = [Convert]::FromBase64String($item) } catch { } } }
    if (-not $pal) {
        $nums = @(@($ab.AccentPalette) | Where-Object { $_ -isnot [string] })
        if ($nums.Count -ge 32) { $pal = [byte[]]($nums[0..31]) }
    }
    if ($pal) {
        try { Set-ItemProperty -Path $accentKey -Name 'AccentPalette' -Value $pal -Type Binary -ErrorAction Stop }
        catch { Write-Host "    Couldn't put back the accent palette; Windows picks a new one from your next colour change." -ForegroundColor DarkGray }
    }
    Restore $accentKey 'AccentColorMenu' $ab.AccentColorMenu 'DWord'
    Restore $accentKey 'StartColorMenu' $ab.StartColorMenu 'DWord'
    Restore $dwmKey 'AccentColor' $ab.AccentColor 'DWord'
    Restore $dwmKey 'ColorizationColor' $ab.ColorizationColor 'DWord'
    Restore $dwmKey 'ColorizationAfterglow' $ab.ColorizationAfterglow 'DWord'
}
$r = [UIntPtr]::Zero
[void][LembasU.Native]::SendMessageTimeoutString([IntPtr]0xffff, 0x001A, [UIntPtr]::Zero, 'ImmersiveColorSet', 2, 3000, [ref]$r)

Step "Desktop icons"
$clsidRoot = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\CLSID'
$iconBackup = Join-Path $Dest 'backup-icons.json'
if (Test-Path $iconBackup) {
    $ib = Get-Content $iconBackup -Raw | ConvertFrom-Json
    foreach ($p in $ib.PSObject.Properties) {
        $k = "$clsidRoot\$($p.Name)\DefaultIcon"
        if ($null -eq $p.Value) { Remove-Item "$clsidRoot\$($p.Name)" -Recurse -Force -ErrorAction SilentlyContinue }
        else {
            foreach ($v in $p.Value.PSObject.Properties) {
                if ($v.Name -eq '(default)') { if ($v.Value) { Set-Item -Path $k -Value $v.Value } }
                else { Restore $k $v.Name $v.Value }
            }
        }
    }
}
$driveList = Join-Path $Dest 'drive-icons.txt'
if (Test-Path $driveList) {
    foreach ($letter in Get-Content $driveList) {
        if ($letter) { Remove-Item "HKCU:\Software\Classes\Applications\Explorer.exe\Drives\$letter" -Recurse -Force -ErrorAction SilentlyContinue }
    }
}
Add-Type -Namespace LembasU2 -Name Shell -MemberDefinition '[DllImport("shell32.dll")] public static extern void SHChangeNotify(int e, uint f, IntPtr a, IntPtr b);'
[LembasU2.Shell]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)

Step "Themes, Terminal schemes and fonts"
Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Microsoft\Windows\Themes') -Filter 'Lembas - *.theme' -ErrorAction SilentlyContinue | Remove-Item -Force
Remove-Item (Join-Path $env:LOCALAPPDATA 'Microsoft\Windows Terminal\Fragments\Lembas') -Recurse -Force -ErrorAction SilentlyContinue
$fontReg = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Fonts'
$props = (Get-Item $fontReg -ErrorAction SilentlyContinue).Property | Where-Object { $_ -like 'Lembas *' }
foreach ($p in $props) {
    $file = (Get-ItemProperty $fontReg -Name $p).$p
    [void][LembasU.Native]::RemoveFontResource($file)
    Remove-ItemProperty -Path $fontReg -Name $p
    Remove-Item $file -Force -ErrorAction SilentlyContinue
}

Step "Files in $Dest"
Set-Location $env:USERPROFILE   # the Start-menu shortcut started us inside the Lembas folder
Remove-Item $Dest -Recurse -Force -ErrorAction SilentlyContinue
if (Test-Path $Dest) { Write-Host "    Some files are still in use; they'll go after a restart, or delete $Dest yourself." -ForegroundColor DarkGray }

Write-Host ""
Write-Host "  Lembas is removed. The lock screen picture stays until you change it in" -ForegroundColor Gray
Write-Host "  Settings > Personalisation > Lock screen." -ForegroundColor Gray
Write-Host ""
if (-not $NoPause) { Read-Host "  Press Enter to close" | Out-Null }
