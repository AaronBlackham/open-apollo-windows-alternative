# Run the tools an elevated script needs from an admin-only copy. Dot-source it from install.ps1 / revert.ps1:
#   . (Join-Path $PSScriptRoot 'safe-tools.ps1')
#   $tools = Initialize-SafeTools $root @('tools\bin\devcon.exe', 'tools\apollo-diag\apollo-diag.exe')
#   if (-not $tools) { exit 1 }   # then run $tools['tools\apollo-diag\apollo-diag.exe']
# PowerShell 5.1, ASCII only.
#
# Why: the apollo-win tree lives in the user profile, so any normal-integrity process running as the user
# could swap apollo-diag.exe or devcon.exe between the moment an elevated script checks them and the moment
# it runs them, or swap the C# assembly Add-Type compiles into %TEMP%, and get administrator rights.
# So, before anything runs elevated:
#   1. %ProgramFiles%\Avium\ApolloTools is (re)made admin-only: owner Administrators, protected DACL with
#      SYSTEM and Administrators full control and nothing else. Users cannot create anything under
#      %ProgramFiles%, so nobody but an administrator can have made or prepared that folder.
#   2. Each needed binary is copied there and the COPY is hashed (SHA-256) and compared with
#      tools\bin\tools-manifest.txt, which tools\build-all.cmd writes after every build. Only the
#      verified copies are run.
#   3. TEMP and TMP of this PowerShell process point at an admin-only subfolder, so Add-Type compiles
#      the audio helpers where no user process can reach.
# What this does not cover: the .ps1 scripts and the manifest themselves still come from the user
# profile. The planned single-exe installer under Program Files (PHASE2.md section 9.5, TODO) removes that.

function Test-Elevated {
    $id = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    return (New-Object System.Security.Principal.WindowsPrincipal($id)).IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Owner Administrators; protected DACL: SYSTEM + Administrators full control, inherited by everything inside.
function Set-AdminOnlyAcl([string]$path) {
    $sy = New-Object System.Security.Principal.SecurityIdentifier('S-1-5-18')
    $ba = New-Object System.Security.Principal.SecurityIdentifier('S-1-5-32-544')
    $inherit = [System.Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit'
    $none = [System.Security.AccessControl.PropagationFlags]::None
    $acl = New-Object System.Security.AccessControl.DirectorySecurity
    $acl.SetOwner($ba)
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($sid in $sy, $ba) {
        $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule($sid, 'FullControl', $inherit, $none, 'Allow')))
    }
    Set-Acl -LiteralPath $path -AclObject $acl
}

# Fails (returns $false) if anyone but SYSTEM/Administrators can do more than nothing, or the owner is someone else.
function Test-AdminOnlyAcl([string]$path) {
    $acl = Get-Acl -LiteralPath $path
    $ok = @('S-1-5-18', 'S-1-5-32-544')
    $owner = (New-Object System.Security.Principal.NTAccount($acl.Owner)).Translate([System.Security.Principal.SecurityIdentifier]).Value
    if ($ok -notcontains $owner) { return $false }
    foreach ($r in $acl.Access) {
        $sid = $r.IdentityReference.Translate([System.Security.Principal.SecurityIdentifier]).Value
        if ($r.AccessControlType -eq 'Allow' -and $ok -notcontains $sid) { return $false }
    }
    return $true
}

function Read-ToolsManifest([string]$root) {
    $m = @{}
    $file = Join-Path $root 'tools\bin\tools-manifest.txt'
    if (-not (Test-Path -LiteralPath $file)) { return $m }
    foreach ($line in Get-Content -LiteralPath $file) {
        if ($line -match '^([0-9A-Fa-f]{64})\s+(\S.*)$') { $m[$Matches[2].Trim().ToLowerInvariant()] = $Matches[1].ToUpperInvariant() }
    }
    return $m
}

# Returns a hashtable relative path -> verified admin-only copy, or $null (after saying why).
function Initialize-SafeTools([string]$root, [string[]]$files, [scriptblock]$Note = { param($s) Write-Host $s }) {
    if (-not (Test-Elevated)) { & $Note 'This script must run elevated (Run as administrator).'; return $null }
    $base = Join-Path $env:ProgramFiles 'Avium'
    $dir = Join-Path $base 'ApolloTools'
    $tmp = Join-Path $dir 'tmp'
    try {
        foreach ($d in $base, $dir) {
            if (-not (Test-Path -LiteralPath $d)) { New-Item -ItemType Directory -Path $d | Out-Null }
        }
        Set-AdminOnlyAcl $dir
        if (-not (Test-AdminOnlyAcl $dir)) { & $Note "Could not make $dir admin-only; stopping."; return $null }
        if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
        New-Item -ItemType Directory -Path $tmp | Out-Null      # inherits the admin-only DACL
        # Add-Type (audio-helpers.ps1) compiles into TEMP: keep that inside the admin-only folder,
        # also when a binary check below fails (revert.ps1 goes on without apollo-diag then).
        $env:TEMP = $tmp
        $env:TMP = $tmp
    } catch {
        & $Note "Could not prepare $dir ($($_.Exception.Message)); stopping."
        return $null
    }

    $manifest = Read-ToolsManifest $root
    if ($manifest.Count -eq 0) {
        & $Note 'tools\bin\tools-manifest.txt is missing: run tools\build-all.cmd first (it writes the hashes).'
        return $null
    }
    $out = @{}
    foreach ($rel in $files) {
        $src = Join-Path $root $rel
        $key = $rel.ToLowerInvariant()
        if (-not $manifest.ContainsKey($key)) { & $Note "$rel is not in tools-manifest.txt: rebuild with tools\build-all.cmd."; return $null }
        if (-not (Test-Path -LiteralPath $src)) { & $Note "$rel is missing."; return $null }
        $dst = Join-Path $dir (Split-Path -Leaf $rel)
        Copy-Item -LiteralPath $src -Destination $dst -Force
        $hash = (Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash.ToUpperInvariant()
        if ($hash -ne $manifest[$key]) {
            Remove-Item -LiteralPath $dst -Force -ErrorAction SilentlyContinue
            & $Note "$rel does not match tools-manifest.txt (SHA-256 $hash). It changed after the build: rebuild with tools\build-all.cmd, and if you did not change it, do not go on."
            return $null
        }
        $out[$rel] = $dst
    }
    return $out
}

# True once Initialize-SafeTools pointed TEMP at the admin-only folder (Add-Type is then safe elevated).
function Test-SafeTemp {
    return ($env:TEMP -and $env:TEMP -like '*\Avium\ApolloTools\tmp' -and (Test-Path -LiteralPath $env:TEMP))
}
