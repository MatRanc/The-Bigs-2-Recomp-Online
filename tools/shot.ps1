# tools\shot.ps1 <tag>: screenshot every running game window to %TEMP%\<tag>_<pid>.png (Windows twin of shot.sh).
# Uses PrintWindow(PW_RENDERFULLCONTENT), so it works even when a terminal covers the game.
#   powershell -ExecutionPolicy Bypass -File shot.ps1 tag
param([string]$Tag = "shot")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class TB2Shot {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
"@
$procs = Get-Process thebigs2 -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }
if (-not $procs) { Write-Host "no game window"; exit 1 }
foreach ($p in $procs) {
  $r = New-Object TB2Shot+RECT
  [void][TB2Shot]::GetWindowRect($p.MainWindowHandle, [ref]$r)
  $w = $r.R - $r.L; $h = $r.B - $r.T
  if ($w -le 0 -or $h -le 0 -or $r.L -le -30000) { Write-Host "$($p.Id): window minimized, skipped"; continue }
  $bmp = New-Object System.Drawing.Bitmap $w, $h
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $dc = $g.GetHdc()
  # 2 = PW_RENDERFULLCONTENT (the game draws with Vulkan; without it the capture is black)
  $ok = [TB2Shot]::PrintWindow($p.MainWindowHandle, $dc, 2)
  $g.ReleaseHdc($dc); $g.Dispose()
  $out = Join-Path $env:TEMP "$($Tag)_$($p.Id).png"
  $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
  Write-Host "$out  ($($p.MainWindowTitle))$(if (-not $ok) { '  PrintWindow failed' })"
}
