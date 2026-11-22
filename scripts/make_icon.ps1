# 生成 resources/app.ico：朱红圆角方块 + 米白衬线 T，与 gui 运行时画的图标同一套样式
# 仅 Windows，依赖 System.Drawing。

param(
  [string]$Out = ""
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $root 'resources\app.ico' }

$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256

function New-IconBitmap([int]$size) {
  $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
  $g.Clear([System.Drawing.Color]::Transparent)

  $pad = [Math]::Max(1, [int]($size * 0.06))
  $rect = New-Object System.Drawing.Rectangle($pad, $pad, ($size - 2 * $pad), ($size - 2 * $pad))
  $radius = [Math]::Max(2, [int]($size * 0.18))

  $path = New-Object System.Drawing.Drawing2D.GraphicsPath
  $d = $radius * 2
  $path.AddArc($rect.X, $rect.Y, $d, $d, 180, 90)
  $path.AddArc($rect.Right - $d, $rect.Y, $d, $d, 270, 90)
  $path.AddArc($rect.Right - $d, $rect.Bottom - $d, $d, $d, 0, 90)
  $path.AddArc($rect.X, $rect.Bottom - $d, $d, $d, 90, 90)
  $path.CloseFigure()

  $brush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 171, 58, 44))
  $g.FillPath($brush, $path)

  if ($size -ge 24) {
    $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(70, 253, 246, 232), 1)
    $g.DrawPath($pen, $path)
    $pen.Dispose()
  }

  $fontSize = [float]($size * 0.62)
  $font = New-Object System.Drawing.Font('Georgia', $fontSize, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
  $fmt = New-Object System.Drawing.StringFormat
  $fmt.Alignment = [System.Drawing.StringAlignment]::Center
  $fmt.LineAlignment = [System.Drawing.StringAlignment]::Center
  $textBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 253, 246, 232))
  $box = New-Object System.Drawing.RectangleF(0, 0, $size, $size)
  $g.DrawString('T', $font, $textBrush, $box, $fmt)

  $textBrush.Dispose(); $fmt.Dispose(); $font.Dispose(); $brush.Dispose(); $path.Dispose(); $g.Dispose()
  return $bmp
}

# ICO 容器：每帧直接塞 PNG 数据（Vista 以后都认）
$pngs = @()
foreach ($s in $sizes) {
  $bmp = New-IconBitmap $s
  $ms = New-Object System.IO.MemoryStream
  $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
  $pngs += ,@($s, $ms.ToArray())
  $ms.Dispose(); $bmp.Dispose()
}

$fs = [System.IO.File]::Create($Out)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([UInt16]0)                 # reserved
$bw.Write([UInt16]1)                 # type: icon
$bw.Write([UInt16]$pngs.Count)

$offset = 6 + 16 * $pngs.Count
foreach ($p in $pngs) {
  $size = $p[0]
  $dim = if ($size -ge 256) { 0 } else { $size }
  $bw.Write([byte]$dim)              # width
  $bw.Write([byte]$dim)              # height
  $bw.Write([byte]0)                 # palette
  $bw.Write([byte]0)                 # reserved
  $bw.Write([UInt16]1)               # planes
  $bw.Write([UInt16]32)              # bpp
  $bw.Write([UInt32]$p[1].Length)
  $bw.Write([UInt32]$offset)
  $offset += $p[1].Length
}
foreach ($p in $pngs) { $bw.Write($p[1]) }

$bw.Flush(); $bw.Close(); $fs.Close()
Write-Host "已生成 $Out ($([Math]::Round((Get-Item $Out).Length / 1KB, 1)) KB, $($pngs.Count) 帧)" -ForegroundColor Green
