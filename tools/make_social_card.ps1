# Builds the social preview card: the picture GitHub shows when somebody shares
# a link to the repository on X, Slack, Reddit or anywhere else that unfurls a
# URL.
#
# GitHub renders that slot at 1280x640, a 2:1 rectangle. The application icon is
# a 256x256 square, and uploading it there gets it letterboxed between two large
# empty margins — which is what most repositories do and why most repositories
# have an ugly card.
#
# Run from the repository root:
#   powershell -ExecutionPolicy Bypass -File tools\make_social_card.ps1
#
# Then: Settings > General > Social preview > Upload an image.

Add-Type -AssemblyName System.Drawing

$root   = Split-Path -Parent $PSScriptRoot
$logo   = Join-Path $root 'docs\images\logo.png'
$output = Join-Path $root 'docs\images\social-card.png'

$W = 1280
$H = 640

# The application's own palette, so the card and the window look related.
$ink    = [System.Drawing.Color]::FromArgb(0x14, 0x17, 0x1C)
$dim    = [System.Drawing.Color]::FromArgb(0x6B, 0x72, 0x80)
$brand  = [System.Drawing.Color]::FromArgb(0xD6, 0x30, 0x31)
$paper  = [System.Drawing.Color]::White

$bitmap   = New-Object System.Drawing.Bitmap $W, $H
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.SmoothingMode     = 'AntiAlias'
$graphics.TextRenderingHint = 'ClearTypeGridFit'
$graphics.InterpolationMode = 'HighQualityBicubic'
$graphics.Clear($paper)

# A thick brand rule down the left edge: the card is mostly white, and without
# one anchor of colour it reads as an empty page in a timeline.
$brandBrush = New-Object System.Drawing.SolidBrush $brand
$graphics.FillRectangle($brandBrush, 0, 0, 14, $H)

# The mark, left of the words rather than above them: at card size a stacked
# layout leaves the text too small to read in a feed.
$mark = [System.Drawing.Image]::FromFile($logo)
$markSize = 240
$graphics.DrawImage($mark, 96, ($H - $markSize) / 2, $markSize, $markSize)
$mark.Dispose()

$textLeft = 96 + $markSize + 60

$titleFont = New-Object System.Drawing.Font 'Segoe UI', 108, ([System.Drawing.FontStyle]::Bold)
$leadFont  = New-Object System.Drawing.Font 'Segoe UI', 31
$subFont   = New-Object System.Drawing.Font 'Segoe UI', 24

$inkBrush = New-Object System.Drawing.SolidBrush $ink
$dimBrush = New-Object System.Drawing.SolidBrush $dim

# Laid out from a measured block rather than from constants, so changing a font
# size does not silently push the last line off the bottom of the card.
$titleH = $graphics.MeasureString('Filo', $titleFont).Height
$leadH  = $graphics.MeasureString('x', $leadFont).Height
$subH   = $graphics.MeasureString('x', $subFont).Height
$blockH = $titleH + 12 + ($leadH * 2) + 20 + $subH
$y = ($H - $blockH) / 2

$graphics.DrawString('Filo', $titleFont, $inkBrush, ($textLeft - 10), $y)
$y += $titleH + 12

# Two lines, and the order matters: what it does, then the part that makes it
# different from everything else that does it.
$graphics.DrawString('Find your files by name, by content,', $leadFont, $inkBrush, $textLeft, $y)
$y += $leadH
$graphics.DrawString('or by what they mean.', $leadFont, $inkBrush, $textLeft, $y)
$y += $leadH + 20

# The middle dot written by code point. Spelled literally, it survives only as
# long as everything reading this file agrees on the encoding, and PowerShell
# reading a UTF-8 file as the local codepage turns it into two characters.
$dot = [char]0x00B7
$graphics.DrawString("Windows   $dot   entirely local   $dot   one .exe",
                     $subFont, $dimBrush, $textLeft, $y)

$bitmap.Save($output, [System.Drawing.Imaging.ImageFormat]::Png)

$titleFont.Dispose(); $leadFont.Dispose(); $subFont.Dispose()
$inkBrush.Dispose(); $dimBrush.Dispose(); $brandBrush.Dispose()
$graphics.Dispose(); $bitmap.Dispose()

Write-Output "wrote $output ($W x $H)"
