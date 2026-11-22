# TeacherBox 冒烟测试：异常退出 -> 反馈弹窗 -> 重启；连续 10 次 -> 致命退出
# 用法：powershell -File scripts\smoke-crash.ps1 [-Rounds 1]
param([int]$Rounds = 1)

$ErrorActionPreference = 'Stop'
Add-Type -Namespace TB -Name W -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumWindowsProc cb, IntPtr lp);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
public delegate bool EnumWindowsProc(IntPtr h, IntPtr lp);
'@

function Get-WindowsOf([int]$procId) {
  $list = New-Object System.Collections.ArrayList
  $cb = [TB.W+EnumWindowsProc]{ param($h,$lp)
    $p = 0; [void][TB.W]::GetWindowThreadProcessId($h, [ref]$p)
    if ($p -eq $procId) {
      $sb = New-Object System.Text.StringBuilder 512; [void][TB.W]::GetWindowText($h, $sb, 512)
      $cl = New-Object System.Text.StringBuilder 512; [void][TB.W]::GetClassName($h, $cl, 512)
      [void]$list.Add([pscustomobject]@{ Hwnd = $h; Title = $sb.ToString(); Class = $cl.ToString() })
    }
    return $true }
  [void][TB.W]::EnumWindows($cb, [IntPtr]::Zero)
  return $list
}

function Click-DialogButton([IntPtr]$dlg, [string]$text) {
  $target = [IntPtr]::Zero
  $cb = [TB.W+EnumWindowsProc]{ param($h,$lp)
    $sb = New-Object System.Text.StringBuilder 256; [void][TB.W]::GetWindowText($h, $sb, 256)
    $cl = New-Object System.Text.StringBuilder 256; [void][TB.W]::GetClassName($h, $cl, 256)
    if ($cl.ToString() -eq 'Button' -and $sb.ToString() -like $text) { $script:found = $h }
    return $true }
  $script:found = [IntPtr]::Zero
  [void][TB.W]::EnumChildWindows($dlg, $cb, [IntPtr]::Zero)
  if ($script:found -ne [IntPtr]::Zero) {
    [void][TB.W]::PostMessage($script:found, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)  # BM_CLICK
    return $true
  }
  return $false
}

function Wait-Dialog([int]$procId, [string]$titleLike, [int]$timeoutSec = 20) {
  $deadline = (Get-Date).AddSeconds($timeoutSec)
  while ((Get-Date) -lt $deadline) {
    foreach ($w in Get-WindowsOf $procId) {
      if ($w.Class -eq '#32770' -and $w.Title -like $titleLike) { return $w }
    }
    Start-Sleep -Milliseconds 300
  }
  return $null
}

$root = Split-Path -Parent $PSScriptRoot
$mainProc = Get-Process main -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $mainProc) { throw 'main.exe 没在跑，请先启动 dist\main.exe' }
"主程序 pid=$($mainProc.Id)"

for ($i = 1; $i -le $Rounds; $i++) {
  $gui = Get-Process gui -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $gui) { throw '第 ' + $i + ' 轮：gui.exe 不在' }
  "--- 第 $i 轮：强杀 gui pid=$($gui.Id)"
  Stop-Process -Id $gui.Id -Force
  Start-Sleep -Milliseconds 800

  $dlg = Wait-Dialog $mainProc.Id '*TeacherBox*'
  if ($dlg) {
    "检测到弹窗：[$($dlg.Title)]"
    if ($dlg.Title -like '*致命*') {
      "== 判定为致命性错误，主程序应即将退出 =="
      $null = Click-DialogButton $dlg.Hwnd '*确定*'
      Start-Sleep -Seconds 2
      if (-not (Get-Process main -ErrorAction SilentlyContinue)) { "✔ main.exe 已退出（致命路径符合预期）" } else { "✘ main.exe 仍在运行" }
      break
    }
    $null = Click-DialogButton $dlg.Hwnd '*否*'
    Start-Sleep -Seconds 3
    $newGui = Get-Process gui -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($newGui) { "✔ gui 已重启 pid=$($newGui.Id)" } else { "✘ gui 未重启" }
  } else {
    "✘ 没等到弹窗（可能主程序在 30s 内异常保护期里）"
  }
}

"--- 当前进程 ---"
Get-Process main, gui -ErrorAction SilentlyContinue | Format-Table Id, ProcessName -AutoSize
