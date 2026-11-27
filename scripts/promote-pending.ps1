# 审核上架：把客户端「发布」暂存到本机的待审包，转成市场里的正式工具
#
#   .\scripts\promote-pending.ps1                 # 上架待审区里最新的那个
#   .\scripts\promote-pending.ps1 -Id mytool      # 上架指定 id（同名取版本最新的）
#   .\scripts\promote-pending.ps1 -Zip <路径>     # 指定某个待审 zip
#   .\scripts\promote-pending.ps1 -All            # 待审区全部上架
#   .\scripts\promote-pending.ps1 -DryRun         # 只打印将要做什么，不动文件
#
# 做四件事：解包到 TeacherBoxWeb\packages\<id>、复制 zip 到 tools\<id>.zip、
# 更新 tools-index.json、重跑 build-api.ps1 生成 api 分页。
# 之后自己 git commit / push 到 TeacherBoxWeb，Pages 就会更新。
#
# 待审包结构（客户端 PublishFromDialog 产出）：
#   <pending>\<id>-<version>.zip   工具文件夹内容，tool.json 在压缩包根
#   <pending>\<id>-<version>.zip.json   sidecar：id/name/version/desc/size/created/src

[CmdletBinding()]
param(
  [string]$Zip,
  [string]$Id,
  [switch]$All,
  [switch]$DryRun,
  [string]$Author = 'minifan2026',
  [string]$WebRepo = 'C:\Users\Administrator\Desktop\项目\TeacherBoxWeb',
  [string]$PendingDir
)

$ErrorActionPreference = 'Stop'

if (-not $PendingDir) { $PendingDir = Join-Path $env:LOCALAPPDATA 'TeacherBox\pending' }
if (-not (Test-Path $PendingDir)) { throw "待审目录不存在：$PendingDir" }
if (-not (Test-Path (Join-Path $WebRepo 'tools-index.json'))) { throw "不像市场仓库：$WebRepo" }

function Read-Sidecar([string]$zipPath) {
  $jp = "$zipPath.json"
  if (Test-Path $jp) {
    try { return (Get-Content -LiteralPath $jp -Raw -Encoding UTF8 | ConvertFrom-Json) } catch { }
  }
  # 没有 sidecar 时退回文件名解析：<id>-<version>.zip
  $leaf = [IO.Path]::GetFileNameWithoutExtension($zipPath)
  $i = $leaf.LastIndexOf('-')
  if ($i -gt 0) {
    return [pscustomobject]@{
      id = $leaf.Substring(0, $i); name = $leaf.Substring(0, $i)
      version = $leaf.Substring($i + 1); desc = ''; file = ''
    }
  }
  throw "无法识别待审包：$zipPath（缺少 sidecar，文件名也不是 <id>-<version>.zip）"
}

# 选出要上架的包
$packs = @()
if ($Zip) {
  if (-not (Test-Path $Zip)) { throw "找不到待审包：$Zip" }
  $packs = @(Get-Item -LiteralPath $Zip)
} else {
  $packs = @(Get-ChildItem -LiteralPath $PendingDir -Filter '*.zip' -File | Sort-Object LastWriteTime -Descending)
}
if ($Id) { $packs = @($packs | Where-Object { (Read-Sidecar $_.FullName).id -eq $Id } | Select-Object -First 1) }
elseif (-not $All -and $packs.Count -gt 1) { $packs = @($packs[0]) }
if ($packs.Count -eq 0) { throw '待审区里没有可上架的包' }

Add-Type -AssemblyName System.IO.Compression.FileSystem
$indexPath = Join-Path $WebRepo 'tools-index.json'
$idx = Get-Content -LiteralPath $indexPath -Raw -Encoding UTF8 | ConvertFrom-Json
$items = @($idx.items)

foreach ($pack in $packs) {
  $meta = Read-Sidecar $pack.FullName
  if (-not $meta.id) { throw "$($pack.Name)：sidecar 没有 id" }
  $id = [string]$meta.id
  if ($id -match '[\\/:*?"<>|]' -or $id -match '\.\.') { throw "$($pack.Name)：id 不合法（$id）" }

  Write-Host "=== $id  $($meta.version)  $($pack.Name)  $($pack.Length) B ===" -ForegroundColor Cyan

  # 1) 校验 zip 里真有 tool.json 在根
  $za = [IO.Compression.ZipFile]::OpenRead($pack.FullName)
  $toolEntry = $za.Entries | Where-Object { $_.FullName -eq 'tool.json' }
  if (-not $toolEntry) {
    $names = ($za.Entries | Select-Object -First 8 | ForEach-Object { $_.FullName }) -join ', '
    $za.Dispose()
    throw "$($pack.Name)：压缩包根没有 tool.json（可能打成了外层文件夹）。前几项：$names"
  }
  $sr = New-Object IO.StreamReader($toolEntry.Open(), [Text.Encoding]::UTF8)
  $toolText = $sr.ReadToEnd(); $sr.Dispose()
  $entries = $za.Entries.Count
  $za.Dispose()
  $tmeta = $toolText | ConvertFrom-Json

  $pkgDir = Join-Path $WebRepo "packages\$id"
  $zipDst = Join-Path $WebRepo "tools\$id.zip"
  Write-Host ("  解包 -> {0}（{1} 项）" -f $pkgDir, $entries)
  Write-Host ("  复制 -> {0}" -f $zipDst)

  if ($DryRun) {
    Write-Host '  [DryRun] 跳过写文件' -ForegroundColor DarkYellow
  } else {
    if (Test-Path $pkgDir) {
      $bak = Join-Path $env:TEMP ("tb-pkg-bak-" + $id + "-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
      Copy-Item -LiteralPath $pkgDir -Destination $bak -Recurse -Force
      Write-Host "  旧版本已备份到 $bak" -ForegroundColor DarkGray
      Remove-Item -LiteralPath $pkgDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force $pkgDir | Out-Null
    [IO.Compression.ZipFile]::ExtractToDirectory($pack.FullName, $pkgDir)
    New-Item -ItemType Directory -Force (Split-Path -Parent $zipDst) | Out-Null
    Copy-Item -LiteralPath $pack.FullName -Destination $zipDst -Force
  }

  # 2) 更新 tools-index.json
  $tmeta = Get-Content -LiteralPath (Join-Path $pkgDir 'tool.json') -Raw -Encoding UTF8 | ConvertFrom-Json
  $entry = [ordered]@{
    id      = $id
    name    = if ($tmeta.name) { $tmeta.name } elseif ($meta.name) { $meta.name } else { $id }
    version = if ($tmeta.version) { $tmeta.version } else { [string]$meta.version }
    author  = if ($tmeta.author) { $tmeta.author } else { $Author }
    dev     = [bool]$tmeta.dev
    desc    = if ($tmeta.desc) { $tmeta.desc } elseif ($meta.desc) { $meta.desc } else { '' }
    file    = "tools/$id.zip"
  }
  $pos = -1
  for ($i = 0; $i -lt $items.Count; $i++) { if ($items[$i].id -eq $id) { $pos = $i; break } }
  if ($pos -ge 0) {
    Write-Host ("  索引：更新第 {0} 项  {1} -> {2}" -f ($pos + 1), $items[$pos].version, $entry.version)
    $items[$pos] = [pscustomobject]$entry
  } else {
    Write-Host '  索引：新增一项'
    $items += [pscustomobject]$entry
  }
}

$newIndex = [ordered]@{
  updated  = (Get-Date -Format 'yyyy-MM-dd')
  pageSize = if ($idx.pageSize) { [int]$idx.pageSize } else { 50 }
  items    = @($items)
}
$json = $newIndex | ConvertTo-Json -Depth 6

if ($DryRun) {
  Write-Host '--- tools-index.json（DryRun 预览）---' -ForegroundColor DarkYellow
  Write-Host $json
  Write-Host '（DryRun：未写入、未生成 api）' -ForegroundColor DarkYellow
  return
}

[IO.File]::WriteAllText($indexPath, $json, (New-Object Text.UTF8Encoding($false)))
Write-Host ("已写入 {0}" -f $indexPath) -ForegroundColor Green

# 3) 重跑分页接口
$buildApi = Join-Path $WebRepo 'scripts\build-api.ps1'
if (Test-Path $buildApi) {
  & $buildApi
} else {
  Write-Host '  （没有 scripts\build-api.ps1，跳过 api 生成）' -ForegroundColor DarkYellow
}

Write-Host ''
Write-Host '上架完成。接下来提交并推送市场仓库：' -ForegroundColor Green
Write-Host ("  cd `"{0}`"" -f $WebRepo)
Write-Host '  git add -A; git commit -m "上架 <id>"; git push origin main'
Write-Host '（pending 里的待审包保留，确认线上没问题后再自己删）' -ForegroundColor DarkGray
