<#
  Lembas - set your location, for the weather in the scene and for sunrise and sunset.
  Type your suburb or town and pick it from the list; or let Lembas use your phone's area (when
  "Weather for where you are" is on in Lembas on the phone and sync is on), or your time zone's
  main city. The place you type is looked up with Open-Meteo's free place search; only the
  rounded coordinates (to about a kilometre) and the place's name are kept, on this PC.
#>
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
$Dest = Split-Path -Parent $PSScriptRoot
$configFile = Join-Path $Dest 'lembas.json'
$config = Get-Content $configFile -Raw | ConvertFrom-Json
function Get-Prop($o, $name, $default) { if ($o.PSObject.Properties[$name] -and $null -ne $o.$name) { $o.$name } else { $default } }
$inv = [Globalization.CultureInfo]::InvariantCulture

$source = Get-Prop $config 'locationSource' ''
$now = switch ($source) {
    'manual' { "Now: $(Get-Prop $config 'locationName' 'a place you set')" }
    'phone' { 'Now: your phone''s area' }
    default { 'Now: your time zone''s main city' }
}

$f = New-Object Windows.Forms.Form
$f.Text = 'Lembas - your location'
$f.Font = New-Object Drawing.Font('Segoe UI', 10)
$f.FormBorderStyle = 'FixedDialog'; $f.MaximizeBox = $false; $f.MinimizeBox = $false
$f.StartPosition = 'CenterScreen'; $f.AutoScaleMode = 'Dpi'; $f.ClientSize = New-Object Drawing.Size(440, 330)
$icon = Join-Path $Dest 'Icons\network.ico'
if (Test-Path $icon) { $f.Icon = New-Object Drawing.Icon($icon) }

$lbl = New-Object Windows.Forms.Label
$lbl.Text = "For the weather in the scene, and sunrise and sunset.`n$now"
$lbl.SetBounds(16, 12, 410, 44)
$box = New-Object Windows.Forms.TextBox
$box.SetBounds(16, 62, 316, 28)
$find = New-Object Windows.Forms.Button
$find.Text = 'Find'; $find.SetBounds(340, 60, 84, 30)
$list = New-Object Windows.Forms.ListBox
$list.SetBounds(16, 98, 408, 120)
$use = New-Object Windows.Forms.Button
$use.Text = 'Use this place'; $use.SetBounds(16, 226, 408, 32); $use.Enabled = $false
$phone = New-Object Windows.Forms.Button
$phone.Text = 'Use my phone''s area, or else my time zone'; $phone.SetBounds(16, 266, 408, 32)
$note = New-Object Windows.Forms.Label
$note.Text = ''; $note.ForeColor = [Drawing.Color]::DimGray; $note.SetBounds(16, 302, 408, 22)
$f.Controls.AddRange(@($lbl, $box, $find, $list, $use, $phone, $note))
$f.AcceptButton = $find

$script:places = @()
$find.Add_Click({
    $q = $box.Text.Trim()
    if (-not $q) { return }
    $note.Text = 'Looking...'; $f.Refresh()
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $r = Invoke-RestMethod ("https://geocoding-api.open-meteo.com/v1/search?count=8&language=en&format=json&name=" + [Uri]::EscapeDataString($q)) -TimeoutSec 15
        $script:places = @($r.results)
        $list.Items.Clear()
        foreach ($p in $script:places) { [void]$list.Items.Add((@($p.name, $p.admin1, $p.country) | Where-Object { $_ }) -join ', ') }
        $note.Text = if ($script:places.Count) { 'Pick one, then "Use this place".' } else { 'Nothing found: try a nearby town.' }
        if ($script:places.Count) { $list.SelectedIndex = 0; $use.Enabled = $true }
    } catch { $note.Text = "Couldn't search: $($_.Exception.Message)" }
})
$list.Add_DoubleClick({ $use.PerformClick() })

function Save-And-Apply {
    $config | ConvertTo-Json -Depth 5 | Set-Content $configFile -Encoding UTF8
    Remove-Item (Join-Path $Dest 'weather.json') -ErrorAction SilentlyContinue   # fetch the weather again for the new place
    if (Get-ScheduledTask -TaskPath '\Lembas\' -TaskName 'Lembas Sun' -ErrorAction SilentlyContinue) { Start-ScheduledTask -TaskPath '\Lembas\' -TaskName 'Lembas Sun' }
}
$use.Add_Click({
    if ($list.SelectedIndex -lt 0) { return }
    $p = $script:places[$list.SelectedIndex]
    $config | Add-Member -NotePropertyName latitude -NotePropertyValue ([math]::Round([double]$p.latitude, 2)) -Force
    $config | Add-Member -NotePropertyName longitude -NotePropertyValue ([math]::Round([double]$p.longitude, 2)) -Force
    $config | Add-Member -NotePropertyName locationSource -NotePropertyValue 'manual' -Force
    $config | Add-Member -NotePropertyName locationName -NotePropertyValue ([string]$list.SelectedItem) -Force
    Save-And-Apply
    [void][Windows.Forms.MessageBox]::Show("Lembas now uses $($list.SelectedItem). The weather updates within a minute or two.", 'Lembas')
    $f.Close()
})
$phone.Add_Click({
    foreach ($k in 'locationSource', 'locationName') { $config.PSObject.Properties.Remove($k) }
    Save-And-Apply
    $sync = Join-Path $PSScriptRoot 'Lembas-Sync.ps1'
    if (Test-Path (Join-Path $env:APPDATA 'Lembas\google-token.dat')) { Start-Process powershell.exe -WindowStyle Hidden -ArgumentList "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$sync`" -Quiet" }
    [void][Windows.Forms.MessageBox]::Show("Lembas uses your phone's area when the phone shares it (Lembas settings on the phone > Weather for where you are, with sync on), and otherwise your time zone's main city.", 'Lembas')
    $f.Close()
})
[void]$f.ShowDialog()
