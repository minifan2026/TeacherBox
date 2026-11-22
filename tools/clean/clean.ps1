param(
  [ValidateSet('scan', 'clean', 'schedule', 'schedule-elevated')]
  [string]$Mode = 'scan',
  [switch]$Quiet
)

$ErrorActionPreference = 'SilentlyContinue'
Add-Type -AssemblyName System.Windows.Forms | Out-Null

$cut = (Get-Date).AddDays(-1)

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
  $dir = Join-Path $env:LOCALAPPDATA 'TeacherBox'
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $stamp = Get-Date -Format 'yyyy-MM-dd HH:mm'
  Add-Content -LiteralPath (Join-Path $dir 'clean.log') -Value ("[$stamp] " + $text) -Encoding UTF8
}

function Report($title, $body) {
  if (-not $Quiet) {
    [System.Windows.Forms.MessageBox]::Show($body, $title, 'OK', 'Information') | Out-Null
  } else {
    Write-Output $body
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

  $recycle = 0
  Clear-RecycleBin -DriveLetter C -Force -ErrorAction SilentlyContinue
  $recycleText = '已清空'

  $body = ($lines -join "`r`n") +
          "`r`n回收站           $recycleText" +
          "`r`n`r`n本次释放 " + (FormatSize $total) + "（不含回收站）"
  AppendHistory ("清理 " + (FormatSize $total))
  Report 'C 盘清理' $body
  return
}

# ---------------------------------------------------------------- 定时

$taskName = 'TeacherBox 清理 C 盘'

if ($Mode -eq 'schedule' -or $Mode -eq 'schedule-elevated') {
  $self = $MyInvocation.MyCommand.Path
  $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

  # 往计划任务根目录写东西要管理员，没权限就提权重来一次
  if ($Mode -eq 'schedule' -and -not $isAdmin) {
    try {
      Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden',
        '-File', ('"' + $self + '"'), '-Mode', 'schedule-elevated'
      ) -ErrorAction Stop
      Report 'C 盘清理' '已经在申请管理员权限，确认后就会建每周任务。'
    } catch {
      Report 'C 盘清理' ('取消或提权失败，没有建任务。' + "`r`n" + $_.Exception.Message)
    }
    return
  }

  $run = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' +
         $self + '" -Mode clean -Quiet'
  & schtasks /create /tn $taskName /tr $run /sc weekly /d SUN /st 20:00 /f *> $null
  if ($LASTEXITCODE -eq 0) {
    AppendHistory '已设定每周日 20:00 清理'
    Report 'C 盘清理' ('已设定：每周日 20:00 自动清理。' + "`r`n" + '取消：schtasks /delete /tn "' + $taskName + '" /f')
  } else {
    Report 'C 盘清理' ('设定失败，退出码 ' + $LASTEXITCODE + '。')
  }
  return
}
