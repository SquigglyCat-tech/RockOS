<#
    make_wallpaper.ps1 - turns a PNG/JPG into the raw RGB888 file that gets
    embedded in the kernel (src/drivers/wallpaper_data.S).

    Output: Width x Height x 3 bytes, R,G,B per pixel, rows top to bottom,
    no header. The kernel scales it to whatever the screen size is, so this
    is only the "master" resolution. Keep Width/Height in sync with
    WALL_SRC_W / WALL_SRC_H in src/kernel/desktop.c (the kernel checks the
    file size at runtime and falls back to a gradient if they disagree).

    Usually run by `make` (assets/wallpaper.png -> assets/wallpaper.rgb).
    Manual run:
        powershell -NoProfile -ExecutionPolicy Bypass -File scripts/make_wallpaper.ps1 `
            -InputPath assets/wallpaper.png -OutputPath assets/wallpaper.rgb
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$InputPath,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    [int]$Width = 960,
    [int]$Height = 640
)

Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class RockOSWallpaperPack {
    // 24bpp BGR (GDI+ order, rows possibly padded) -> tight RGB.
    public static byte[] ToRgb(Bitmap bitmap) {
        int w = bitmap.Width;
        int h = bitmap.Height;
        BitmapData data = bitmap.LockBits(new Rectangle(0, 0, w, h),
            ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
        try {
            byte[] result = new byte[w * h * 3];
            byte[] row = new byte[w * 3];
            for (int y = 0; y < h; y++) {
                IntPtr line = new IntPtr(data.Scan0.ToInt64() + (long)y * data.Stride);
                Marshal.Copy(line, row, 0, w * 3);
                int o = y * w * 3;
                for (int x = 0; x < w; x++) {
                    result[o + x * 3 + 0] = row[x * 3 + 2];
                    result[o + x * 3 + 1] = row[x * 3 + 1];
                    result[o + x * 3 + 2] = row[x * 3 + 0];
                }
            }
            return result;
        } finally {
            bitmap.UnlockBits(data);
        }
    }
}
'@

$inputFull = (Resolve-Path $InputPath).Path
$outputFull = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputPath)

$source = [System.Drawing.Bitmap]::FromFile($inputFull)
$scaled = New-Object System.Drawing.Bitmap(
    $Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb
)
$graphics = [System.Drawing.Graphics]::FromImage($scaled)
$graphics.InterpolationMode =
    [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
$graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
# Edge-clamped wrap avoids a faint dark/light fringe on the borders.
$wrap = New-Object System.Drawing.Imaging.ImageAttributes
$wrap.SetWrapMode([System.Drawing.Drawing2D.WrapMode]::TileFlipXY)
$destination = New-Object System.Drawing.Rectangle(0, 0, $Width, $Height)
$graphics.DrawImage($source, $destination, 0, 0, $source.Width, $source.Height,
    [System.Drawing.GraphicsUnit]::Pixel, $wrap)
$graphics.Dispose()

$bytes = [RockOSWallpaperPack]::ToRgb($scaled)

$outputDirectory = Split-Path -Parent $outputFull
if ($outputDirectory) {
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}
[System.IO.File]::WriteAllBytes($outputFull, $bytes)

Write-Host ("wallpaper: {0}x{1} -> {2}x{3}, {4} bytes -> {5}" -f
    $source.Width, $source.Height, $Width, $Height, $bytes.Length, $OutputPath)

$wrap.Dispose()
$scaled.Dispose()
$source.Dispose()
