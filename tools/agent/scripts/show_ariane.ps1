# Un-minimise the Ariane window (a minimised window renders at 0x0, so agent
# captures come back empty) and print its client size.
$sig = @'
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
[DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
public struct RECT { public int L, T, R, B; }
'@
Add-Type -MemberDefinition $sig -Name Win -Namespace ArianeShow
$p = Get-Process ariane -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { Write-Output "ariane is not running"; exit 1 }
if ([ArianeShow.Win]::IsIconic($p.MainWindowHandle)) { [ArianeShow.Win]::ShowWindow($p.MainWindowHandle, 9) | Out-Null; Start-Sleep -Seconds 2 }
$r = New-Object ArianeShow.Win+RECT
[ArianeShow.Win]::GetClientRect($p.MainWindowHandle, [ref]$r) | Out-Null
Write-Output ("ariane window {0}x{1}, minimised={2}" -f $r.R, $r.B, [ArianeShow.Win]::IsIconic($p.MainWindowHandle))
