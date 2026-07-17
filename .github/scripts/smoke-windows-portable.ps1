[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $Archive
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$startedAt = [DateTime]::UtcNow
$root = Join-Path $env:RUNNER_TEMP 'shotcut-windows-smoke'
$payload = Join-Path $root 'payload'
$diagnostics = Join-Path $root 'diagnostics'
$dumps = Join-Path $diagnostics 'dumps'
if (Test-Path -LiteralPath $root) {
    Remove-Item -LiteralPath $root -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $payload, $diagnostics, $dumps | Out-Null

if (-not (Test-Path -LiteralPath $Archive -PathType Leaf)) {
    throw "Portable ZIP missing: $Archive"
}
Expand-Archive -LiteralPath $Archive -DestinationPath $payload -Force
$portable = Join-Path $payload 'Shotcut'
$exe = Join-Path $portable 'shotcut.exe'
$sidecar = Join-Path $portable 'shotcut-mcp.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Portable executable missing: $exe"
}
if (-not (Test-Path -LiteralPath $sidecar -PathType Leaf)) {
    throw "MCP sidecar missing: $sidecar"
}
if ((Get-Item -LiteralPath $sidecar).Length -eq 0) {
    throw "MCP sidecar is empty: $sidecar"
}
if (-not (Test-Path -LiteralPath (Join-Path $portable 'opengl32sw.dll') -PathType Leaf)) {
    throw 'Portable package is missing opengl32sw.dll'
}

Get-FileHash -LiteralPath $exe -Algorithm SHA256 |
    Format-List |
    Out-File (Join-Path $diagnostics 'shotcut-exe-sha256.txt') -Encoding utf8
Get-FileHash -LiteralPath $sidecar -Algorithm SHA256 |
    Format-List |
    Out-File (Join-Path $diagnostics 'shotcut-mcp-exe-sha256.txt') -Encoding utf8
Get-ChildItem -LiteralPath $portable -Recurse -File | ForEach-Object {
    [pscustomobject]@{
        Path = [IO.Path]::GetRelativePath($portable, $_.FullName)
        Length = $_.Length
    }
} | Export-Csv (Join-Path $diagnostics 'portable-files.csv') -NoTypeInformation -Encoding utf8

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class ShotcutSmokeNative
{
    [DllImport("kernel32.dll")]
    public static extern uint SetErrorMode(uint mode);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool ShowWindowAsync(IntPtr hWnd, int command);
}
"@

# SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX.
# The child inherits this and cannot hang CI behind an Application Error dialog.
[void] [ShotcutSmokeNative]::SetErrorMode(0x0001 -bor 0x0002 -bor 0x8000)
$wer = 'HKCU:\Software\Microsoft\Windows\Windows Error Reporting'
New-Item -Path $wer -Force | Out-Null
New-ItemProperty -Path $wer -Name DontShowUI -PropertyType DWord -Value 1 -Force | Out-Null
try {
    $dumpKey = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\shotcut.exe'
    New-Item -Path $dumpKey -Force | Out-Null
    New-ItemProperty -Path $dumpKey -Name DumpFolder -PropertyType ExpandString -Value $dumps -Force |
        Out-Null
    New-ItemProperty -Path $dumpKey -Name DumpCount -PropertyType DWord -Value 4 -Force |
        Out-Null
    New-ItemProperty -Path $dumpKey -Name DumpType -PropertyType DWord -Value 1 -Force |
        Out-Null
} catch {
    "WER dump setup failed: $_" |
        Out-File (Join-Path $diagnostics 'wer-setup-warning.txt') -Encoding utf8
}

function Format-ExitCode([Diagnostics.Process] $Process)
{
    $code = [int] $Process.ExitCode
    $hex = [BitConverter]::ToUInt32([BitConverter]::GetBytes($code), 0)
    return ('{0} (0x{1:X8})' -f $code, $hex)
}

function Assert-Running([Diagnostics.Process] $Process, [string] $Stage)
{
    if ($Process.HasExited) {
        throw "shotcut.exe exited during $Stage with $(Format-ExitCode $Process)"
    }
}

function Save-Screenshot([string] $Path)
{
    try {
        $bounds = [Windows.Forms.SystemInformation]::VirtualScreen
        $bitmap = [Drawing.Bitmap]::new($bounds.Width, $bounds.Height)
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.CopyFromScreen($bounds.Left, $bounds.Top, 0, 0, $bounds.Size)
            $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
        } finally {
            $graphics.Dispose()
            $bitmap.Dispose()
        }
    } catch {
        "Screenshot failed: $_" | Out-File "$Path.txt" -Encoding utf8
    }
}

function Get-ThumbnailCount([string] $Directory)
{
    return @(
        Get-ChildItem -LiteralPath $Directory -Filter '*.png' -File -ErrorAction SilentlyContinue
    ).Count
}

function Invoke-ShotcutSmoke([ValidateSet('d3d11', 'opengl')] [string] $Backend)
{
    $runRoot = Join-Path $diagnostics $Backend
    $appData = Join-Path $runRoot 'appdata'
    $roaming = Join-Path $runRoot 'roaming'
    $local = Join-Path $runRoot 'local'
    $stdoutPath = Join-Path $runRoot 'stdout.txt'
    $stderrPath = Join-Path $runRoot 'stderr.txt'
    $thumbnailDir = Join-Path $appData 'thumbnails'
    New-Item -ItemType Directory -Force -Path $runRoot, $appData, $roaming, $local |
        Out-Null

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $exe
    $startInfo.WorkingDirectory = $portable
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.ArgumentList.Add('--noupgrade')
    $startInfo.ArgumentList.Add('--appdata')
    $startInfo.ArgumentList.Add($appData)

    # Keep undeployed MSYS2 and Qt DLLs out of the portable package test.
    $startInfo.Environment['PATH'] = @(
        $portable,
        (Join-Path $env:SystemRoot 'System32'),
        $env:SystemRoot
    ) -join ';'
    $startInfo.Environment['SHOTCUT_WATCHDOG'] = '1'
    $startInfo.Environment['QSG_RHI_BACKEND'] = $Backend
    $startInfo.Environment['QSG_INFO'] = '1'
    $startInfo.Environment['QT_FORCE_STDERR_LOGGING'] = '1'
    $startInfo.Environment['APPDATA'] = $roaming
    $startInfo.Environment['LOCALAPPDATA'] = $local
    foreach ($name in @(
        'MSYSTEM',
        'MINGW_PREFIX',
        'MSYS2_PATH_TYPE',
        'PKG_CONFIG_PATH',
        'CMAKE_PREFIX_PATH',
        'LD_LIBRARY_PATH',
        'QT_PLUGIN_PATH',
        'QT_QPA_PLATFORM_PLUGIN_PATH',
        'QML2_IMPORT_PATH',
        'QML_IMPORT_PATH',
        'QT_OPENGL',
        'QSG_RHI_PREFER_SOFTWARE_RENDERER',
        'SHOTCUT_MCP_ENABLE',
        'SHOTCUT_MCP_TOKEN',
        'SHOTCUT_MCP_ENDPOINT',
        'SHOTCUT_MCP_ALLOWED_ROOTS'
    )) {
        [void] $startInfo.Environment.Remove($name)
    }
    if ($Backend -eq 'd3d11') {
        # Qt uses D3D11 WARP on the hosted VM.
        $startInfo.Environment['QSG_RHI_PREFER_SOFTWARE_RENDERER'] = '1'
    } else {
        # Use the bundled opengl32sw.dll.
        $startInfo.Environment['QT_OPENGL'] = 'software'
    }

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    $stdoutTask = $null
    $stderrTask = $null
    $started = $false
    try {
        if (-not $process.Start()) {
            throw 'Process.Start returned false'
        }
        $started = $true
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()

        # A transient splash or merely live process is not readiness. Require a
        # visible, responsive main HWND for eight consecutive 500 ms samples.
        $stable = 0
        $readyDeadline = [DateTime]::UtcNow.AddSeconds(60)
        while ([DateTime]::UtcNow -lt $readyDeadline -and $stable -lt 8) {
            Assert-Running $process 'GUI startup'
            $process.Refresh()
            $handle = $process.MainWindowHandle
            if (
                $handle -ne [IntPtr]::Zero -and
                [ShotcutSmokeNative]::IsWindowVisible($handle) -and
                $process.Responding
            ) {
                $stable++
            } else {
                $stable = 0
            }
            Start-Sleep -Milliseconds 500
        }
        if ($stable -lt 8) {
            throw 'No visible, responsive MainWindow within 60 seconds'
        }

        $expectedBackend = if ($Backend -eq 'd3d11') {
            'graphics backend = Direct3D 11'
        } else {
            'graphics backend = OpenGL'
        }
        $appLog = Join-Path $appData 'shotcut-log.txt'
        $logDeadline = [DateTime]::UtcNow.AddSeconds(15)
        $backendConfirmed = $false
        while ([DateTime]::UtcNow -lt $logDeadline) {
            Assert-Running $process 'backend verification'
            if (Test-Path -LiteralPath $appLog) {
                $backendConfirmed = Select-String -LiteralPath $appLog -SimpleMatch $expectedBackend -Quiet
            }
            if ($backendConfirmed) {
                break
            }
            Start-Sleep -Milliseconds 500
        }
        if (-not $backendConfirmed) {
            throw "Application log did not confirm: $expectedBackend"
        }

        # Maximize every run to stress thumbnail generation even when Elements is
        # already the active tab in the fresh default layout.
        $process.Refresh()
        $handle = $process.MainWindowHandle
        [void] [ShotcutSmokeNative]::ShowWindowAsync($handle, 3)
        Start-Sleep -Milliseconds 500

        $deadline = [DateTime]::UtcNow.AddSeconds(10)
        while (
            [DateTime]::UtcNow -lt $deadline -and
            (Get-ThumbnailCount $thumbnailDir) -eq 0
        ) {
            Assert-Running $process 'initial Elements load'
            Start-Sleep -Milliseconds 500
        }

        # If needed, activate Elements. Toggling twice handles a visible but
        # inactive tab. The thumbnail assertion prevents UI input false passes.
        for (
            $attempt = 1;
            $attempt -le 2 -and (Get-ThumbnailCount $thumbnailDir) -eq 0;
            $attempt++
        ) {
            Assert-Running $process "Elements activation $attempt"
            $process.Refresh()
            $handle = $process.MainWindowHandle
            [void] [ShotcutSmokeNative]::ShowWindowAsync($handle, 3)
            [void] [ShotcutSmokeNative]::SetForegroundWindow($handle)
            Start-Sleep -Milliseconds 500
            [Windows.Forms.SendKeys]::SendWait('^+2')
            $activationDeadline = [DateTime]::UtcNow.AddSeconds(20)
            while (
                [DateTime]::UtcNow -lt $activationDeadline -and
                (Get-ThumbnailCount $thumbnailDir) -eq 0
            ) {
                Assert-Running $process "Elements activation $attempt"
                Start-Sleep -Milliseconds 500
            }
        }

        # Four cached thumbnails proves multiple Glaxnimate/MLT jobs ran.
        $thumbnailDeadline = [DateTime]::UtcNow.AddSeconds(90)
        while (
            [DateTime]::UtcNow -lt $thumbnailDeadline -and
            (Get-ThumbnailCount $thumbnailDir) -lt 4
        ) {
            Assert-Running $process 'Elements thumbnail generation'
            Start-Sleep -Milliseconds 500
        }
        $thumbnailCount = Get-ThumbnailCount $thumbnailDir
        if ($thumbnailCount -lt 4) {
            throw "Elements path was not exercised: only $thumbnailCount thumbnails generated"
        }

        # Catch delayed access violations or hangs after thumbnail startup.
        $badSamples = 0
        $soakDeadline = [DateTime]::UtcNow.AddSeconds(30)
        while ([DateTime]::UtcNow -lt $soakDeadline) {
            Assert-Running $process '30-second soak'
            $process.Refresh()
            $handle = $process.MainWindowHandle
            if (
                $handle -eq [IntPtr]::Zero -or
                -not [ShotcutSmokeNative]::IsWindowVisible($handle) -or
                -not $process.Responding
            ) {
                $badSamples++
            } else {
                $badSamples = 0
            }
            if ($badSamples -ge 6) {
                throw 'MainWindow was continuously unresponsive for 3 seconds'
            }
            Start-Sleep -Milliseconds 500
        }

        if (-not $process.CloseMainWindow()) {
            throw 'CloseMainWindow returned false'
        }
        if (-not $process.WaitForExit(15000)) {
            throw 'Shotcut did not close within 15 seconds'
        }
        if ($process.ExitCode -ne 0) {
            throw "Shotcut returned $(Format-ExitCode $process) on normal close"
        }
        "PASS backend=$Backend thumbnails=$thumbnailCount" |
            Set-Content (Join-Path $runRoot 'status.txt') -Encoding utf8
    } catch {
        ($_ | Out-String) |
            Set-Content (Join-Path $runRoot 'failure.txt') -Encoding utf8
        Save-Screenshot (Join-Path $runRoot 'failure-screen.png')
        throw
    } finally {
        if ($started -and -not $process.HasExited) {
            try {
                $process.Kill($true)
            } catch {
                "Kill failed: $_" | Add-Content (Join-Path $runRoot 'cleanup.txt')
            }
            try {
                [void] $process.WaitForExit(10000)
            } catch {
            }
        }
        if ($null -ne $stdoutTask) {
            try {
                if ($stdoutTask.Wait(5000)) {
                    [IO.File]::WriteAllText(
                        $stdoutPath,
                        $stdoutTask.GetAwaiter().GetResult()
                    )
                }
            } catch {
                "stdout capture failed: $_" | Set-Content $stdoutPath
            }
        }
        if ($null -ne $stderrTask) {
            try {
                if ($stderrTask.Wait(5000)) {
                    [IO.File]::WriteAllText(
                        $stderrPath,
                        $stderrTask.GetAwaiter().GetResult()
                    )
                }
            } catch {
                "stderr capture failed: $_" | Set-Content $stderrPath
            }
        }
        $process.Dispose()
    }
}

$failures = [Collections.Generic.List[string]]::new()
foreach ($backend in @('d3d11', 'opengl')) {
    try {
        Invoke-ShotcutSmoke $backend
    } catch {
        $failures.Add(('{0}: {1}' -f $backend, $_))
    }
}

try {
    $events = Get-WinEvent -FilterHashtable @{
        LogName = 'Application'
        StartTime = $startedAt.AddSeconds(-5)
    } -ErrorAction SilentlyContinue | Where-Object {
        $_.ProviderName -in @('Application Error', 'Windows Error Reporting') -or
        $_.Message -match 'shotcut[.]exe'
    }
    $events |
        Select-Object TimeCreated, ProviderName, Id, LevelDisplayName, Message |
        Format-List |
        Out-File (Join-Path $diagnostics 'application-events.txt') -Encoding utf8
    $events | ForEach-Object { $_.ToXml() } |
        Set-Content (Join-Path $diagnostics 'application-events.xml') -Encoding utf8
} catch {
    "Event log collection failed: $_" |
        Set-Content (Join-Path $diagnostics 'event-log-warning.txt')
}

if ($failures.Count) {
    throw ($failures -join [Environment]::NewLine)
}