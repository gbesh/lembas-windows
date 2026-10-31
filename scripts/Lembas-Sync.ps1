<#
  Lembas Sync - keeps the look and its settings the same on this PC, your phone and your watch,
  through your own Google account.

  Lembas keeps one small file (lembas-sync.json) in a hidden folder of your Google Drive that only
  Lembas can see (the "drive.appdata" permission: it can't see any of your other files). The
  newest change wins: change the look on the phone and the PC follows within ten minutes, or at once
  when you change it here.

    Lembas-Sync.ps1 -SignIn     sign in with Google in your browser (once), then sync
    Lembas-Sync.ps1 -SignOut    forget the sign-in on this PC
    Lembas-Sync.ps1             sync now (run by Lembas Sun every 10 minutes, and after changes)

  Synced: the look (or "follow the sun"), "keep it until", weather in the scene, the clock style
  and the hobbit mealtimes. The sign-in is kept encrypted for your Windows account only.
#>
param([switch]$SignIn, [switch]$SignOut, [switch]$Quiet, [switch]$NoApply)

$ErrorActionPreference = 'Stop'
$Dest = Split-Path -Parent $PSScriptRoot
$configFile = Join-Path $Dest 'lembas.json'
$roam = Join-Path $env:APPDATA 'Lembas'
# Lembas's Google sign-in ID: kept on this PC only (never in the public code)
$clientFile = @((Join-Path $env:USERPROFILE '.lembas\google-client.json'), (Join-Path $roam 'google-client.json')) |
    Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $clientFile) { $clientFile = Join-Path $env:USERPROFILE '.lembas\google-client.json' }
$tokenFile = Join-Path $roam 'google-token.dat'
$stateFile = Join-Path $Dest 'sync-state.json'
$logFile = Join-Path $Dest 'sync.log'
$FileName = 'lembas-sync.json'
$Scope = 'https://www.googleapis.com/auth/drive.appdata'

function Log($text) {
    if ((Test-Path $logFile) -and (Get-Item $logFile).Length -gt 100KB) { Remove-Item $logFile -Force }
    Add-Content -Path $logFile -Value ("[{0:yyyy-MM-dd HH:mm:ss}] {1}" -f (Get-Date), $text) -ErrorAction SilentlyContinue
    if (-not $Quiet) { Write-Host "  $text" }
}
function Say($text, $color = 'Gray') { if (-not $Quiet) { Write-Host "  $text" -ForegroundColor $color } }
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

if (-not (Test-Path -LiteralPath $clientFile)) {
    Log "no Google sign-in ID on this PC ($clientFile); sync is off"
    if ($SignIn) { Say "Lembas can't sign in to Google on this PC yet: its sign-in ID is missing." 'Yellow'; Start-Sleep 4 }
    exit 0
}
$client = Get-Content -LiteralPath $clientFile -Raw | ConvertFrom-Json

# ------------------------------------------------------------------ the sign-in (kept with Windows' own encryption, DPAPI)
function Save-Refresh([string]$token) {
    New-Item -ItemType Directory -Force -Path $roam | Out-Null
    $sec = ConvertTo-SecureString $token -AsPlainText -Force
    ConvertFrom-SecureString $sec | Set-Content $tokenFile
}
function Read-Refresh {
    if (-not (Test-Path $tokenFile)) { return $null }
    try {
        $sec = Get-Content $tokenFile | ConvertTo-SecureString
        return [Runtime.InteropServices.Marshal]::PtrToStringBSTR([Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec))
    } catch { return $null }
}
function B64Url([byte[]]$b) { [Convert]::ToBase64String($b).TrimEnd('=').Replace('+', '-').Replace('/', '_') }

if ($SignOut) {
    $r = Read-Refresh
    if ($r) { try { Invoke-RestMethod -Method Post -Uri "https://oauth2.googleapis.com/revoke?token=$([uri]::EscapeDataString($r))" | Out-Null } catch { } }
    Remove-Item $tokenFile -Force -ErrorAction SilentlyContinue
    Remove-Item $stateFile -Force -ErrorAction SilentlyContinue
    Log "signed out"
    Say "Lembas sync is off on this PC." 'Green'
    Start-Sleep 2
    exit 0
}

if ($SignIn) {
    # The standard sign-in for desktop apps: your browser opens Google's page; Google sends the
    # answer back to a one-off address on this PC (127.0.0.1), which Lembas listens on briefly.
    $verifierBytes = New-Object byte[] 48; [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($verifierBytes)
    $verifier = B64Url $verifierBytes
    $challenge = B64Url ([Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::ASCII.GetBytes($verifier)))
    $stateBytes = New-Object byte[] 16; [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($stateBytes)
    $state = B64Url $stateBytes
    # a one-off listener on this PC only (127.0.0.1), for Google's answer
    $tcp = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 0); $tcp.Start(); $port = $tcp.LocalEndpoint.Port
    $redirect = "http://127.0.0.1:$port/"
    $url = "https://accounts.google.com/o/oauth2/v2/auth?response_type=code&access_type=offline&prompt=consent" +
        "&client_id=$([uri]::EscapeDataString($client.client_id))&redirect_uri=$([uri]::EscapeDataString($redirect))" +
        "&scope=$([uri]::EscapeDataString($Scope))&code_challenge=$challenge&code_challenge_method=S256&state=$state"
    Say ""
    Say "LEMBAS SYNC" 'Yellow'
    Say "Your browser is opening Google's sign-in. Choose your account and allow Lembas"
    Say "to keep its settings file in your Drive. (It can't see your other files.)"
    Log "sign-in: waiting for the browser on port $port"
    Start-Process $url
    $code = $null; $err = $null; $deadline = (Get-Date).AddMinutes(5)
    while (-not $code -and -not $err -and (Get-Date) -lt $deadline) {
        if (-not $tcp.Pending()) { Start-Sleep -Milliseconds 200; continue }
        $client2 = $tcp.AcceptTcpClient()
        $stream = $client2.GetStream()
        $reader = New-Object IO.StreamReader($stream)
        $first = $reader.ReadLine()          # GET /?state=..&code=.. HTTP/1.1
        $target = if ($first -match '^GET (\S+)') { $Matches[1] } else { '/' }
        $query = @{}
        if ($target.Contains('?')) {
            foreach ($pair in $target.Substring($target.IndexOf('?') + 1).Split('&')) {
                $kv = $pair.Split('=', 2)
                if ($kv.Count -eq 2) { $query[$kv[0]] = [uri]::UnescapeDataString($kv[1].Replace('+', ' ')) }
            }
        }
        $ok = $query['code'] -and $query['state'] -eq $state
        if ($ok) { $code = $query['code'] } elseif ($query['error']) { $err = $query['error'] }
        $page = if ($ok) { 'Lembas is signed in. You can close this tab.' } elseif ($err) { 'Lembas was not signed in. You can close this tab and try again.' } else { 'Lembas' }
        $html = "<html><body style='font-family:Segoe UI;background:#0B1020;color:#F4ECD8;display:flex;align-items:center;justify-content:center;height:100vh;margin:0'><h2 style='font-weight:400'>$page</h2></body></html>"
        $bytes = [Text.Encoding]::UTF8.GetBytes("HTTP/1.1 200 OK`r`nContent-Type: text/html; charset=utf-8`r`nContent-Length: $([Text.Encoding]::UTF8.GetByteCount($html))`r`nConnection: close`r`n`r`n$html")
        $stream.Write($bytes, 0, $bytes.Length); $stream.Flush(); $client2.Close()
    }
    $tcp.Stop()
    $ok = [bool]$code
    $qs = @{ code = $code; error = $err }
    if (-not $ok) { Log "sign-in cancelled ($($qs['error']))"; Say "Not signed in." 'Yellow'; Start-Sleep 3; exit 1 }
    try {
    $tok = Invoke-RestMethod -Method Post -Uri 'https://oauth2.googleapis.com/token' -Body @{
        code = $qs['code']; client_id = $client.client_id; client_secret = $client.client_secret
        redirect_uri = $redirect; grant_type = 'authorization_code'; code_verifier = $verifier
    }
    } catch { Log "sign-in: Google refused the code: $($_.ErrorDetails.Message) $($_.Exception.Message)"; Say "Google didn't accept the sign-in; try again." 'Yellow'; Start-Sleep 5; exit 1 }
    if (-not $tok.refresh_token) { Log "sign-in: no refresh token"; Say "Google didn't hand back a lasting sign-in; try again." 'Yellow'; Start-Sleep 4; exit 1 }
    Save-Refresh $tok.refresh_token
    Remove-Item $stateFile -Force -ErrorAction SilentlyContinue
    Log "signed in"
    Say "Signed in. Syncing now..." 'Green'
}

$refresh = Read-Refresh
if (-not $refresh) { exit 0 }   # not signed in: nothing to do

# ------------------------------------------------------------------ Drive (the hidden app folder only)
function Get-Access {
    try {
        (Invoke-RestMethod -Method Post -Uri 'https://oauth2.googleapis.com/token' -Body @{
            client_id = $client.client_id; client_secret = $client.client_secret; refresh_token = $refresh; grant_type = 'refresh_token'
        }).access_token
    } catch {
        $msg = "$($_.ErrorDetails.Message)"
        if ($msg -match 'invalid_grant') { Log "the Google sign-in was removed; sign in again from the tray"; Remove-Item $tokenFile -Force -ErrorAction SilentlyContinue }
        else { Log "offline? $($_.Exception.Message)" }
        return $null
    }
}
$access = Get-Access
if (-not $access) { exit 0 }
$auth = @{ Authorization = "Bearer $access" }

function Find-Remote {
    $q = [uri]::EscapeDataString("name='$FileName'")
    $r = Invoke-RestMethod -Headers $auth -Uri "https://www.googleapis.com/drive/v3/files?spaces=appDataFolder&q=$q&fields=files(id,modifiedTime)"
    if ($r.files.Count -gt 0) { $r.files[0] } else { $null }
}
function Read-Remote($id) {
    $raw = Invoke-WebRequest -UseBasicParsing -Headers $auth -Uri "https://www.googleapis.com/drive/v3/files/$id`?alt=media"
    [Text.Encoding]::UTF8.GetString($raw.RawContentStream.ToArray()) | ConvertFrom-Json
}
function Write-Remote($id, $obj) {
    $json = $obj | ConvertTo-Json -Compress
    if ($id) {
        Invoke-RestMethod -Method Patch -Headers $auth -ContentType 'application/json; charset=utf-8' `
            -Uri "https://www.googleapis.com/upload/drive/v3/files/$id`?uploadType=media" -Body ([Text.Encoding]::UTF8.GetBytes($json)) | Out-Null
    } else {
        $b = "lembas" + [guid]::NewGuid().ToString('N')
        $meta = @{ name = $FileName; parents = @('appDataFolder') } | ConvertTo-Json -Compress
        $body = "--$b`r`nContent-Type: application/json; charset=UTF-8`r`n`r`n$meta`r`n--$b`r`nContent-Type: application/json`r`n`r`n$json`r`n--$b--"
        Invoke-RestMethod -Method Post -Headers $auth -ContentType "multipart/related; boundary=$b" `
            -Uri 'https://www.googleapis.com/upload/drive/v3/files?uploadType=multipart' -Body ([Text.Encoding]::UTF8.GetBytes($body)) | Out-Null
    }
}

# ------------------------------------------------------------------ this PC's settings <-> the shared file
$config = Get-Content $configFile -Raw | ConvertFrom-Json
function Get-Prop($o, $name, $default) { if ($o.PSObject.Properties[$name] -and $null -ne $o.$name) { $o.$name } else { $default } }
function Local-Settings {
    [ordered]@{
        look = [string](Get-Prop $config 'look' 'adaptive')
        keepUntil = [long](Get-Prop $config 'keepUntil' 0)
        weather = [bool](Get-Prop $config 'weather' $true)
        clock = [string](Get-Prop $config 'desktopClockStyle' 'modern')
        meals = [bool](Get-Prop $config 'meals' $true)
    }
}
function Sig($s) { "{0}|{1}|{2}|{3}|{4}" -f $s.look, $s.keepUntil, $s.weather, $s.clock, $s.meals }
$syncState = if (Test-Path $stateFile) { Get-Content $stateFile -Raw | ConvertFrom-Json } else { [pscustomobject]@{ sig = ''; remoteUpdated = 0 } }

try {
    $mine = Local-Settings
    $changedHere = (Sig $mine) -ne $syncState.sig
    $remoteMeta = Find-Remote
    $remote = if ($remoteMeta) { Read-Remote $remoteMeta.id } else { $null }
    $remoteUpdated = if ($remote) { [long](Get-Prop $remote 'updated' 0) } else { 0 }
    $changedThere = $remoteUpdated -gt [long]$syncState.remoteUpdated
    $now = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()

    if ($remote -and $changedThere -and -not ($changedHere -and $syncState.sig -ne '')) {
        # take the other device's settings
        $config | Add-Member -NotePropertyName look -NotePropertyValue ([string]$remote.look) -Force
        $config | Add-Member -NotePropertyName keepUntil -NotePropertyValue ([long](Get-Prop $remote 'keepUntil' 0)) -Force
        $config | Add-Member -NotePropertyName weather -NotePropertyValue ([bool](Get-Prop $remote 'weather' $true)) -Force
        $config | Add-Member -NotePropertyName desktopClockStyle -NotePropertyValue ([string](Get-Prop $remote 'clock' 'modern')) -Force
        $config | Add-Member -NotePropertyName meals -NotePropertyValue ([bool](Get-Prop $remote 'meals' $true)) -Force
        $config | ConvertTo-Json -Depth 5 | Set-Content $configFile -Encoding UTF8
        $mine = Local-Settings
        Log ("took the settings from your {0}: look {1}" -f (Get-Prop $remote 'by' 'other device'), $mine.look)
        # apply: the look now, and tell Lembas Desktop to re-read its settings
        if (-not $NoApply) { & (Join-Path $PSScriptRoot 'LembasSun.ps1') -SkipSync }   # (Lembas Sun applies it itself when it called us)
        $exe = Join-Path $Dest 'Living\LembasDesktop.exe'
        if (Test-Path $exe) { Start-Process $exe -ArgumentList '--reload' -WindowStyle Hidden }
        $syncState = [pscustomobject]@{ sig = (Sig $mine); remoteUpdated = $remoteUpdated }
    } elseif ($changedHere -or -not $remote) {
        $out = [ordered]@{ v = 1; updated = $now; by = 'PC' }
        foreach ($k in $mine.Keys) { $out[$k] = $mine[$k] }
        Write-Remote ($(if ($remoteMeta) { $remoteMeta.id } else { $null })) $out
        Log ("sent this PC's settings: look {0}" -f $mine.look)
        $syncState = [pscustomobject]@{ sig = (Sig $mine); remoteUpdated = $now }
    } else {
        $syncState = [pscustomobject]@{ sig = (Sig $mine); remoteUpdated = $remoteUpdated }
    }
    $syncState | ConvertTo-Json | Set-Content $stateFile -Encoding UTF8
    if ($SignIn) { Say "Done. This PC now keeps the same look as your phone and watch." 'Green'; Start-Sleep 3 }
} catch {
    Log "sync failed: $($_.Exception.Message)"
    if ($SignIn) { Say "Couldn't reach Google Drive just now; Lembas will try again in 10 minutes." 'Yellow'; Start-Sleep 4 }
}
