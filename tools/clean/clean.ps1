param(
  [ValidateSet('scan', 'clean', 'schedule', 'schedule-elevated', 'unschedule', 'unschedule-elevated', 'ui', 'ui-selftest')]
  [string]$Mode = 'scan',
  [string]$Day = '',
  [string]$Time = '',
  [switch]$Quiet
)

$ErrorActionPreference = 'SilentlyContinue'
Add-Type -AssemblyName System.Windows.Forms | Out-Null
Add-Type -AssemblyName System.Drawing | Out-Null

$cut = (Get-Date).AddDays(-1)

$taskName = 'TeacherBox 清理 C 盘'
$cfgDir = Join-Path $env:LOCALAPPDATA 'TeacherBox'
$cfgPath = Join-Path $cfgDir 'clean-task.json'
$resultPath = Join-Path $cfgDir 'clean-task.last.txt'
$selfPath = $PSCommandPath
if (-not $selfPath) { $selfPath = $MyInvocation.MyCommand.Path }

$Days = @('MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT', 'SUN')
$DayLabels = @('周一', '周二', '周三', '周四', '周五', '周六', '周日')

# 只碰临时文件、更新缓存、缩略图这类可再生的东西，不动用户目录里的文档
$targets = @(
  @{ n = '用户临时文件';   p = $env:TEMP;                                        filter = $null;             old = $true  },
  @{ n = '系统临时文件';   p = "$env:SystemRoot\Temp";                           filter = $null;             old = $true  },
  @{ n = 'Windows 更新缓存'; p = "$env:SystemRoot\SoftwareDistribution\Download"; filter = $null;             old = $false },
  @{ n = '缩略图缓存';     p = "$env:LOCALAPPDATA\Microsoft\Windows\Explorer";   filter = 'thumbcache_*.db'; old = $false }
)

function FormatSize([double]$bytes) {
  if ($bytes -ge 1GB) { return ('{0:N1} GB' -f ($bytes / 1GB)) }
  if ($bytes -ge 1MB) { return ('{0:N0} MB' -f ($bytes / 1MB)) }
  if ($bytes -gt 0)   { return ('{0:N0} KB' -f [Math]::Ceiling($bytes / 1KB)) }
  return '0'
}

function MeasureTarget($t) {
  if (-not (Test-Path -LiteralPath $t.p)) { return 0 }
  $items = if ($t.filter) {
    Get-ChildItem -LiteralPath $t.p -Filter $t.filter -Force -File
  } else {
    Get-ChildItem -LiteralPath $t.p -Force -Recurse -File
  }
  $sum = ($items | Measure-Object Length -Sum).Sum
  if ($null -eq $sum) { return 0 }
  return $sum
}

function RemoveTarget($t) {
  if (-not (Test-Path -LiteralPath $t.p)) { return @{ bytes = 0; skipped = $false } }
  $bytes = 0
  $items = if ($t.filter) {
    Get-ChildItem -LiteralPath $t.p -Filter $t.filter -Force -File
  } else {
    Get-ChildItem -LiteralPath $t.p -Force -Recurse -File
  }
  foreach ($f in $items) {
    if ($t.old -and $f.LastWriteTime -ge $cut) { continue }
    $len = $f.Length
    try {
      Remove-Item -LiteralPath $f.FullName -Force -ErrorAction Stop
      $bytes += $len
    } catch {
      # 被占用/没权限的留着，不计入释放量
    }
  }
  # 顺手把腾空的子目录收掉，根目录留着
  if (-not $t.filter -and (Test-Path -LiteralPath $t.p)) {
    Get-ChildItem -LiteralPath $t.p -Force -Recurse -Directory |
      Sort-Object { $_.FullName.Length } -Descending |
      ForEach-Object {
        if (-not (Get-ChildItem -LiteralPath $_.FullName -Force)) {
          Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue
        }
      }
  }
  return @{ bytes = $bytes; skipped = $false }
}

function AppendHistory($text) {
  New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
  $stamp = Get-Date -Format 'yyyy-MM-dd HH:mm'
  Add-Content -LiteralPath (Join-Path $cfgDir 'clean.log') -Value ("[$stamp] " + $text) -Encoding UTF8
}

function Report($title, $body) {
  if (-not $Quiet) {
    [System.Windows.Forms.MessageBox]::Show($body, $title, 'OK', 'Information') | Out-Null
  } else {
    Write-Output $body
  }
}

# ---------------------------------------------------------------- 定时配置

function ReadConfig {
  $def = @{ day = 'SUN'; time = '20:00' }
  if (-not (Test-Path -LiteralPath $cfgPath)) { return $def }
  try {
    $j = Get-Content -LiteralPath $cfgPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $d = [string]$j.day
    $t = [string]$j.time
    if ($Days -notcontains $d) { $d = 'SUN' }
    if ($t -notmatch '^\d{1,2}:\d{2}$') { $t = '20:00' }
    return @{ day = $d; time = $t }
  } catch {
    return $def
  }
}

function SaveConfig($day, $time) {
  New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
  $obj = [ordered]@{ day = $day; time = $time; savedAt = (Get-Date).ToString('yyyy-MM-dd HH:mm') }
  ($obj | ConvertTo-Json) | Set-Content -LiteralPath $cfgPath -Encoding UTF8
}

function DayLabel($code) {
  $i = [Array]::IndexOf($Days, $code)
  if ($i -lt 0) { return '周日' }
  return $DayLabels[$i]
}

function NormalizeTime($t) {
  if ($t -notmatch '^(\d{1,2}):(\d{2})$') { return '20:00' }
  $h = [int]$Matches[1]
  $m = [int]$Matches[2]
  if ($h -gt 23 -or $m -gt 59) { return '20:00' }
  return ('{0:00}:{1:00}' -f $h, $m)
}

function RegisterCleanTask($day, $time) {
  $run = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' +
         $selfPath + '" -Mode clean -Quiet'
  & schtasks /create /tn $taskName /tr $run /sc weekly /d $day /st $time /f *> $null
  return $LASTEXITCODE
}

function RemoveCleanTask {
  & schtasks /delete /tn $taskName /f *> $null
  return $LASTEXITCODE
}

function TestTaskExists {
  & schtasks /query /tn $taskName *> $null
  return ($LASTEXITCODE -eq 0)
}

function WriteResult($text) {
  New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
  Set-Content -LiteralPath $resultPath -Value $text -Encoding UTF8
}

function InvokeElevated($extraArgs) {
  Remove-Item -LiteralPath $resultPath -Force -ErrorAction SilentlyContinue
  $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden',
            '-File', ('"' + $selfPath + '"')) + $extraArgs
  try {
    Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $args -Wait -ErrorAction Stop
    if (Test-Path -LiteralPath $resultPath) {
      return ((Get-Content -LiteralPath $resultPath -Raw -Encoding UTF8).Trim())
    }
    return 'FAIL|没有拿到结果，任务可能没建上'
  } catch {
    return ('CANCEL|' + $_.Exception.Message)
  }
}

# ---------------------------------------------------------------- 扫描

if ($Mode -eq 'scan') {
  $lines = @()
  $total = 0
  foreach ($t in $targets) {
    $size = MeasureTarget $t
    $total += $size
    $lines += ('{0,-16} {1}' -f $t.n, (FormatSize $size))
  }
  $body = ($lines -join "`r`n") + "`r`n`r`n合计约 " + (FormatSize $total) + " 可清理"
  AppendHistory "扫描 " + (FormatSize $total)
  Report 'C 盘扫描' $body
  return
}

# ---------------------------------------------------------------- 清理

if ($Mode -eq 'clean') {
  $lines = @()
  $total = 0
  foreach ($t in $targets) {
    $r = RemoveTarget $t
    $total += $r.bytes
    $lines += ('{0,-16} {1}' -f $t.n, (FormatSize $r.bytes))
  }

  Clear-RecycleBin -DriveLetter C -Force -ErrorAction SilentlyContinue
  $recycleText = '已清空'

  $body = ($lines -join "`r`n") +
          "`r`n回收站           $recycleText" +
          "`r`n`r`n本次释放 " + (FormatSize $total) + "（不含回收站）"
  AppendHistory ("清理 " + (FormatSize $total))
  Report 'C 盘清理' $body
  return
}

# ---------------------------------------------------------------- 设定 / 取消（提权后的执行体）

if ($Mode -eq 'schedule-elevated') {
  $cfg = ReadConfig
  if ($Day -eq '' -or $Days -notcontains $Day) { $Day = $cfg.day }
  $Time = NormalizeTime $(if ($Time -eq '') { $cfg.time } else { $Time })
  $code = RegisterCleanTask $Day $Time
  if ($code -eq 0) {
    SaveConfig $Day $Time
    AppendHistory ('已设定每周' + (DayLabel $Day) + ' ' + $Time + ' 清理')
    WriteResult ('OK|已设定：每' + (DayLabel $Day) + ' ' + $Time + ' 自动清理')
  } else {
    WriteResult ('FAIL|设定失败，schtasks 退出码 ' + $code)
  }
  return
}

if ($Mode -eq 'unschedule-elevated') {
  $code = RemoveCleanTask
  if ($code -eq 0) {
    AppendHistory '已取消定时清理'
    WriteResult 'OK|已取消定时清理，不会再自动跑'
  } else {
    WriteResult ('FAIL|取消失败，schtasks 退出码 ' + $code + '（任务可能本来就不存在）')
  }
  return
}

# ---------------------------------------------------------------- 定时（入口：弹窗选星期与时间）

function NewScheduleForm($cfg) {
  $exists = TestTaskExists
  $cur = '当前：' + $(if ($exists) { '每' + (DayLabel $cfg.day) + ' ' + $cfg.time } else { '没有设定定时' })

  $form = New-Object System.Windows.Forms.Form
  $form.Text = 'C 盘清理 · 定时'
  $form.ClientSize = New-Object System.Drawing.Size(360, 232)
  $form.StartPosition = 'CenterScreen'
  $form.FormBorderStyle = 'FixedDialog'
  $form.MaximizeBox = $false
  $form.MinimizeBox = $false
  $form.Font = New-Object System.Drawing.Font('Microsoft YaHei UI', 9)

  $lblDay = New-Object System.Windows.Forms.Label
  $lblDay.Text = '每周'
  $lblDay.SetBounds(20, 22, 50, 24)
  $form.Controls.Add($lblDay)

  $cmb = New-Object System.Windows.Forms.ComboBox
  $cmb.DropDownStyle = 'DropDownList'
  $cmb.SetBounds(74, 18, 110, 26)
  foreach ($l in $DayLabels) { [void]$cmb.Items.Add($l) }
  $idx = [Array]::IndexOf($Days, $cfg.day)
  if ($idx -lt 0) { $idx = 6 }
  $cmb.SelectedIndex = $idx
  $form.Controls.Add($cmb)

  $lblTime = New-Object System.Windows.Forms.Label
  $lblTime.Text = '时间'
  $lblTime.SetBounds(200, 22, 40, 24)
  $form.Controls.Add($lblTime)

  $dtp = New-Object System.Windows.Forms.DateTimePicker
  $dtp.Format = 'Custom'
  $dtp.CustomFormat = 'HH:mm'
  $dtp.ShowUpDown = $true
  $dtp.SetBounds(240, 18, 96, 26)
  $st = [TimeSpan]::Parse($cfg.time)
  $dtp.Value = (Get-Date).Date.Add($st)
  $form.Controls.Add($dtp)

  $lblCur = New-Object System.Windows.Forms.Label
  $lblCur.Text = $cur
  $lblCur.ForeColor = [System.Drawing.Color]::FromArgb(110, 105, 90)
  $lblCur.SetBounds(24, 60, 320, 24)
  $form.Controls.Add($lblCur)

  $lblTip = New-Object System.Windows.Forms.Label
  $lblTip.Text = '到点自动跑一次「清理」。电脑没开或没登录就跳过这一周，下一次照常。' +
                 "`r`n第一次设定会弹一次管理员确认。"
  $lblTip.ForeColor = [System.Drawing.Color]::FromArgb(110, 105, 90)
  $lblTip.SetBounds(24, 86, 320, 48)
  $form.Controls.Add($lblTip)

  $btnSave = New-Object System.Windows.Forms.Button
  $btnSave.Text = '设定'
  $btnSave.SetBounds(24, 152, 92, 32)
  $btnSave.Add_Click({ $form.Tag = 'save'; $form.Close() })
  $form.Controls.Add($btnSave)

  $btnDel = New-Object System.Windows.Forms.Button
  $btnDel.Text = '取消定时'
  $btnDel.SetBounds(126, 152, 96, 32)
  $btnDel.Enabled = $exists
  $btnDel.Add_Click({ $form.Tag = 'delete'; $form.Close() })
  $form.Controls.Add($btnDel)

  $btnClose = New-Object System.Windows.Forms.Button
  $btnClose.Text = '关闭'
  $btnClose.SetBounds(232, 152, 92, 32)
  $btnClose.Add_Click({ $form.Tag = ''; $form.Close() })
  $form.Controls.Add($btnClose)

  $form.AcceptButton = $btnSave
  $form.CancelButton = $btnClose
  return @{ form = $form; cmb = $cmb; dtp = $dtp; exists = $exists }
}

function ShowScheduleDialog {
  $cfg = ReadConfig
  $ui = NewScheduleForm $cfg
  $form = $ui.form
  [void]$form.ShowDialog()
  $act = [string]$form.Tag
  $day = $Days[$ui.cmb.SelectedIndex]
  $time = $ui.dtp.Value.ToString('HH:mm')
  $form.Dispose()

  if ($act -eq 'save') {
    $res = InvokeElevated @('-Mode', 'schedule-elevated', '-Day', $day, '-Time', $time)
    $parts = $res.Split('|', 2)
    if ($parts[0] -eq 'OK') {
      Report 'C 盘清理' ($parts[1] + "`r`n`r`n想改时间再点一次「定时设置」就行。")
    } elseif ($parts[0] -eq 'CANCEL') {
      Report 'C 盘清理' ('没有设定：管理员确认被取消了。' + "`r`n" + $parts[1])
    } else {
      Report 'C 盘清理' ('没有设定成功。' + "`r`n" + $parts[1])
    }
  } elseif ($act -eq 'delete') {
    $res = InvokeElevated @('-Mode', 'unschedule-elevated')
    $parts = $res.Split('|', 2)
    if ($parts[0] -eq 'OK') {
      Report 'C 盘清理' $parts[1]
    } elseif ($parts[0] -eq 'CANCEL') {
      Report 'C 盘清理' ('没有取消：管理员确认被取消了。' + "`r`n" + $parts[1])
    } else {
      Report 'C 盘清理' ('没有取消成功。' + "`r`n" + $parts[1])
    }
  }
}

if ($Mode -eq 'ui-selftest') {
  $cfg = ReadConfig
  $ui = NewScheduleForm $cfg
  $bad = @()
  if ($ui.cmb.Items.Count -ne 7) { $bad += '星期项=' + $ui.cmb.Items.Count }
  if ($ui.dtp.CustomFormat -ne 'HH:mm') { $bad += '时间格式' }
  if ($ui.form.ClientSize.Width -lt 300) { $bad += '窗体尺寸' }
  if ($ui.form.Controls.Count -lt 6) { $bad += '控件数=' + $ui.form.Controls.Count }
  if ($ui.cmb.SelectedIndex -lt 0) { $bad += '默认星期' }
  $ui.form.Dispose()
  $text = $(if ($bad.Count -eq 0) { 'UI OK' } else { 'UI FAIL: ' + ($bad -join ', ') }) +
          ' day=' + (DayLabel $cfg.day) + ' time=' + $cfg.time
  if ($Quiet) { Write-Output $text } else { Report 'C 盘清理' $text }
  return
}
if ($Mode -eq 'ui') {
  ShowScheduleDialog
  return
}

# 兼容旧调用：直接设定（默认读上次保存的星期/时间，没有就周日 20:00）
if ($Mode -eq 'schedule' -or $Mode -eq 'unschedule') {
  $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

  if (-not $isAdmin) {
    $extra = if ($Mode -eq 'unschedule') {
      @('-Mode', 'unschedule-elevated')
    } else {
      $cfg = ReadConfig
      @('-Mode', 'schedule-elevated', '-Day', $(if ($Day -eq '') { $cfg.day } else { $Day }),
        '-Time', (NormalizeTime $(if ($Time -eq '') { $cfg.time } else { $Time })))
    }
    $res = InvokeElevated $extra
    $parts = $res.Split('|', 2)
    if ($parts[0] -eq 'OK') {
      Report 'C 盘清理' $parts[1]
    } elseif ($parts[0] -eq 'CANCEL') {
      Report 'C 盘清理' ('已取消，没有改动。' + "`r`n" + $parts[1])
    } else {
      Report 'C 盘清理' $parts[1]
    }
    return
  }

  if ($Mode -eq 'unschedule') {
    $code = RemoveCleanTask
    if ($code -eq 0) { Report 'C 盘清理' '已取消定时清理。' } else { Report 'C 盘清理' ('取消失败，退出码 ' + $code + '。') }
    return
  }

  $cfg = ReadConfig
  if ($Day -eq '') { $Day = $cfg.day }
  $Time = NormalizeTime $(if ($Time -eq '') { $cfg.time } else { $Time })
  $code = RegisterCleanTask $Day $Time
  if ($code -eq 0) {
    SaveConfig $Day $Time
    AppendHistory ('已设定每周' + (DayLabel $Day) + ' ' + $Time + ' 清理')
    Report 'C 盘清理' ('已设定：每' + (DayLabel $Day) + ' ' + $Time + ' 自动清理。')
  } else {
    Report 'C 盘清理' ('设定失败，退出码 ' + $code + '。')
  }
  return
}
