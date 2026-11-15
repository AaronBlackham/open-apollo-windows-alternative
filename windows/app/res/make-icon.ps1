# Draws app.ico (16-256 px, PNG entries): a dark tile with three meter bars.
# Run: powershell -ExecutionPolicy Bypass -File make-icon.ps1
Add-Type -AssemblyName System.Drawing
$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$pngs = @()
foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)

    $r = [Math]::Max(2, [int]($s * 0.18))
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $w = $s - 1
    $path.AddArc(0, 0, 2 * $r, 2 * $r, 180, 90)
    $path.AddArc($w - 2 * $r, 0, 2 * $r, 2 * $r, 270, 90)
    $path.AddArc($w - 2 * $r, $w - 2 * $r, 2 * $r, 2 * $r, 0, 90)
    $path.AddArc(0, $w - 2 * $r, 2 * $r, 2 * $r, 90, 90)
    $path.CloseFigure()
    $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 30, 33, 37))), $path)

    # Three bars, heights 45% / 75% / 60%; the tallest has an amber top segment.
    $pad = [Math]::Max(3, [int]($s * 0.22))
    $gap = [Math]::Max(1, [int]($s * 0.08))
    $bw = [Math]::Max(2, [int](($s - 2 * $pad - 2 * $gap) / 3))
    $base = $s - $pad
    $heights = 0.45, 0.75, 0.60
    $light = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 214, 218, 222))
    $amber = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 226, 168, 64))
    $x = [int](($s - (3 * $bw + 2 * $gap)) / 2)
    for ($i = 0; $i -lt 3; $i++) {
        $h = [int](($s - 2 * $pad) * $heights[$i])
        $g.FillRectangle($light, $x, $base - $h, $bw, $h)
        if ($i -eq 1) {
            $cap = [Math]::Max(1, [int]($h * 0.22))
            $g.FillRectangle($amber, $x, $base - $h, $bw, $cap)
        }
        $x += $bw + $gap
    }
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    if ($s -ge 256) {
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    } else {
        # Classic DIB entry: BITMAPINFOHEADER (double height), BGRA bottom-up, 1-bpp AND mask.
        $w2 = New-Object System.IO.BinaryWriter $ms
        $w2.Write([UInt32]40); $w2.Write([Int32]$s); $w2.Write([Int32](2 * $s))
        $w2.Write([UInt16]1); $w2.Write([UInt16]32); $w2.Write([UInt32]0)
        $maskStride = [int]([Math]::Ceiling($s / 32.0) * 4)
        $w2.Write([UInt32]($s * $s * 4 + $maskStride * $s))
        $w2.Write([Int32]0); $w2.Write([Int32]0); $w2.Write([UInt32]0); $w2.Write([UInt32]0)
        for ($y = $s - 1; $y -ge 0; $y--) {
            for ($x2 = 0; $x2 -lt $s; $x2++) {
                $c = $bmp.GetPixel($x2, $y)
                $w2.Write([Byte]$c.B); $w2.Write([Byte]$c.G); $w2.Write([Byte]$c.R); $w2.Write([Byte]$c.A)
            }
        }
        for ($y = $s - 1; $y -ge 0; $y--) {
            $row = New-Object Byte[] $maskStride
            for ($x2 = 0; $x2 -lt $s; $x2++) {
                if ($bmp.GetPixel($x2, $y).A -eq 0) { $row[[int][Math]::Floor($x2 / 8)] = $row[[int][Math]::Floor($x2 / 8)] -bor (0x80 -shr ($x2 % 8)) }
            }
            $w2.Write($row)
        }
        $w2.Flush()
    }
    $bmp.Dispose()
    $pngs += , @($s, $ms.ToArray())
}

$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $out
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$pngs.Count)
$offset = 6 + 16 * $pngs.Count
foreach ($p in $pngs) {
    $dim = if ($p[0] -ge 256) { 0 } else { $p[0] }
    $bw.Write([Byte]$dim); $bw.Write([Byte]$dim); $bw.Write([Byte]0); $bw.Write([Byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$p[1].Length); $bw.Write([UInt32]$offset)
    $offset += $p[1].Length
}
foreach ($p in $pngs) { $bw.Write($p[1]) }
$bw.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $PSScriptRoot 'app.ico'), $out.ToArray())
"wrote app.ico ($($out.Length) bytes)"
