# アイコンの元データ。Windows標準のSystem.Drawingだけで再生成できる。
Add-Type -AssemblyName System.Drawing
$iconDirectory = Join-Path $PSScriptRoot 'icons'
[System.IO.Directory]::CreateDirectory($iconDirectory) | Out-Null
function Color([string]$value) { return [System.Drawing.ColorTranslator]::FromHtml($value) }
function RoundedPath([single]$x, [single]$y, [single]$width, [single]$height, [single]$radius) {
    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $diameter = 2 * $radius
    $path.AddArc($x, $y, $diameter, $diameter, 180, 90)
    $path.AddArc($x + $width - $diameter, $y, $diameter, $diameter, 270, 90)
    $path.AddArc($x + $width - $diameter, $y + $height - $diameter, $diameter, $diameter, 0, 90)
    $path.AddArc($x, $y + $height - $diameter, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    return ,$path
}
function FillRounded($graphics, [string]$color, [single]$x, [single]$y, [single]$width, [single]$height, [single]$radius) {
    $brush = [System.Drawing.SolidBrush]::new((Color $color))
    $path = RoundedPath $x $y $width $height $radius
    $graphics.FillPath($brush, $path)
    $path.Dispose(); $brush.Dispose()
}
function Line($graphics, [string]$color, [single]$width, [single]$x1, [single]$y1, [single]$x2, [single]$y2) {
    $pen = [System.Drawing.Pen]::new((Color $color), $width)
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $graphics.DrawLine($pen, $x1, $y1, $x2, $y2)
    $pen.Dispose()
}
function DrawIcon([bool]$ui) {
    $bitmap = [System.Drawing.Bitmap]::new(1024, 1024, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.ScaleTransform(4, 4)
    $tile = RoundedPath 16 16 224 224 49
    $gradient = [System.Drawing.Drawing2D.LinearGradientBrush]::new([System.Drawing.PointF]::new(28, 20), [System.Drawing.PointF]::new(220, 240), (Color '#A18AFF'), (Color '#5931C6'))
    $graphics.FillPath($gradient, $tile)
    $edge = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(70, 255, 255, 255), 1.5)
    $graphics.DrawPath($edge, $tile)
    $edge.Dispose(); $gradient.Dispose(); $tile.Dispose()
    if ($ui) {
        FillRounded $graphics '#F6F3FF' 48 60 160 128 16
        Line $graphics '#DDD5F4' 2 49 94 207 94
        Line $graphics '#C8BBE6' 5 64 169 115 169
        Line $graphics '#C8BBE6' 5 128 169 144 169
        $letterColor = '#6746BC'
        $dotColor = '#A38ECF'
    } else {
        FillRounded $graphics '#211B3A' 48 60 160 128 16
        Line $graphics '#49405F' 2 49 94 207 94
        Line $graphics '#B9A4FF' 3 167 73 172 78
        Line $graphics '#B9A4FF' 3 172 78 167 83
        Line $graphics '#B9A4FF' 3 180 83 188 83
        Line $graphics '#67578F' 4 65 169 115 169
        $letterColor = '#F5F0FF'
        $dotColor = '#9281BA'
    }
    # 最後の文字は小文字のl。字形をパス化して縮小時にも輪郭を保つ。
    $letters = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $family = [System.Drawing.FontFamily]::new('Consolas')
    $format = [System.Drawing.StringFormat]::GenericTypographic.Clone()
    $letters.AddString('CPPl', $family, [int][System.Drawing.FontStyle]::Bold, [single]55, [System.Drawing.PointF]::new(62, 102), $format)
    $brush = [System.Drawing.SolidBrush]::new((Color $letterColor))
    $graphics.FillPath($brush, $letters)
    $brush.Dispose(); $letters.Dispose(); $family.Dispose(); $format.Dispose()
    foreach ($x in @(64, 77, 90)) { FillRounded $graphics $dotColor $x 75 6 6 3 }
    $state = $graphics.Save()
    $graphics.TranslateTransform(194, 190)
    $graphics.RotateTransform(42)
    $graphics.TranslateTransform(-186, -175)
    FillRounded $graphics '#85EDD2' 166 143 40 66 7
    FillRounded $graphics '#1B625C' 166 184 40 25 6
    $brush = [System.Drawing.SolidBrush]::new((Color '#1B625C'))
    $graphics.FillRectangle($brush, [single]166, [single]184, [single]40, [single]10)
    $brush.Dispose()
    Line $graphics '#E1FFF7' 2 168 184 204 184
    $path = RoundedPath 166 143 40 66 7
    $outline = [System.Drawing.Pen]::new((Color '#D8FFF4'), 2)
    $graphics.DrawPath($outline, $path)
    $outline.Dispose(); $path.Dispose()
    $graphics.Restore($state)
    $graphics.Dispose()
    return ,$bitmap
}
function Resize($source, [int]$size) {
    $bitmap = [System.Drawing.Bitmap]::new($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.DrawImage($source, [System.Drawing.Rectangle]::new(0, 0, $size, $size))
    $graphics.Dispose()
    return ,$bitmap
}
foreach ($name in @('delCppl', 'delCpplUI')) {
    $source = DrawIcon ($name -eq 'delCpplUI')
    $preview = Resize $source 512
    $preview.Save((Join-Path $iconDirectory ($name + '.png')), [System.Drawing.Imaging.ImageFormat]::Png)
    $preview.Dispose()
    $frames = [System.Collections.Generic.List[byte[]]]::new()
    $sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
    foreach ($size in $sizes) {
        $bitmap = Resize $source $size
        $stream = [System.IO.MemoryStream]::new()
        $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
        $frames.Add($stream.ToArray())
        $stream.Dispose(); $bitmap.Dispose()
    }
    $file = [System.IO.File]::Create((Join-Path $iconDirectory ($name + '.ico')))
    $writer = [System.IO.BinaryWriter]::new($file)
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; ++$i) {
        $sizeByte = if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
        $writer.Write([byte]$sizeByte); $writer.Write([byte]$sizeByte)
        $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$frames[$i].Length); $writer.Write([uint32]$offset)
        $offset += $frames[$i].Length
    }
    foreach ($frame in $frames) { $writer.Write([byte[]]$frame) }
    $writer.Dispose(); $source.Dispose()
}

# デザイン確認用。メイン画像と小さい表示サイズを並べる。
$sheet = [System.Drawing.Bitmap]::new(512, 276)
$graphics = [System.Drawing.Graphics]::FromImage($sheet)
$graphics.Clear((Color '#15171E'))
$graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$font = [System.Drawing.Font]::new('Segoe UI', 12, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$ink = [System.Drawing.SolidBrush]::new((Color '#E8E5F5'))
$format = [System.Drawing.StringFormat]::new()
$format.Alignment = [System.Drawing.StringAlignment]::Center
$names = @('delCppl', 'delCpplUI')
for ($i = 0; $i -lt 2; ++$i) {
    $image = [System.Drawing.Image]::FromFile((Join-Path $iconDirectory ($names[$i] + '.png')))
    $x = 48 + $i * 256
    $graphics.DrawImage($image, [System.Drawing.Rectangle]::new($x, 12, 160, 160))
    $graphics.DrawString($names[$i], $font, $ink, [System.Drawing.RectangleF]::new($x, 181, 160, 22), $format)
    $smallX = $x + 8
    foreach ($size in @(16, 24, 32, 48)) {
        $graphics.DrawImage($image, [System.Drawing.Rectangle]::new($smallX, (252 - $size), $size, $size))
        $smallX += $size + 10
    }
    $image.Dispose()
}
$sheet.Save((Join-Path $iconDirectory 'preview.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$graphics.Dispose(); $font.Dispose(); $ink.Dispose(); $format.Dispose(); $sheet.Dispose()
