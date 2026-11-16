# setup-plugins.ps1 - [PlugSetup] Set up UAD plug-in hosting for Apollo Control on this PC (a release install).
#
# What a developer does by hand (plugins\README.md, "Commands") as one resumable flow, for the user's own profile:
#   check     a 64-bit Windows, UA's UAD install (read only), free disk space
#   python    Python 3.13.17 x64, python.org's embeddable package, SHA-256 pinned below (python.org's release page)
#   packages  pip 26.2.2 (run from its wheel, offline) installs numpy 2.5.4, unicorn 2.1.4, capstone 5.0.9 and
#             pefile 2024.8.26 from the downloaded wheels with --require-hashes (SHA-256 pinned below, from PyPI)
#   tools     the mapper code (this folder: the vendored open-apollo plus our ports) copied next to the maps
#   specs     open-apollo's pinned panel specs: the tarball plugin-panels.conf pins, downloaded here and checked against
#             its SHA-256, then tools\install_panels.py --from (it checks the pin again and installs)
#   store     the module store from the user's own UAD install (tools\oa_assets_dsp.py dsp and strips)
#   licence   the plug-ins to map: those the units license (render ids from Apollo Control), or every one
#   map       the mapper (python -m plugin_mapper, absolute paths), mono and stereo maps; resumable
#   modules   the maps' DSP modules into System32\drivers\ApolloModules through Apollo Control's own elevated step
#             (apollo-control.exe --plugin-admin, what Settings > Install modules runs; one administrator prompt),
#             only while plug-in hosting is on (turning it on copies them otherwise)
#
# Everything it makes is under -Root (default %LOCALAPPDATA%\Avium\ApolloPlugins, where Apollo Control looks):
#   python\ tools\ downloads\ specs\ store\ bundles\ work\ generated\ setup\ (status.json, setup.log, state.txt)
# Nothing goes to Program Files, the install folder or the source tree. A second run continues: every finished
# step is checked and skipped, the mapper keeps what it mapped (and the plug-ins that failed with the same files).
#
# Network (this script only; no Python process downloads anything): www.python.org and files.pythonhosted.org, and
# the github.com release URL tools\open-apollo\configs\plugin-panels.conf pins (with GitHub's release-asset host it
# redirects to). Any other redirect is refused, and a Universal Audio domain always. No UA program, service or SDK DLL is started or loaded: the store builder reads the DLLs as
# bytes, the mapper runs plug-in code only inside the Unicorn emulator (plugins\README.md, "Nothing here talks to UA").
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File setup-plugins.ps1 [options]
#     -Root DIR            the per-user plug-in folder (default %LOCALAPPDATA%\Avium\ApolloPlugins)
#     -UadRoot DIR         the Windows root the UAD install is under (default %SystemDrive%\)
#     -AuthorizedIds HEX   render ids the units' licence answers admit (Apollo Control passes them), comma separated
#     -Authorized all|FILE map every plug-in open-apollo knows, or the names in FILE; default: the names an earlier
#                          run kept (setup\authorized.txt), else all
#     -AppExe EXE          apollo-control.exe for the elevated module step (default: found above this folder)
#     -Yes                 no questions (Apollo Control runs it so; progress goes to setup\status.json)
#     -SkipModules | -InstallModules   leave the driver's module store alone | copy even with hosting off
#     -Workers N           plug-ins mapped at once (default: CPU threads - 1)
#     -RetryFailures       map again plug-ins that failed with the same files
#     -Only NAME[,NAME]    tests: map just these (the licence step is skipped)
#     -StopAfter PHASE     tests: stop after python, packages, tools, specs, store, licence or map
#     -SelfTest            offline checks of this script (and setup_plugins.py when a Python is at hand)
# Exit: 0 done, 1 failed, 2 bad option, 3 another setup runs, 4 cancelled (setup\cancel), 5 no UAD install.
# PowerShell 5.1, ASCII. SPDX-License-Identifier: GPL-2.0-only
[CmdletBinding()]
param(
    [string]$Root = '',
    [string]$UadRoot = '',
    [string]$AuthorizedIds = '',
    [string]$Authorized = '',
    [string]$AppExe = '',
    [switch]$Yes,
    [switch]$SkipModules,
    [switch]$InstallModules,
    [int]$Workers = 0,
    [switch]$RetryFailures,
    [string[]]$Only = @(),
    [string]$StopAfter = '',
    [switch]$SelfTest
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version 2.0

# ---------------------------------------------------------------- the pins (change together, with their sources)
# Python: https://www.python.org/downloads/release/python-31317/ ("SHA-256 checksum" column, Windows embeddable
# package (64-bit)). Wheels: PyPI's JSON API (https://pypi.org/pypi/<name>/<version>/json, "digests.sha256"); pip's
# also on python.org's 3.13.17 release page. Byte-identical (RECORD) to the developer venv's packages, Nov 16 2026.
$Pins = @{
    Python   = @{ File = 'python-3.13.17-embed-amd64.zip'; Version = '3.13.17'; Size = 11441267
                  Url = 'https://www.python.org/ftp/python/3.13.17/python-3.13.17-embed-amd64.zip'
                  Sha256 = '9e35319bffedab8b36ad44d8f377cb4f1733bcec257a0b6d04d94f01486ddb38' }
    Pip      = @{ File = 'pip-26.2.2-py3-none-any.whl'; Version = '26.2.2'; Size = 1819850
                  Url = 'https://files.pythonhosted.org/packages/3f/06/3e4420302363fbb71be3c7f35ce042f54bfca330e3e66b3a2e3c4cca579f/pip-26.2.2-py3-none-any.whl'
                  Sha256 = 'fd7f4dfa8190e014013b9f08f3e973f4e4c26ffd179a204895533a7254813a70' }
    Wheels   = @(
        @{ Name = 'numpy'; Version = '2.5.4'; File = 'numpy-2.5.4-cp313-cp313-win_amd64.whl'; Size = 12566113
           Url = 'https://files.pythonhosted.org/packages/90/8c/140a40398a66b4471211be1affdb6ed24c486d581bd28d07b7f2fcb69540/numpy-2.5.4-cp313-cp313-win_amd64.whl'
           Sha256 = '8b4d2fd2d34e5f8c9235ee787de5631a37a28402b15cb80814df973d2be54129' },
        @{ Name = 'unicorn'; Version = '2.1.4'; File = 'unicorn-2.1.4-cp37-abi3-win_amd64.whl'; Size = 15943079
           Url = 'https://files.pythonhosted.org/packages/70/3d/de7be9bd1addabe6d8a1369381f8a080400c349850e978689c5e18287957/unicorn-2.1.4-cp37-abi3-win_amd64.whl'
           Sha256 = 'd7107500c64ce5c168fbff6bef9485b5db1350050036f4cea568650cf8bdbdf5' },
        @{ Name = 'capstone'; Version = '5.0.9'; File = 'capstone-5.0.9-py3-none-win_amd64.whl'; Size = 1273459
           Url = 'https://files.pythonhosted.org/packages/50/e6/6f06fdb6a9ed32b2f7cd9c036b92d5324112c3ef7080f2c71efc367d40dd/capstone-5.0.9-py3-none-win_amd64.whl'
           Sha256 = '732cedbbb56d42e723f14d7af6387f1454194a820b4b96b56d1e53f865ef85d0' },
        @{ Name = 'pefile'; Version = '2024.8.26'; File = 'pefile-2024.8.26-py3-none-any.whl'; Size = 74766
           Url = 'https://files.pythonhosted.org/packages/54/16/12b82f791c7f50ddec566873d5bdd245baa1491bac11d15ffb98aecc8f8b/pefile-2024.8.26-py3-none-any.whl'
           Sha256 = '76f8b485dcd3b1bb8166f1128d395fa3d87af26360c2358fb75b80019b957c6f' }
    )
}
$AllowedHosts = @('www.python.org', 'files.pythonhosted.org')
# the pinned PANELS_URL's host, and the host GitHub redirects a release asset to (the panel specs only). This script
# downloads the tarball (Windows' certificate chain engine: a fresh Windows fetches GitHub's root on demand, which
# Python's ssl, reading only the roots already in the store, cannot) and hands it to install_panels.py --from.
$PanelHosts = @('github.com')
$PanelRedirectHosts = @('github.com', 'release-assets.githubusercontent.com', 'objects.githubusercontent.com')
$Phases = @(
    @{ Id = 'check';    Label = 'Checking this PC';                Weight = 1 },
    @{ Id = 'python';   Label = 'Python 3.13.17 runtime';          Weight = 6 },
    @{ Id = 'packages'; Label = 'Python packages';                 Weight = 8 },
    @{ Id = 'tools';    Label = 'Mapper code';                     Weight = 1 },
    @{ Id = 'specs';    Label = 'Panel specs';                     Weight = 2 },
    @{ Id = 'store';    Label = 'Module store from your UAD install'; Weight = 7 },
    @{ Id = 'licence';  Label = 'Plug-ins to map';                 Weight = 1 },
    @{ Id = 'map';      Label = 'Mapping plug-ins';                Weight = 70 },
    @{ Id = 'modules';  Label = 'Driver module store';             Weight = 4 }
)
$MinFreeBytes = 700MB
$SetupSchema = 'apollo-plugin-setup/1'
# set by the phases (strict mode: defined up front)
$script:Uad = $null; $script:stripNote = ''; $script:pyver = ''; $script:pkgver = ''; $script:sum = $null
$script:namesOut = ''; $script:modOut = ''; $script:stFail = 0
$Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

# ---------------------------------------------------------------- small helpers

# A failure with a code Apollo Control shows (network, hash, nouad, space, python, packages, specs, store, map, ...).
function New-SetupError([string]$Code, [string]$Message) {
    $e = New-Object System.Exception $Message
    $e.Data['SetupCode'] = $Code
    return $e
}

function Join([string]$a, [string]$b) { return [IO.Path]::Combine($a, $b) }

function Get-Sha256([string]$Path) { return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant() }

# Windows command-line quoting (CommandLineToArgvW / the MSVC runtime): "C:\" must become "C:\\".
function Quote-Arg([string]$s) {
    if ($s -ne '' -and $s -notmatch '[\s"]') { return $s }
    $out = New-Object Text.StringBuilder
    [void]$out.Append('"')
    $bs = 0
    foreach ($ch in $s.ToCharArray()) {
        if ($ch -eq '\') { $bs++; continue }
        if ($ch -eq '"') { [void]$out.Append('\', 2 * $bs + 1); [void]$out.Append('"'); $bs = 0; continue }
        if ($bs) { [void]$out.Append('\', $bs); $bs = 0 }
        [void]$out.Append($ch)
    }
    if ($bs) { [void]$out.Append('\', 2 * $bs) }
    [void]$out.Append('"')
    return $out.ToString()
}

# JSON text of a value (ordered dictionaries, arrays, strings, numbers, booleans, null), ASCII only: anything
# outside printable ASCII becomes \uXXXX, so any reader sees the same text.
function ConvertTo-JsonText($v) {
    if ($null -eq $v) { return 'null' }
    if ($v -is [bool]) { if ($v) { return 'true' } else { return 'false' } }
    if ($v -is [int] -or $v -is [long] -or $v -is [double] -or $v -is [decimal]) {
        return [string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0}', $v)
    }
    if ($v -is [string] -or $v -is [char]) {
        $sb = New-Object Text.StringBuilder
        [void]$sb.Append('"')
        foreach ($c in ([string]$v).ToCharArray()) {
            $n = [int]$c
            if ($c -eq '"') { [void]$sb.Append('\"') }
            elseif ($c -eq '\') { [void]$sb.Append('\\') }
            elseif ($n -eq 10) { [void]$sb.Append('\n') }
            elseif ($n -eq 13) { [void]$sb.Append('\r') }
            elseif ($n -eq 9) { [void]$sb.Append('\t') }
            elseif ($n -lt 32 -or $n -gt 126) { [void]$sb.Append(('\u{0:x4}' -f $n)) }
            else { [void]$sb.Append($c) }
        }
        [void]$sb.Append('"')
        return $sb.ToString()
    }
    if ($v -is [Collections.IDictionary]) {
        $parts = foreach ($k in $v.Keys) { (ConvertTo-JsonText ([string]$k)) + ':' + (ConvertTo-JsonText $v[$k]) }
        return '{' + (@($parts) -join ',') + '}'
    }
    if ($v -is [Collections.IEnumerable]) {
        $parts = foreach ($x in $v) { ConvertTo-JsonText $x }
        return '[' + (@($parts) -join ',') + ']'
    }
    return ConvertTo-JsonText ([string]$v)
}

# Mapper and pip lines carry a few non-ASCII marks; the console and the status file get ASCII.
function To-Ascii([string]$s) {
    if ($null -eq $s) { return '' }
    $s = $s.Replace([string][char]0x2026, '...').Replace([string][char]0x00B7, ';').Replace([string][char]0x2013, '-').Replace([string][char]0x2014, '-').Replace([string][char]0x2500, '-')
    return ($s.ToCharArray() | ForEach-Object { if ([int]$_ -ge 32 -and [int]$_ -le 126) { $_ } else { '?' } }) -join ''
}

function Test-AllowedUrl([string]$Url, [string[]]$Hosts) {
    $u = $null
    if (-not [Uri]::TryCreate($Url, [UriKind]::Absolute, [ref]$u)) { return $false }
    if ($u.Scheme -ne 'https' -or $u.UserInfo -ne '' -or -not $u.IsDefaultPort) { return $false }
    $h = $u.Host.ToLowerInvariant()
    if ($h -match '(^|\.)(uaudio|universalaudio|ua-?connect)\.') { return $false }     # never, whatever the list says
    return ($Hosts -contains $h)
}

# Read a file other processes may be replacing (the status file, the mapper's progress): share everything.
function Read-Shared([string]$Path) {
    $fs = $null
    try {
        $fs = New-Object IO.FileStream($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8)
        return $sr.ReadToEnd()
    } catch { return $null } finally { if ($fs) { $fs.Dispose() } }
}

function Write-Atomic([string]$Path, [string]$Text) {
    $tmp = "$Path.tmp"
    [IO.File]::WriteAllText($tmp, $Text, (New-Object Text.UTF8Encoding $false))
    for ($i = 0; $i -lt 20; $i++) {
        try {
            # [NullString]: PowerShell would pass $null to a string parameter as "" (no backup file wanted)
            if (Test-Path -LiteralPath $Path) { [IO.File]::Replace($tmp, $Path, [NullString]::Value) } else { [IO.File]::Move($tmp, $Path) }
            return
        } catch { Start-Sleep -Milliseconds 100 }
    }
    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------- state, status, log

$S = @{
    Root = ''; SetupDir = ''; Log = ''; StatusPath = ''; Lock = $null; State = @{}; Phase = ''; PhaseFrac = 0.0
    PhaseState = @{}; PhaseDetail = @{}; Text = ''; Done = 0; Total = 0; Eta = -1; Code = ''; Error = ''
    Summary = ''; Modules = ''; Mode = ''; Started = (Get-Date).ToString('s'); LastSave = [DateTime]::MinValue
    Interactive = $false; Runtime = ''; Python = ''; Tools = ''; Child = $null; LastConsole = ''
}

function Write-Log([string]$Text) {
    $line = '{0}  {1}' -f (Get-Date).ToString('yyyy-MM-dd HH:mm:ss'), (To-Ascii $Text)
    if ($S.Log) { try { [IO.File]::AppendAllText($S.Log, $line + "`r`n", [Text.Encoding]::ASCII) } catch { } }
}

function Say([string]$Text) {
    Write-Log $Text
    if ($S.Interactive) { Write-Host (To-Ascii $Text) }
}

function Save-Status([string]$State = 'running', [switch]$Force) {
    if (-not $S.StatusPath) { return }
    $now = Get-Date
    if (-not $Force -and $State -eq 'running' -and ($now - $S.LastSave).TotalMilliseconds -lt 400) { return }
    $S.LastSave = $now
    $pct = 0.0
    foreach ($p in $Phases) {
        $st = $S.PhaseState[$p.Id]
        if ($st -eq 'done' -or $st -eq 'skipped') { $pct += $p.Weight }
        elseif ($p.Id -eq $S.Phase) { $pct += $p.Weight * [Math]::Max(0.0, [Math]::Min(1.0, $S.PhaseFrac)) }
    }
    if ($State -eq 'done') { $pct = 100 }
    $phases = foreach ($p in $Phases) {
        [ordered]@{ id = $p.Id; label = $p.Label; state = $(if ($S.PhaseState[$p.Id]) { $S.PhaseState[$p.Id] } else { 'pending' })
                    detail = [string]$S.PhaseDetail[$p.Id] }
    }
    $o = [ordered]@{
        schema = $SetupSchema; state = $State; phase = $S.Phase; phases = @($phases); text = (To-Ascii $S.Text)
        percent = [int][Math]::Floor($pct); done = [int]$S.Done; total = [int]$S.Total; eta = [int]$S.Eta
        code = $S.Code; error = (To-Ascii $S.Error); summary = (To-Ascii $S.Summary); modules = (To-Ascii $S.Modules)
        mode = $S.Mode; log = $S.Log; root = $S.Root; pid = $PID
        beat = [long]([DateTimeOffset]::UtcNow.ToUnixTimeSeconds()); started = $S.Started; updated = $now.ToString('s')
    }
    Write-Atomic $S.StatusPath ((ConvertTo-JsonText $o) + "`n")
}

function Set-Phase([string]$Id, [string]$State, [string]$Detail = '') {
    $S.PhaseState[$Id] = $State
    if ($Detail) { $S.PhaseDetail[$Id] = $Detail }
    if ($State -eq 'running') {
        $S.Phase = $Id; $S.PhaseFrac = 0.0; $S.Done = 0; $S.Total = 0; $S.Eta = -1
        $label = ($Phases | Where-Object { $_.Id -eq $Id }).Label
        $S.Text = $label
        Say "== $label"
    } elseif ($Detail) {
        Say "   $Detail"
    }
    Save-Status -Force
}

function Get-StateValue([string]$Key) { if ($S.State.ContainsKey($Key)) { return $S.State[$Key] } return '' }
function Set-StateValue([string]$Key, [string]$Value) {
    $S.State[$Key] = $Value
    $lines = foreach ($k in ($S.State.Keys | Sort-Object)) { "$k=$($S.State[$k])" }
    Write-Atomic (Join $S.SetupDir 'state.txt') ((@($lines) -join "`r`n") + "`r`n")
}

function Test-Cancel {
    if ($S.SetupDir -and (Test-Path -LiteralPath (Join $S.SetupDir 'cancel'))) {
        throw (New-SetupError 'cancelled' 'Cancelled. Set up plug-ins again continues where it stopped.')
    }
}

# ---------------------------------------------------------------- downloads (pinned, hashed, allowed hosts only)

# $Pin: File, Url, Sha256, Size (0: unknown, then at most 64 MB). $Hosts: where it may start, $Redirects: where it may go.
function Get-Pinned($Pin, [string]$Dir, [double]$From, [double]$To, [string[]]$Hosts = $AllowedHosts, [string[]]$Redirects = $AllowedHosts) {
    if (-not (Test-AllowedUrl $Pin.Url $Hosts)) { throw (New-SetupError 'network' "Refusing to download from $($Pin.Url)") }
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    $dst = Join $Dir $Pin.File
    if (Test-Path -LiteralPath $dst) {
        if ((Get-Sha256 $dst) -eq $Pin.Sha256) { Write-Log "kept $($Pin.File) (SHA-256 matches the pin)"; $S.PhaseFrac = $To; return $dst }
        Remove-Item -LiteralPath $dst -Force
    }
    $part = "$dst.part"
    $url = $Pin.Url
    $resp = $null
    for ($hop = 0; $hop -lt 4; $hop++) {
        Test-Cancel
        $req = [Net.HttpWebRequest]::Create($url)
        $req.AllowAutoRedirect = $false
        $req.UserAgent = 'apollo-plugin-setup'
        $req.Timeout = 30000
        $req.ReadWriteTimeout = 60000
        try { $resp = $req.GetResponse() }
        catch [Net.WebException] {
            if ($_.Exception.Response) { $resp = $_.Exception.Response }
            else { throw (New-SetupError 'network' "Could not download $($Pin.File): $($_.Exception.Message)") }
        }
        $code = [int]$resp.StatusCode
        if ($code -ge 300 -and $code -lt 400) {
            $loc = $resp.Headers['Location']
            $resp.Close()
            $next = (New-Object Uri((New-Object Uri $url), $loc)).AbsoluteUri
            if (-not (Test-AllowedUrl $next $Redirects)) { throw (New-SetupError 'network' "Refusing a redirect of $($Pin.File) to $next") }
            $url = $next
            continue
        }
        if ($code -ne 200) { $resp.Close(); throw (New-SetupError 'network' "Could not download $($Pin.File): HTTP $code") }
        break
    }
    if ($code -ne 200) { throw (New-SetupError 'network' "Too many redirects for $($Pin.File)") }
    $expect = if ($Pin.Size -gt 0) { [double]$Pin.Size } elseif ($resp.ContentLength -gt 0) { [double]$resp.ContentLength } else { 1MB }
    Say ("   downloading {0} ({1:N1} MB) from {2}" -f $Pin.File, ($expect / 1MB), ([Uri]$url).Host)
    $in = $resp.GetResponseStream()
    $out = [IO.File]::Create($part)
    try {
        $buf = New-Object byte[] 65536
        $got = 0L
        $cap = if ($Pin.Size -gt 0) { [long]$Pin.Size + 1MB } else { 64MB }
        while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) {
            $out.Write($buf, 0, $n)
            $got += $n
            if ($got -gt $cap) { throw (New-SetupError 'hash' "$($Pin.File) is larger than pinned ($cap bytes at most)") }
            $S.PhaseFrac = $From + ($To - $From) * [Math]::Min(1.0, $got / $expect)
            $S.Text = ('Downloading {0}: {1:N1} of {2:N1} MB' -f $Pin.File, ($got / 1MB), ($expect / 1MB))
            Save-Status
            if (($got % 4MB) -lt $n) { Test-Cancel }
        }
    } finally { $out.Dispose(); $in.Dispose(); $resp.Close() }
    $sha = Get-Sha256 $part
    if ($sha -ne $Pin.Sha256 -or ($Pin.Size -gt 0 -and (Get-Item -LiteralPath $part).Length -ne $Pin.Size)) {
        Remove-Item -LiteralPath $part -Force
        throw (New-SetupError 'hash' "$($Pin.File) does not match its pinned SHA-256 (got $sha): not used")
    }
    Move-Item -LiteralPath $part -Destination $dst -Force
    Write-Log "downloaded $($Pin.File), SHA-256 $sha = pin"
    return $dst
}

# ---------------------------------------------------------------- the private runtime's processes

# python.exe of the runtime with a clean environment: no PYTHON* variable of the user's leaks in, PYTHONHOME pins the
# runtime (else Windows' registry entry of another Python 3.13 install would add its Lib and DLLs), no user site.
function Invoke-Py([string[]]$Arguments, [string]$WorkDir, [scriptblock]$OnLine = $null, [double]$Timeout = 0) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $S.Python
    $psi.Arguments = (@($Arguments | ForEach-Object { Quote-Arg $_ }) -join ' ')
    $psi.WorkingDirectory = $WorkDir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = New-Object Text.UTF8Encoding $false
    $psi.StandardErrorEncoding = New-Object Text.UTF8Encoding $false
    foreach ($k in @($psi.EnvironmentVariables.Keys)) { if ($k -like 'python*') { $psi.EnvironmentVariables.Remove($k) } }
    $psi.EnvironmentVariables['PYTHONHOME'] = $S.Runtime
    $psi.EnvironmentVariables['PYTHONNOUSERSITE'] = '1'
    $psi.EnvironmentVariables['PYTHONUTF8'] = '1'
    Write-Log ("run: python {0}  (in {1})" -f $psi.Arguments, $WorkDir)
    $p = [Diagnostics.Process]::Start($psi)
    $S.Child = $p
    $started = Get-Date
    $lastBeat = Get-Date
    $streams = @(@{ R = $p.StandardOutput; T = $null; Done = $false; Err = $false }, @{ R = $p.StandardError; T = $null; Done = $false; Err = $true })
    foreach ($st in $streams) { $st.T = $st.R.ReadLineAsync() }
    $killed = $false
    try {
        while (-not ($streams[0].Done -and $streams[1].Done)) {
            $any = $false
            foreach ($st in $streams) {
                if ($st.Done -or -not $st.T.IsCompleted) { continue }
                $line = $st.T.Result
                $any = $true
                if ($null -eq $line) { $st.Done = $true; continue }
                Write-Log ('  | ' + $line)
                if ($OnLine) { & $OnLine $line $st.Err }
                $st.T = $st.R.ReadLineAsync()
            }
            if ($any) { continue }
            Start-Sleep -Milliseconds 100
            if (((Get-Date) - $lastBeat).TotalSeconds -ge 5) { Save-Status -Force; $lastBeat = Get-Date }
            if (-not $killed) {
                $stop = $S.SetupDir -and (Test-Path -LiteralPath (Join $S.SetupDir 'cancel'))
                if ($Timeout -gt 0 -and ((Get-Date) - $started).TotalSeconds -gt $Timeout) { $stop = $true; Write-Log 'timed out' }
                if ($stop) { Stop-Tree $p.Id; $killed = $true }
            }
        }
        $p.WaitForExit()
        $rc = $p.ExitCode
    } finally {
        $S.Child = $null
        if (-not $p.HasExited) { Stop-Tree $p.Id }
        $p.Dispose()
    }
    Test-Cancel
    if ($killed) { throw (New-SetupError 'timeout' "A step took longer than $Timeout s and was stopped") }
    Write-Log "exit code $rc"
    return $rc
}

function Stop-Tree([int]$Id) {
    Write-Log "stopping process $Id and its children"
    $tk = Join ([Environment]::GetFolderPath('System')) 'taskkill.exe'
    # PowerShell 5.1 turns a native tool's stderr into error records under 2>&1: never terminating ones here
    $ErrorActionPreference = 'Continue'
    & $tk /PID $Id /T /F 2>&1 | ForEach-Object { Write-Log ('  | ' + $_) }
}

# ---------------------------------------------------------------- phases

function Get-UadPaths([string]$UadRootDir) {
    return @{
        Dlls = Join $UadRootDir 'Program Files\Universal Audio\Powered Plugins\UAD-2 Powered Plugins'
        Sdk  = Join $UadRootDir 'Windows\System32\UAD2SDK.dll'
    }
}

# UA's install, by file names only (nothing of it is opened here). $null when there is none.
function Find-Uad([string]$UadRootDir) {
    $u = Get-UadPaths $UadRootDir
    if (-not (Test-Path -LiteralPath $u.Dlls -PathType Container) -or -not (Test-Path -LiteralPath $u.Sdk -PathType Leaf)) { return $null }
    $dlls = @(Get-ChildItem -LiteralPath $u.Dlls -Filter '*.dll' -File -ErrorAction SilentlyContinue)
    if ($dlls.Count -eq 0) { return $null }
    $sdk = Get-Item -LiteralPath $u.Sdk
    $bytes = 0L; $newest = 0L
    foreach ($d in $dlls) { $bytes += $d.Length; $newest = [Math]::Max($newest, $d.LastWriteTimeUtc.Ticks) }
    $u.Count = $dlls.Count
    $u.Fingerprint = '{0}/{1}/{2}/{3}/{4}' -f $dlls.Count, $bytes, $newest, $sdk.Length, $sdk.LastWriteTimeUtc.Ticks
    return $u
}

function Phase-Check {
    Set-Phase 'check' 'running'
    if (-not [Environment]::Is64BitOperatingSystem -or -not [Environment]::Is64BitProcess) {
        throw (New-SetupError 'arch' 'Run this from the 64-bit Windows PowerShell (UAD plug-in hosting needs 64-bit Windows).')
    }
    $prepOnly = $StopAfter -and (@('python', 'packages', 'tools', 'specs') -contains $StopAfter)
    $uad = Find-Uad $UadRoot
    if (-not $uad -and -not $prepOnly) {
        $u = Get-UadPaths $UadRoot
        throw (New-SetupError 'nouad' ("No UAD install found on this PC (no UAD-2 Powered Plugins folder with plug-ins in " +
            "$($u.Dlls), or no $($u.Sdk)). Install UA's UAD software, then set up plug-ins again. Nothing was downloaded."))
    }
    $drive = New-Object IO.DriveInfo ([IO.Path]::GetPathRoot($S.Root))
    if ($drive.AvailableFreeSpace -lt $MinFreeBytes) {
        throw (New-SetupError 'space' ('{0} has {1:N0} MB free; setting up plug-ins needs about {2:N0} MB.' -f $drive.Name, ($drive.AvailableFreeSpace / 1MB), ($MinFreeBytes / 1MB)))
    }
    $script:Uad = $uad
    $msg = if ($uad) { "UAD install: $($uad.Count) Powered plug-in DLLs (read offline, as files)" } else { 'UAD install: not checked (preparation only)' }
    Set-Phase 'check' 'done' $msg
}

function Phase-Python {
    $pin = $Pins.Python
    $stamp = "$($pin.Version) $($pin.Sha256)"
    if ((Test-Path -LiteralPath $S.Python) -and (Get-StateValue 'runtime') -eq $stamp -and
        -not (Test-Path -LiteralPath (Join $S.Runtime 'python313._pth'))) {
        Set-Phase 'python' 'skipped' "Python $($pin.Version) is already in $($S.Runtime)"
        return
    }
    Set-Phase 'python' 'running'
    $zip = Get-Pinned $pin (Join $S.Root 'downloads') 0.0 0.8
    Test-Cancel
    $new = "$($S.Runtime).new"
    if (Test-Path -LiteralPath $new) { Remove-Item -LiteralPath $new -Recurse -Force }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $new)            # refuses entries outside the folder
    # The embeddable package's ._pth makes every run isolated with the script's folder off sys.path, which the
    # mapper's own child scripts need: without it the runtime is an ordinary one (Lib\site-packages, PYTHONHOME).
    Remove-Item -LiteralPath (Join $new 'python313._pth') -Force
    New-Item -ItemType Directory -Force -Path (Join $new 'Lib\site-packages') | Out-Null
    if (Test-Path -LiteralPath $S.Runtime) {
        try { Remove-Item -LiteralPath $S.Runtime -Recurse -Force }
        catch { throw (New-SetupError 'python' "The old runtime in $($S.Runtime) is in use (Apollo Control's parameter planner?): quit Apollo Control and try again.") }
    }
    Move-Item -LiteralPath $new -Destination $S.Runtime
    Set-StateValue 'runtime' $stamp
    Set-StateValue 'packages' ''
    $script:pyver = ''
    $rc = Invoke-Py @('-I', '-c', 'import sys; print(sys.version.split()[0], sys.maxsize > 2**32)') $S.Runtime { param($l, $e) if (-not $e) { $script:pyver = $l } }
    if ($rc -ne 0 -or $script:pyver -ne "$($pin.Version) True") { throw (New-SetupError 'python' "The Python runtime does not start ($rc, '$($script:pyver)')") }
    Set-Phase 'python' 'done' "Python $($pin.Version) (64-bit) in $($S.Runtime)"
}

function Phase-Packages {
    $req = (@($Pins.Wheels | ForEach-Object { '{0}=={1} --hash=sha256:{2}' -f $_.Name, $_.Version, $_.Sha256 }) -join "`n") + "`n"
    $stamp = (@($Pins.Wheels | ForEach-Object { "$($_.Name)==$($_.Version)" }) -join ',')
    # the four import, and their installed distributions are the pinned versions (capstone 5.0.9's own __version__ says 5.0.7)
    $probe = 'import numpy, unicorn, capstone, pefile; from importlib.metadata import version as v; print(*(v(n) for n in ("numpy", "unicorn", "capstone", "pefile")))'
    $want = (@($Pins.Wheels | ForEach-Object { $_.Version }) -join ' ')
    if ((Get-StateValue 'packages') -eq $stamp) {
        $script:pkgver = ''
        $rc = Invoke-Py @('-I', '-c', $probe) $S.Runtime { param($l, $e) if (-not $e) { $script:pkgver = $l } }
        if ($rc -eq 0 -and $script:pkgver -eq $want) { Set-Phase 'packages' 'skipped' "numpy, unicorn, capstone and pefile are installed ($want)"; return }
    }
    Set-Phase 'packages' 'running'
    $dir = Join $S.Root 'downloads\wheels'
    $all = @($Pins.Pip) + @($Pins.Wheels)
    $total = 0.0; foreach ($w in $all) { $total += $w.Size }
    $at = 0.0
    $pip = $null
    foreach ($w in $all) {
        $f = Get-Pinned $w $dir (0.8 * $at / $total) (0.8 * ($at + $w.Size) / $total)
        if ($w.File -eq $Pins.Pip.File) { $pip = $f }
        $at += $w.Size
    }
    # only the four wheels (pip runs from its own wheel and is not installed): offline, hashes required, no builds;
    # anything else in the wheel folder (an older pin's file) goes, so --find-links sees the pinned files only
    $keep = @($all | ForEach-Object { $_.File })
    @(Get-ChildItem -LiteralPath $dir -File | Where-Object { $keep -notcontains $_.Name }) | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }
    $reqFile = Join $S.SetupDir 'requirements.txt'
    [IO.File]::WriteAllText($reqFile, $req, [Text.Encoding]::ASCII)
    $S.Text = 'Installing numpy, unicorn, capstone and pefile (offline, hashes checked)'
    $S.PhaseFrac = 0.85
    Save-Status -Force
    $rc = Invoke-Py @('-I', (Join $pip 'pip'), 'install', '--no-index', '--find-links', $dir, '--require-hashes', '--only-binary', ':all:',
                      '--no-deps', '--no-cache-dir', '--disable-pip-version-check', '--no-warn-script-location', '--upgrade', '-r', $reqFile) $S.Runtime
    if ($rc -ne 0) { throw (New-SetupError 'packages' "pip could not install the packages (exit $rc): see $($S.Log)") }
    $script:pkgver = ''
    $rc = Invoke-Py @('-I', '-c', $probe) $S.Runtime { param($l, $e) if (-not $e) { $script:pkgver = $l } }
    if ($rc -ne 0 -or $script:pkgver -ne $want) { throw (New-SetupError 'packages' "The packages do not import ($rc, '$($script:pkgver)')") }
    Set-StateValue 'packages' $stamp
    Set-Phase 'packages' 'done' "numpy $($Pins.Wheels[0].Version), unicorn $($Pins.Wheels[1].Version), capstone $($Pins.Wheels[2].Version), pefile $($Pins.Wheels[3].Version)"
}

# The mapper code next to the maps (Apollo Control's planner and the mapper read it there, as in the developer
# layout). Source: this script's folder. Files that differ are copied, files gone from the source removed.
function Phase-Tools {
    Set-Phase 'tools' 'running'
    $src = (Resolve-Path -LiteralPath $PSScriptRoot).Path.TrimEnd('\')
    $dst = $S.Tools
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    if ((Resolve-Path -LiteralPath $dst).Path.TrimEnd('\') -eq $src) { Set-Phase 'tools' 'skipped' 'Running from the maps folder itself'; return }
    $isSource = { param($f, $base) $rel = $f.FullName.Substring($base.Length + 1); ($rel -notmatch '(^|\\)__pycache__(\\|$)') -and ($f.Extension -ne '.pyc') }
    $want = @{}
    foreach ($f in @(Get-ChildItem -LiteralPath $src -Recurse -File)) { if (& $isSource $f $src) { $want[$f.FullName.Substring($src.Length + 1)] = $f.FullName } }
    $copied = 0; $removed = 0
    foreach ($rel in $want.Keys) {
        $to = Join $dst $rel
        if ((Test-Path -LiteralPath $to) -and (Get-Item -LiteralPath $to).Length -eq (Get-Item -LiteralPath $want[$rel]).Length -and
            (Get-Sha256 $to) -eq (Get-Sha256 $want[$rel])) { continue }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to) | Out-Null
        Copy-Item -LiteralPath $want[$rel] -Destination $to -Force
        $copied++
    }
    $dstFull = (Resolve-Path -LiteralPath $dst).Path.TrimEnd('\')
    foreach ($f in @(Get-ChildItem -LiteralPath $dstFull -Recurse -File)) {
        if ((& $isSource $f $dstFull) -and -not $want.ContainsKey($f.FullName.Substring($dstFull.Length + 1))) { Remove-Item -LiteralPath $f.FullName -Force; $removed++ }
    }
    Set-Phase 'tools' 'done' ("{0} files ({1} copied, {2} removed) in {3}" -f $want.Count, $copied, $removed, $dst)
}

function Phase-Specs {
    Set-Phase 'specs' 'running'
    $conf = Join $S.Tools 'open-apollo\configs\plugin-panels.conf'
    $pin = @{}
    foreach ($line in (Get-Content -LiteralPath $conf)) { if ($line -match '^(PANELS_VERSION|PANELS_SHA256|PANELS_URL)=(.+)$') { $pin[$Matches[1]] = $Matches[2].Trim() } }
    $ver = [string]$pin['PANELS_VERSION']
    $url = ([string]$pin['PANELS_URL']) -replace '\{version\}', $ver
    if ($ver -notmatch '^[0-9][0-9A-Za-z.+-]*$' -or [string]$pin['PANELS_SHA256'] -notmatch '^[0-9a-f]{64}$' -or -not (Test-AllowedUrl $url $PanelHosts)) {
        throw (New-SetupError 'specs' "The panel specs pin is not as expected: $url")
    }
    $specs = Join $S.Root 'specs'
    $src = Join $specs '.source'
    $args2 = @('-I', (Join $S.Tools 'install_panels.py'), '--conf', $conf, '--downloads', (Join $S.Root 'downloads'), '--dest', $specs)
    if (-not ((Test-Path -LiteralPath $src) -and (Get-Content -LiteralPath $src -Raw).Trim() -eq $pin['PANELS_SHA256'])) {
        $tar = Get-Pinned @{ File = "open-apollo-panels-$ver.tar.gz"; Url = $url; Sha256 = $pin['PANELS_SHA256']; Size = 0 } (Join $S.Root 'downloads\panels') 0.0 0.7 $PanelHosts $PanelRedirectHosts
        $args2 += @('--from', $tar)
    }
    $S.Text = 'Installing the panel specs (install_panels.py checks the pinned SHA-256 again)'
    $rc = Invoke-Py $args2 $S.Tools
    if ($rc -ne 0) { throw (New-SetupError 'specs' "The panel specs were refused or could not be installed (exit $rc): see $($S.Log)") }
    $n = @(Get-ChildItem -LiteralPath (Join $S.Root 'specs') -Filter 'PluginSpec_*.json' -File).Count
    Set-Phase 'specs' 'done' "$n panel specs"
}

function Phase-Store {
    $store = Join $S.Root 'store'
    $pins = Join $S.Tools 'open-apollo\console-rs\crates\oa-assets\data\firmware-pins.json'
    if ((Test-Path -LiteralPath (Join $store 'manifest.json')) -and (Get-StateValue 'store') -eq $script:Uad.Fingerprint -and
        (Test-Path -LiteralPath (Join $store 'modules'))) {
        Set-Phase 'store' 'skipped' 'The module store is current (the UAD install has not changed)'
        return
    }
    Set-Phase 'store' 'running'
    $S.Text = "Reading the DSP modules of $($script:Uad.Count) plug-in DLLs (as bytes; nothing is run)"
    Save-Status -Force
    $rc = Invoke-Py @('-I', '-X', 'utf8', (Join $S.Tools 'oa_assets_dsp.py'), 'dsp', '--from', $UadRoot, '--out', $store, '--pins', $pins) $S.Tools {
        param($l, $e) if ($l -match '^\[ OK \]|\[INFO\]\s+\d+ plug-ins carry') { $S.Text = (To-Ascii $l); $S.PhaseFrac = [Math]::Min(0.9, $S.PhaseFrac + 0.3); Save-Status }
    }
    if ($rc -ne 0) { throw (New-SetupError 'store' "The module store could not be built from the UAD install (exit $rc): see $($S.Log)") }
    $script:stripNote = ''
    $rc = Invoke-Py @('-I', '-X', 'utf8', (Join $S.Tools 'oa_assets_dsp.py'), 'strips', '--store', $store, '--pins', $pins, '--out', (Join $store 'dsp')) $S.Tools
    if ($rc -ne 0) {
        $script:stripNote = 'The Console strip modules in this UAD install are not the pinned ones (see the log): inserts need them.'
        Say "   $($script:stripNote)"
    }
    $mods = @(Get-ChildItem -LiteralPath (Join $store 'modules') -Filter '*.bill' -File).Count
    Set-StateValue 'store' $script:Uad.Fingerprint
    Set-Phase 'store' 'done' "$mods modules from $($script:Uad.Count) plug-in DLLs"
}

# Which plug-ins: -Only (tests), the names the units' licence answers admit (-AuthorizedIds, through the store's
# render ids), a names file, or every plug-in open-apollo knows. Returns the mapper's selection arguments.
function Phase-Licence {
    Set-Phase 'licence' 'running'
    $names = Join $S.SetupDir 'authorized.txt'
    if ($Only.Count) {
        $S.Mode = 'only'
        Set-Phase 'licence' 'done' ("Only: " + ($Only -join ', '))
        $sel = @(); foreach ($n in $Only) { $sel += @('--only', $n) }
        return ,$sel
    }
    if ($AuthorizedIds) {
        if ($AuthorizedIds -notmatch '^[0-9a-fA-Fx,; ]+$') { throw (New-SetupError 'licence' 'Bad -AuthorizedIds') }
        $script:namesOut = ''
        $rc = Invoke-Py @('-I', '-X', 'utf8', (Join $S.Tools 'setup_plugins.py'), 'names', '--manifest', (Join $S.Root 'store\manifest.json'),
                          '--ids', $AuthorizedIds, '--out', $names) $S.Tools { param($l, $e) if (-not $e) { $script:namesOut = $l } }
        if ($rc -ne 0) { throw (New-SetupError 'licence' "The licensed plug-ins could not be read (exit $rc)") }
        $n = @(Get-Content -LiteralPath $names | Where-Object { $_.Trim() }).Count
        $S.Mode = 'licensed'
        Set-Phase 'licence' 'done' "$n plug-in$(if ($n -ne 1) { 's' }) your units license (the mapper maps those on open-apollo's list)"
        return ,@('--authorized', $names)
    }
    if ($Authorized -and $Authorized -ne 'all') {
        $file = (Resolve-Path -LiteralPath $Authorized).Path
        $n = @(Get-Content -LiteralPath $file | Where-Object { $_.Trim() }).Count
        $S.Mode = 'licensed'
        Set-Phase 'licence' 'done' "$n plug-ins from $file"
        return ,@('--authorized', $file)
    }
    if (-not $Authorized -and (Test-Path -LiteralPath $names)) {
        $n = @(Get-Content -LiteralPath $names | Where-Object { $_.Trim() }).Count
        $S.Mode = 'licensed'
        Set-Phase 'licence' 'done' "$n plug-ins your units licensed when Apollo Control last read them"
        return ,@('--authorized', $names)
    }
    $S.Mode = 'all'
    Set-Phase 'licence' 'done' "Every plug-in in your UAD install that open-apollo can map (no licence answer here)"
    return ,@('--authorized', 'all')
}

function Phase-Map([string[]]$Selection) {
    Set-Phase 'map' 'running'
    $mapper = Join $S.Tools 'open-apollo\tools\plugin-mapper'
    $w = if ($Workers -gt 0) { $Workers } else { [Math]::Max(1, [Math]::Min(64, [Environment]::ProcessorCount - 1)) }
    # absolute paths (plugins\README.md: a bundle records its metadata path as given)
    $pyArgs = @('-X', 'utf8', '-m', 'plugin_mapper', '--from', $UadRoot, '--out', (Join $S.Root 'bundles'), '--work', (Join $S.Root 'work'),
                '--specs', (Join $S.Root 'specs'), '--module-store', (Join $S.Root 'store'), '--texts', (Join $S.Root 'generated\Text'),
                '--workers', [string]$w, '--markers') + $Selection
    if ($RetryFailures) { $pyArgs += '--retry-failures' }
    $S.Text = 'Starting the mapper'
    Save-Status -Force
    # mapper lines: "::progress plugins DONE TOTAL ETA TEXT" (setup markers) and its own human lines
    $rc = Invoke-Py $pyArgs $mapper {
        param($l, $e)
        if ($l -match '^::progress plugins (\d+) (\d+) (-?\d+) (.*)$') {
            $S.Done = [int]$Matches[1]; $S.Total = [int]$Matches[2]; $S.Eta = [int]$Matches[3]
            $S.PhaseFrac = if ($S.Total -gt 0) { $S.Done / [double]$S.Total } else { 0.0 }
            Save-Status
        } elseif (-not $e -and $l) {
            $S.Text = (To-Ascii $l)
            if ($S.Interactive -and $l -notmatch '^Still mapping') { Write-Host ('   ' + (To-Ascii $l)) }
            Save-Status
        }
    }
    if ($rc -ne 0) { Say "   the mapper ended with exit code $rc (no plug-in could be mapped)" }
    $script:sum = $null
    $sumArgs = @('-I', '-X', 'utf8', (Join $S.Tools 'setup_plugins.py'), 'summary', '--bundles', (Join $S.Root 'bundles'))
    if ($Only.Count) { foreach ($n in $Only) { $sumArgs += @('--only', $n) } }
    elseif ($Selection.Count -eq 2 -and $Selection[1] -ne 'all') { $sumArgs += @('--names', $Selection[1]) }
    $null = Invoke-Py $sumArgs $S.Tools { param($l, $e) if (-not $e -and $l.StartsWith('{')) { $script:sum = $l | ConvertFrom-Json } }
    if (-not $script:sum) { throw (New-SetupError 'map' "The maps could not be read after mapping: see $($S.Log)") }
    $sum = $script:sum
    $text = "$($sum.plugins) plug-in$(if ($sum.plugins -ne 1) { 's' }) mapped ($($sum.bundles) maps, mono and stereo)"
    if (@($sum.failed).Count) { $text += "; $(@($sum.failed).Count) could not be mapped (" + ((@($sum.failed) | Select-Object -First 3 | ForEach-Object { $_.name }) -join ', ') + $(if (@($sum.failed).Count -gt 3) { ', ...' } else { '' }) + ')' }
    if ($sum.waiting) { $text += "; $($sum.waiting) wait for a licence" }
    $S.Summary = $text
    if ($sum.plugins -eq 0) { throw (New-SetupError 'map' "No plug-in could be mapped: see $($S.Log)") }
    Set-Phase 'map' 'done' $text
}

function Find-AppExe {
    if ($AppExe) { return $AppExe }
    $d = $PSScriptRoot
    for ($i = 0; $i -lt 5 -and $d; $i++) {
        foreach ($c in @((Join $d 'apollo-control.exe'), (Join $d 'app\bin\apollo-control.exe'))) { if (Test-Path -LiteralPath $c -PathType Leaf) { return $c } }
        $d = Split-Path -Parent $d
    }
    return ''
}

function Test-HostingOn {
    try {
        $v = Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Services\apollo\Parameters' -Name 'PluginHost' -ErrorAction Stop
        return [int]$v.PluginHost -ne 0
    } catch { return $false }
}

function Phase-Modules {
    if ($SkipModules) { $S.Modules = 'Not copied (-SkipModules).'; Set-Phase 'modules' 'skipped' $S.Modules; return }
    if (-not $InstallModules -and -not (Test-HostingOn)) {
        $S.Modules = 'Plug-in hosting is off: turning it on in Apollo Control (Settings, Plug-in hosting) copies the modules (one administrator prompt).'
        Set-Phase 'modules' 'skipped' $S.Modules
        return
    }
    $exe = Find-AppExe
    if (-not $exe) { $S.Modules = 'Apollo Control was not found: Settings, Plug-in hosting, Install modules copies them.'; Set-Phase 'modules' 'skipped' $S.Modules; return }
    Set-Phase 'modules' 'running'
    $req = Join $S.SetupDir 'modules-request.json'
    $res = "$req.result"
    Remove-Item -LiteralPath $res -Force -ErrorAction SilentlyContinue
    $script:modOut = ''
    $rc = Invoke-Py @('-I', '-X', 'utf8', (Join $S.Tools 'setup_plugins.py'), 'modules', '--bundles', (Join $S.Root 'bundles'), '--store', (Join $S.Root 'store'),
                      '--pins', (Join $S.Tools 'open-apollo\console-rs\crates\oa-assets\data\firmware-pins.json'), '--out', $req) $S.Tools {
        param($l, $e) if (-not $e) { $script:modOut = $l } }
    if (-not (Test-Path -LiteralPath $req)) { throw (New-SetupError 'modules' "The module list could not be made (exit $rc)") }
    $S.Text = 'Waiting for the administrator prompt: copying the plug-ins'' DSP modules into the driver''s module store'
    $S.PhaseFrac = 0.3
    Save-Status -Force
    Say "   $($S.Text)"
    try {
        $p = Start-Process -FilePath $exe -ArgumentList ('--plugin-admin ' + (Quote-Arg $req)) -Verb RunAs -Wait -PassThru -WindowStyle Hidden
        $code = $p.ExitCode
    } catch {
        $S.Modules = 'Not copied: cancelled at the administrator prompt. Settings, Plug-in hosting, Install modules does it later.'
        Set-Phase 'modules' 'skipped' $S.Modules
        return
    }
    $detail = ''
    $t = Read-Shared $res
    if ($t) { try { $detail = ($t | ConvertFrom-Json).detail } catch { } }
    if ($code -ne 0) { $S.Modules = "Not copied ($detail). Settings, Plug-in hosting, Install modules tries again."; Set-Phase 'modules' 'skipped' $S.Modules; return }
    $S.Modules = "Driver module store: $detail."
    Set-Phase 'modules' 'done' $S.Modules
}

# ---------------------------------------------------------------- the explanation (console runs)

function Show-Intro {
    $bytes = $Pins.Python.Size + $Pins.Pip.Size
    foreach ($w in $Pins.Wheels) { $bytes += $w.Size }
    $mb = $bytes / 1MB
    Write-Host ''
    Write-Host 'Set up UAD plug-ins for Apollo Control'
    Write-Host ''
    Write-Host 'This maps the UAD plug-ins in your own UAD install so Apollo Control can run them on your Apollo''s DSPs.'
    Write-Host "Everything it makes stays in $($S.Root)."
    Write-Host ''
    Write-Host ('Downloads (each file checked against a pinned SHA-256 before use), about {0:N0} MB:' -f ($mb + 1))
    Write-Host ("  Python {0} for Windows (embeddable package), from www.python.org, {1:N1} MB" -f $Pins.Python.Version, ($Pins.Python.Size / 1MB))
    Write-Host ("  pip, numpy, unicorn, capstone and pefile wheels, from files.pythonhosted.org (PyPI), {0:N1} MB" -f (($mb * 1MB - $Pins.Python.Size) / 1MB))
    Write-Host '  open-apollo''s plug-in panel specs, from github.com, about 1 MB'
    Write-Host ''
    Write-Host 'It reads your UAD install offline: the plug-in files are read as bytes and plug-in code runs only inside an'
    Write-Host 'emulator. It never contacts Universal Audio and starts no Universal Audio program.'
    Write-Host ''
    Write-Host 'Preparing takes a few minutes. Mapping takes about 1 to 3 minutes per plug-in, several at once (a few reverbs'
    Write-Host 'much longer); every plug-in open-apollo knows takes an hour or more. If it stops, run it again: it continues.'
    Write-Host 'Disk: about 150 MB for Python, plus the maps (most under 1 MB a plug-in, the Chambers reverbs about 200 MB each).'
    Write-Host ''
}

# ---------------------------------------------------------------- self-test (offline)

function Invoke-SelfTest {
    function Check([bool]$ok, [string]$label) { if (-not $ok) { $script:stFail++ }; Write-Host ("{0}  {1}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $label) }
    $script:stFail = 0
    $ErrorActionPreference = 'Continue'     # the nested runs below are native processes (stderr is not an error here)
    # pins: well-formed, on the allowed hosts, file names match the URLs
    $all = @($Pins.Python, $Pins.Pip) + @($Pins.Wheels)
    Check (@($all | Where-Object { $_.Sha256 -notmatch '^[0-9a-f]{64}$' }).Count -eq 0) 'every pin has a 64-digit lower-case SHA-256'
    Check (@($all | Where-Object { -not (Test-AllowedUrl $_.Url $AllowedHosts) -or -not $_.Url.EndsWith('/' + $_.File) -or $_.Size -le 0 }).Count -eq 0) 'every pinned URL is https on python.org or files.pythonhosted.org and ends in its file name'
    Check (@($Pins.Wheels | Where-Object { $_.File -notmatch '(cp313-cp313-win_amd64|cp37-abi3-win_amd64|py3-none-win_amd64|py3-none-any)\.whl$' }).Count -eq 0) 'the wheels are the Windows x64 builds a CPython 3.13 takes'
    Check ($Pins.Python.File -eq "python-$($Pins.Python.Version)-embed-amd64.zip") 'Python: the 64-bit embeddable package of the pinned version'
    # hosts: never Universal Audio, never http, never another host, never a user or port
    Check (-not (Test-AllowedUrl 'https://www.uaudio.com/x.zip' @('www.uaudio.com'))) 'a Universal Audio host is refused even when listed'
    Check (-not (Test-AllowedUrl 'https://help.universalaudio.com/x' $AllowedHosts)) 'universalaudio.com is refused'
    Check (-not (Test-AllowedUrl 'http://www.python.org/ftp/x.zip' $AllowedHosts)) 'plain http is refused'
    Check (-not (Test-AllowedUrl 'https://www.python.org.evil.example/x' $AllowedHosts)) 'a look-alike host is refused'
    Check (-not (Test-AllowedUrl 'https://user@www.python.org/x' $AllowedHosts) -and -not (Test-AllowedUrl 'https://www.python.org:8443/x' $AllowedHosts)) 'user info and other ports are refused'
    Check ((Test-AllowedUrl 'https://github.com/rolotrealanis98/open-apollo-panels/releases/download/v1/x.tar.gz' $PanelHosts)) 'the panel specs URL host (github.com) is taken for the specs only'
    Check ((Test-AllowedUrl 'https://release-assets.githubusercontent.com/github-production-release-asset/1/x' $PanelRedirectHosts) -and
           -not (Test-AllowedUrl 'https://release-assets.githubusercontent.com/x' $PanelHosts) -and -not (Test-AllowedUrl 'https://github.com/x' $AllowedHosts)) 'GitHub''s release-asset host only as the specs'' redirect; github.com never for the Python downloads'
    $conf = Join $PSScriptRoot 'open-apollo\configs\plugin-panels.conf'
    if (Test-Path -LiteralPath $conf) {
        $u = ((Get-Content -LiteralPath $conf) | Where-Object { $_ -match '^PANELS_URL=' }) -replace '^PANELS_URL=', ''
        Check (Test-AllowedUrl ($u -replace '\{version\}', '0') $PanelHosts) "the vendored plugin-panels.conf pins a github.com URL ($u)"
    }
    # quoting (CommandLineToArgvW rules)
    Check ((Quote-Arg 'C:\') -eq 'C:\' -and (Quote-Arg 'C:\a b\') -eq '"C:\a b\\"' -and (Quote-Arg 'a"b') -eq '"a\"b"' -and (Quote-Arg '') -eq '""') 'command-line quoting: trailing backslashes and quotes'
    # JSON writer: ASCII, escapes, nesting, round trip through ConvertFrom-Json
    $o = [ordered]@{ s = "a`"b\c`n" + [char]0x2026; n = 42; f = 1.5; b = $true; z = $null; a = @(1, 'x'); e = @(); d = [ordered]@{ k = 'v' } }
    $j = ConvertTo-JsonText $o
    $back = $j | ConvertFrom-Json
    Check ($j -notmatch '[^\x20-\x7e]' -and $back.s -eq ("a`"b\c`n" + [char]0x2026) -and $back.n -eq 42 -and $back.f -eq 1.5 -and $back.b -eq $true -and
           $null -eq $back.z -and @($back.a).Count -eq 2 -and @($back.e).Count -eq 0 -and $back.d.k -eq 'v') "status JSON: ASCII only, round trip ($j)"
    Check ((To-Ascii ('Mapping 1/2: UAD X (mono, stereo)' + [char]0x2026 + ' ' + [char]0x00B7 + ' ok')) -eq 'Mapping 1/2: UAD X (mono, stereo)... ; ok') 'the mapper''s marks become ASCII'
    # a fresh PC without UAD: the check stops cleanly, before any download
    $tmp = Join ([IO.Path]::GetTempPath()) ('plugsetup-selftest-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    try {
        Check ($null -eq (Find-Uad $tmp)) 'no UAD under an empty root: none found'
        $u = Get-UadPaths $tmp
        New-Item -ItemType Directory -Force -Path $u.Dlls, (Split-Path -Parent $u.Sdk) | Out-Null
        Check ($null -eq (Find-Uad $tmp)) 'an empty UAD-2 Powered Plugins folder without UAD2SDK.dll: none found'
        [IO.File]::WriteAllBytes((Join $u.Dlls 'UAD Test.dll'), [byte[]](1, 2, 3))
        [IO.File]::WriteAllBytes($u.Sdk, [byte[]](4))
        $f = Find-Uad $tmp
        Check ($f -and $f.Count -eq 1 -and $f.Fingerprint -match '^1/3/\d+/1/\d+$') "a UAD install found by file names, with a fingerprint ($($f.Fingerprint))"
        # the whole script on that empty root: exit 5, status "failed" with code nouad, nothing downloaded
        $root2 = Join $tmp 'root'
        $empty = Join $tmp 'empty'
        New-Item -ItemType Directory -Force -Path $empty | Out-Null
        $ps = Join ([Environment]::GetFolderPath('System')) 'WindowsPowerShell\v1.0\powershell.exe'
        & $ps -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $PSCommandPath -Yes -Root $root2 -UadRoot $empty -SkipModules | Out-Null
        $rc = $LASTEXITCODE
        $st = (Read-Shared (Join $root2 'setup\status.json')) | ConvertFrom-Json
        Check ($rc -eq 5 -and $st.state -eq 'failed' -and $st.code -eq 'nouad' -and $st.error -match 'No UAD install found' -and
               -not (Test-Path -LiteralPath (Join $root2 'downloads')) -and -not (Test-Path -LiteralPath (Join $root2 'python'))) "no UAD: exit 5, status failed/nouad, nothing downloaded ($rc, $($st.code))"
        # a second run while the lock is held: exit 3, the running one's status untouched
        $lock = [IO.File]::Open((Join $root2 'setup\.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        try {
            $before = Read-Shared (Join $root2 'setup\status.json')
            & $ps -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $PSCommandPath -Yes -Root $root2 -UadRoot $empty -SkipModules | Out-Null
            $rc = $LASTEXITCODE
            Check ($rc -eq 3 -and (Read-Shared (Join $root2 'setup\status.json')) -eq $before) "a second setup while one runs: exit 3, status untouched ($rc)"
        } finally { $lock.Dispose() }
        # cancel: a cancel file stops the run at its first check, status cancelled
        [IO.File]::WriteAllText((Join $root2 'setup\cancel'), '')
        & $ps -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $PSCommandPath -Yes -Root $root2 -UadRoot $tmp -SkipModules -StopAfter 'check' | Out-Null
        $rc = $LASTEXITCODE
        $st = (Read-Shared (Join $root2 'setup\status.json')) | ConvertFrom-Json
        Check ($rc -eq 4 -and $st.state -eq 'cancelled' -and -not (Test-Path -LiteralPath (Join $root2 'setup\cancel'))) "cancel: exit 4, status cancelled, the request consumed ($rc, $($st.state))"
        # StopAfter check with the fake UAD: done after the check, nothing downloaded
        & $ps -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $PSCommandPath -Yes -Root $root2 -UadRoot $tmp -SkipModules -StopAfter 'check' | Out-Null
        $rc = $LASTEXITCODE
        $st = (Read-Shared (Join $root2 'setup\status.json')) | ConvertFrom-Json
        Check ($rc -eq 0 -and $st.state -eq 'done' -and ($st.phases | Where-Object { $_.id -eq 'check' }).state -eq 'done' -and
               ($st.phases | Where-Object { $_.id -eq 'python' }).state -eq 'pending' -and -not (Test-Path -LiteralPath (Join $root2 'downloads'))) "-StopAfter check: done, later phases pending ($rc)"
    } finally { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
    # setup_plugins.py, with a Python if one is at hand (the runtime, else a developer venv): never downloads
    $py = @((Join $env:LOCALAPPDATA 'Avium\ApolloPlugins\python\python.exe'), (Join (Split-Path -Parent $PSScriptRoot) '.venv\Scripts\python.exe')) |
        Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if ($py) {
        $out = & $py -I -X utf8 (Join $PSScriptRoot 'setup_plugins.py') selftest 2>&1
        $out | ForEach-Object { Write-Host "  py: $_" }
        Check ($LASTEXITCODE -eq 0) "setup_plugins.py selftest ($py)"
    } else { Write-Host 'note: no Python here: setup_plugins.py selftest not run' }
    Write-Host ('setup-plugins selftest: ' + $(if ($script:stFail) { "$($script:stFail) FAILED" } else { 'all passed' }))
    if ($script:stFail) { exit 1 } else { exit 0 }
}

# ---------------------------------------------------------------- main

if ($SelfTest) { Invoke-SelfTest }

$validStops = @('', 'check', 'python', 'packages', 'tools', 'specs', 'store', 'licence', 'map')
if ($validStops -notcontains $StopAfter) { Write-Host "-StopAfter must be one of $($validStops -join ', ')"; exit 2 }
if ($Authorized -and $AuthorizedIds) { Write-Host 'Give -Authorized or -AuthorizedIds, not both'; exit 2 }
if (-not $Root) { $Root = Join ([Environment]::GetFolderPath('LocalApplicationData')) 'Avium\ApolloPlugins' }
if (-not $UadRoot) { $UadRoot = [IO.Path]::GetPathRoot([Environment]::SystemDirectory) }
$Root = [IO.Path]::GetFullPath($Root)
$UadRoot = [IO.Path]::GetFullPath($UadRoot)
foreach ($bad in @(${env:ProgramFiles}, ${env:ProgramFiles(x86)}, ${env:SystemRoot})) {
    if ($bad -and ($Root + '\').StartsWith($bad.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { Write-Host "Refusing -Root $Root (a system folder)"; exit 2 }
}

$S.Root = $Root
$S.SetupDir = Join $Root 'setup'
$S.Log = Join $S.SetupDir 'setup.log'
$S.Runtime = Join $Root 'python'
$S.Python = Join $S.Runtime 'python.exe'
$S.Tools = Join $Root 'tools'
$S.Interactive = -not $Yes
New-Item -ItemType Directory -Force -Path $S.SetupDir | Out-Null

# one setup at a time (Apollo Control's and the Start-menu shortcut's): a second one leaves the first's status alone
try { $S.Lock = [IO.File]::Open((Join $S.SetupDir '.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None) }
catch {
    Write-Host 'Another plug-in setup is running (Apollo Control or the Start-menu shortcut). Let it finish, or cancel it there.'
    if ($S.Interactive) { [void](Read-Host 'Press Enter to close') }
    exit 3
}
$S.StatusPath = Join $S.SetupDir 'status.json'
try {
    if (Test-Path -LiteralPath $S.Log) { if ((Get-Item -LiteralPath $S.Log).Length -gt 4MB) { Move-Item -LiteralPath $S.Log -Destination "$($S.Log).old" -Force } }
    foreach ($line in @(Get-Content -LiteralPath (Join $S.SetupDir 'state.txt') -ErrorAction SilentlyContinue)) {
        if ($line -match '^([a-z]+)=(.*)$') { $S.State[$Matches[1]] = $Matches[2] }
    }
} catch { }
$exit = 0
try {
    # below normal priority for this script and everything it starts (the mapper's workers): audio and the UI first
    try { (Get-Process -Id $PID).PriorityClass = 'BelowNormal' } catch { }
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    Write-Log "---- setup started (pid $PID): root $Root, UAD root $UadRoot, script $PSCommandPath"
    Save-Status -Force
    if ($S.Interactive) {
        Show-Intro
        $a = Read-Host 'Set up UAD plug-ins now? [Y/n]'
        if ($a -and $a.Trim() -notmatch '^(y|yes)$') { throw (New-SetupError 'cancelled' 'Not started.') }
    }
    Test-Cancel
    Phase-Check
    if ($StopAfter -ne 'check') {
        Test-Cancel; Phase-Python
        if ($StopAfter -ne 'python') {
            Test-Cancel; Phase-Packages
            if ($StopAfter -ne 'packages') {
                Test-Cancel; Phase-Tools
                if ($StopAfter -ne 'tools') {
                    Test-Cancel; Phase-Specs
                    if ($StopAfter -ne 'specs') {
                        Test-Cancel; Phase-Store
                        if ($StopAfter -ne 'store') {
                            Test-Cancel; $sel = Phase-Licence
                            if ($StopAfter -ne 'licence') {
                                Test-Cancel; Phase-Map $sel
                                if ($StopAfter -ne 'map') { Test-Cancel; Phase-Modules }
                            }
                        }
                    }
                }
            }
        }
    }
    $S.Phase = ''
    $S.Text = if ($S.Summary) { 'Plug-ins are set up: ' + $S.Summary + '.' } else { 'Done.' }
    if ($script:stripNote) { $S.Text += ' ' + $script:stripNote }
    if ($S.Modules) { $S.Text += ' ' + $S.Modules }
    Save-Status 'done' -Force
    Say $S.Text
} catch {
    $err = $_.Exception
    $code = if ($err.Data.Contains('SetupCode')) { [string]$err.Data['SetupCode'] } else { 'error' }
    if ($S.Child) { try { Stop-Tree $S.Child.Id } catch { } }
    if ($S.Phase -and $S.PhaseState[$S.Phase] -eq 'running') { $S.PhaseState[$S.Phase] = $(if ($code -eq 'cancelled') { 'pending' } else { 'failed' }) }
    $S.Code = $code
    $S.Error = $err.Message
    if ($code -eq 'cancelled') {
        Remove-Item -LiteralPath (Join $S.SetupDir 'cancel') -Force -ErrorAction SilentlyContinue
        $S.Text = $err.Message
        Save-Status 'cancelled' -Force
        $exit = 4
    } else {
        if ($code -eq 'error') { Write-Log ("unexpected: " + ($_ | Out-String)) }
        $S.Text = $err.Message
        Save-Status 'failed' -Force
        $exit = if ($code -eq 'nouad') { 5 } else { 1 }
    }
    Say ("Stopped: " + $err.Message)
} finally {
    Write-Log "---- setup ended, exit $exit"
    if ($S.Lock) { $S.Lock.Dispose() }
}
if ($S.Interactive) {
    Write-Host ''
    Write-Host "Log: $($S.Log)"
    [void](Read-Host 'Press Enter to close')
}
exit $exit
