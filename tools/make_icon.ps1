# Generates the Filo logo and packs it into a multi-resolution .ico.
#
# The mark: a thread that loops once around itself and then straightens out,
# running dead flat to a dot. At 16px several curls turn to mush, so the
# drawing keeps ONE of them and thickens the stroke.

Add-Type -AssemblyName System.Drawing

# The "tangled" path: a spiral that starts tight at the centre, widens as it
# turns, and on the way out straightens into a line running to the dot.
# Returns the vertices in the 100x100 design space.
function Get-SpiralPoints {
    param([double]$Turns = 2.0)

    $cx = 38.0; $cy = 54.0
    $thetaStart = -[math]::PI / 2 - ($Turns * 2 * [math]::PI)
    $thetaEnd   = -[math]::PI / 2      # exits at the top, tangent to the right
    $rStart = 4.0; $rEnd = 27.0

    $points = @()
    $steps = 110
    for ($i = 0; $i -le $steps; $i++) {
        $t = $i / $steps
        $theta = $thetaStart + $t * ($thetaEnd - $thetaStart)
        # the radius grows faster than linearly: the inner turns stay tight
        # (the tangle) and the outer ones open up (the unravelling)
        $r = $rStart + ($rEnd - $rStart) * [math]::Pow($t, 1.35)
        $points += New-Object System.Drawing.PointF `
            ([float]($cx + $r * [math]::Cos($theta))), ([float]($cy + $r * [math]::Sin($theta)))
    }

    # The way out: from the top of the spiral it eases down onto the baseline
    # and carries straight on. These are the points that turn a loop into
    # something that straightens out.
    $points += New-Object System.Drawing.PointF ([float]52), ([float]27.5)
    $points += New-Object System.Drawing.PointF ([float]60), ([float]33)
    $points += New-Object System.Drawing.PointF ([float]64), ([float]43)
    $points += New-Object System.Drawing.PointF ([float]66), ([float]50)
    $points += New-Object System.Drawing.PointF ([float]74), ([float]50)
    return ,$points
}

function New-LogoBitmap {
    param([int]$Size, [System.Drawing.Color]$Color)

    $bmp = New-Object System.Drawing.Bitmap $Size, $Size,
        ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    # 100x100 design space, scaled to the size that was asked for.
    $k = $Size / 100.0

    # The spiral at EVERY size: it is the project's mark and has to be
    # recognisable everywhere. Below 40px the turns crowd together, so the
    # number of turns drops and the stroke thins out instead of the drawing
    # being swapped for another one: same shape, simplified.
    # The spiral covers a radius from 4 to 27 units: with two turns they sit
    # about 11 units apart. The stroke has to stay well under that distance or
    # the gap between the turns closes up and all that is left is a blob.
    # At small sizes turns go rather than thinning the stroke further: under
    # 2 pixels the stroke vanishes into the antialiasing.
    if ($Size -ge 96)     { $turns = 2.0;  $strokeUnits = 5.5 }
    elseif ($Size -ge 48) { $turns = 1.5;  $strokeUnits = 7.0 }
    elseif ($Size -ge 28) { $turns = 1.25; $strokeUnits = 9.0 }
    else                  { $turns = 1.0;  $strokeUnits = 11.0 }
    $w = $strokeUnits * $k
    $detailed = $true

    $pen = New-Object System.Drawing.Pen $Color, $w
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap   = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round

    if ($detailed) {
        $pts = Get-SpiralPoints -Turns $turns
        $scaled = @()
        foreach ($p in $pts) {
            $scaled += New-Object System.Drawing.PointF ([float]($p.X * $k)), ([float]($p.Y * $k))
        }
        # low tension: the curve goes through the points without swinging wide
        $g.DrawCurve($pen, [System.Drawing.PointF[]]$scaled, 0.4)
    } else {
        $path = New-Object System.Drawing.Drawing2D.GraphicsPath
        $path.AddArc((12 * $k), (29 * $k), (42 * $k), (42 * $k), 55, 290)
        $path.AddLine((54 * $k), (50 * $k), (74 * $k), (50 * $k))
        $g.DrawPath($pen, $path)
        $path.Dispose()
    }

    # The dot at the end: the file you were looking for.
    $dotUnits = 9.0
    if ($detailed) { $dotUnits = 7.5 }
    $r = $dotUnits * $k
    $brush = New-Object System.Drawing.SolidBrush $Color
    $g.FillEllipse($brush, (84 * $k - $r), (50 * $k - $r), (2 * $r), (2 * $r))

    $brush.Dispose(); $pen.Dispose(); $g.Dispose()
    return $bmp
}

# Turns a bitmap into the DIB layout icons expect: a BITMAPINFOHEADER with
# DOUBLE the height (image + mask), BGRA pixels stored bottom-up, and the AND
# mask tacked on the end (all zeroes: the real transparency rides in the alpha
# channel).
function ConvertTo-IconDib {
    param([System.Drawing.Bitmap]$Bitmap)

    $w = $Bitmap.Width; $h = $Bitmap.Height
    $rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
    $data = $Bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                             [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $pixels = New-Object byte[] ($data.Stride * $h)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $pixels, 0, $pixels.Length)
    $Bitmap.UnlockBits($data)

    $out = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter $out

    $bw.Write([uint32]40); $bw.Write([int32]$w); $bw.Write([int32](2 * $h))
    $bw.Write([uint16]1);  $bw.Write([uint16]32); $bw.Write([uint32]0)
    $bw.Write([uint32]($w * $h * 4))
    $bw.Write([int32]0); $bw.Write([int32]0); $bw.Write([uint32]0); $bw.Write([uint32]0)

    for ($y = $h - 1; $y -ge 0; $y--) {
        $bw.Write($pixels, $y * $data.Stride, $w * 4)
    }
    $maskStride = [int][math]::Floor((($w + 31) / 32)) * 4
    $bw.Write((New-Object byte[] ($maskStride * $h)))

    $bw.Flush()
    $bytes = $out.ToArray()
    $bw.Dispose(); $out.Dispose()
    # The comma is mandatory: without it PowerShell unrolls the byte[] into the
    # pipeline and the caller gets back an Object[]. The length looks right,
    # but BinaryWriter.Write no longer finds the overload and writes nothing.
    return ,$bytes
}

# --- building the .ico container --------------------------------------------
# Layout: ICONDIR(6 bytes) + one ICONDIRENTRY(16 bytes) each + the data after.
#
# EVERY entry is a DIB, the 256 included. Icons have been allowed to hold PNGs
# since Windows Vista, but rc.exe has NOT: it tries to read them as DIBs, finds
# the PNG signature where the header should be, and fails with RC2176
# "old DIB".
function Save-Ico {
    param([int[]]$Sizes, [System.Drawing.Color]$Color, [string]$Path)

    $payloads = @()
    foreach ($size in $Sizes) {
        $bmp = New-LogoBitmap -Size $size -Color $Color
        $payloads += ,@($size, (ConvertTo-IconDib -Bitmap $bmp))
        $bmp.Dispose()
    }

    $out = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter $out

    $bw.Write([uint16]0)                    # reserved
    $bw.Write([uint16]1)                    # type = icon
    $bw.Write([uint16]$payloads.Count)

    $offset = 6 + 16 * $payloads.Count
    foreach ($p in $payloads) {
        $size = $p[0]; $bytes = $p[1]
        # 256 is encoded as 0 in a single byte
        $bw.Write([byte]($(if ($size -ge 256) { 0 } else { $size })))
        $bw.Write([byte]($(if ($size -ge 256) { 0 } else { $size })))
        $bw.Write([byte]0)                  # colours in the palette
        $bw.Write([byte]0)                  # reserved
        $bw.Write([uint16]1)                # planes
        $bw.Write([uint16]32)               # bits per pixel
        $bw.Write([uint32]$bytes.Length)
        $bw.Write([uint32]$offset)
        $offset += $bytes.Length
    }
    foreach ($p in $payloads) { $bw.Write([byte[]]$p[1]) }

    $bw.Flush()
    [System.IO.File]::WriteAllBytes($Path, $out.ToArray())
    $bw.Dispose(); $out.Dispose()
}

# The output lives in the repository, next to the .rc that embeds it, so the
# paths come from the script's own location: nothing to configure.
$repoRoot    = Split-Path -Parent $PSScriptRoot
$icoPath     = Join-Path $repoRoot 'res\filo.ico'
$previewPath = Join-Path $repoRoot 'res\logo-preview.png'

# Red: in the myth, and in every classical depiction of it, Ariadne's thread is
# red.
$brand = [System.Drawing.Color]::FromArgb(255, 214, 48, 49)
# 128 is left out: Windows gets it by scaling the 256, and a DIB at that size
# would cost 66 KB for nothing.
Save-Ico -Sizes @(16, 20, 24, 32, 48, 64, 256) -Color $brand -Path $icoPath

# Large preview, to eyeball the result
$preview = New-LogoBitmap -Size 256 -Color $brand
$preview.Save($previewPath, [System.Drawing.Imaging.ImageFormat]::Png)
$preview.Dispose()

Write-Output "icon generated: $icoPath"
