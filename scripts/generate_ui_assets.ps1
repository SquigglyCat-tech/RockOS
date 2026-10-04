param(
    [Parameter(Mandatory = $true)]
    [string]$FontPath,
    [Parameter(Mandatory = $true)]
    [string]$LogoPath,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

[StructLayout(LayoutKind.Sequential)]
public struct RockOSKerningPair {
    public ushort First;
    public ushort Second;
    public int Amount;
}

public static class RockOSFontKerning {
    [DllImport("gdi32.dll", EntryPoint = "GetKerningPairsW")]
    public static extern uint GetKerningPairs(
        IntPtr deviceContext, uint count,
        [Out] RockOSKerningPair[] pairs);

    [DllImport("gdi32.dll", EntryPoint = "SelectObject")]
    public static extern IntPtr SelectObject(
        IntPtr deviceContext, IntPtr graphicObject);

    [DllImport("gdi32.dll", EntryPoint = "DeleteObject")]
    public static extern bool DeleteObject(IntPtr graphicObject);
}
'@

$fontWidth = 24
$fontHeight = 20
$logoWidth = 480
$logoHeight = 116
$packing = 4
$fontAdvanceScale = 4
$fontCollection = New-Object System.Drawing.Text.PrivateFontCollection
$fontCollection.AddFontFile((Resolve-Path $FontPath).Path)
if ($fontCollection.Families.Count -eq 0) {
    throw "No font family was loaded from $FontPath"
}
$font = New-Object System.Drawing.Font(
    $fontCollection.Families[0],
    14,
    [System.Drawing.FontStyle]::Regular,
    [System.Drawing.GraphicsUnit]::Pixel
)
$typographicFormat = [System.Drawing.StringFormat]::GenericTypographic.Clone()
$typographicFormat.FormatFlags = $typographicFormat.FormatFlags -bor
    [System.Drawing.StringFormatFlags]::MeasureTrailingSpaces
$whiteBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)

function Get-CoverageLevel([int]$value) {
    $level = [int][Math]::Round($value * 3.0 / 255.0)
    if ($level -lt 0) { return 0 }
    if ($level -gt 3) { return 3 }
    return $level
}

function Get-PackedCoverage([System.Drawing.Bitmap]$Bitmap, [bool]$Logo) {
    $packed = New-Object 'System.Collections.Generic.List[byte]'
    $current = 0
    for ($index = 0; $index -lt ($Bitmap.Width * $Bitmap.Height); $index++) {
        $x = $index % $Bitmap.Width
        $y = [Math]::Floor($index / $Bitmap.Width)
        $pixel = $Bitmap.GetPixel($x, $y)
        if ($Logo) {
            $luminance = [int](($pixel.R * 299 + $pixel.G * 587 +
                $pixel.B * 114) / 1000)
            $coverage = [Math]::Min(255,
                [Math]::Max(0, [int]((255 - $luminance) * 255 / 180)))
        } else {
            $coverage = $pixel.R
        }

        $shift = 6 - (($index % $packing) * 2)
        $current = $current -bor ((Get-CoverageLevel $coverage) -shl $shift)
        if (($index % $packing) -eq ($packing - 1)) {
            $packed.Add([byte]$current)
            $current = 0
        }
    }
    if (($Bitmap.Width * $Bitmap.Height) % $packing) {
        $packed.Add([byte]$current)
    }
    return ,$packed.ToArray()
}

function Add-CArray(
    [System.Text.StringBuilder]$Builder,
    [string]$Name,
    [byte[]]$Bytes
) {
    [void]$Builder.Append("static const uint8_t $Name[] = {`n")
    for ($index = 0; $index -lt $Bytes.Length; $index++) {
        if (($index % 16) -eq 0) {
            [void]$Builder.Append('    ')
        }
        [void]$Builder.Append(('0x{0:X2}' -f $Bytes[$index]))
        if ($index + 1 -lt $Bytes.Length) {
            [void]$Builder.Append(', ')
        }
        if (($index % 16) -eq 15 -or $index + 1 -eq $Bytes.Length) {
            [void]$Builder.Append("`n")
        }
    }
    [void]$Builder.Append("};`n`n")
}

$fontBytes = New-Object 'System.Collections.Generic.List[byte]'
$fontAdvances = New-Object 'System.Collections.Generic.List[byte]'
$fontAdvancesQ = New-Object 'System.Collections.Generic.List[int]'
$fontGraphicsBitmap = New-Object System.Drawing.Bitmap(
    128, 32, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
)
$fontGraphics = [System.Drawing.Graphics]::FromImage($fontGraphicsBitmap)
$fontGraphics.TextRenderingHint =
    [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
for ($character = 32; $character -le 126; $character++) {
    $glyph = New-Object System.Drawing.Bitmap(
        $fontWidth, $fontHeight, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
    )
    $graphics = [System.Drawing.Graphics]::FromImage($glyph)
    $graphics.Clear([System.Drawing.Color]::Black)
    $graphics.TextRenderingHint =
        [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $graphics.DrawString([string][char]$character, $font, $whiteBrush,
        [System.Drawing.PointF]::Empty, $typographicFormat)
    $fontBytes.AddRange([byte[]](Get-PackedCoverage $glyph $false))
    $measure = $fontGraphics.MeasureString([string][char]$character, $font,
        [System.Drawing.PointF]::Empty, $typographicFormat)
    $advanceQ = [int][Math]::Round($measure.Width * $fontAdvanceScale)
    if ($advanceQ -lt 1 -or $advanceQ -gt 255) {
        throw "Unexpected UI font advance for character $character`: $advanceQ"
    }
    $fontAdvancesQ.Add($advanceQ)
    $fontAdvances.Add([byte]$advanceQ)
    $graphics.Dispose()
    $glyph.Dispose()
}

$fontKerning = New-Object 'System.Collections.Generic.List[int]'
for ($index = 0; $index -lt (95 * 95); $index++) {
    $fontKerning.Add(0)
}
$hfont = $font.ToHfont()
$hdc = $fontGraphics.GetHdc()
$oldFont = [RockOSFontKerning]::SelectObject($hdc, $hfont)
try {
    $nativeKerningCount = [RockOSFontKerning]::GetKerningPairs($hdc, 0, $null)
    if ($nativeKerningCount -gt 0) {
        $nativeKerning = New-Object 'RockOSKerningPair[]' `
            ([int]$nativeKerningCount)
        $actualKerningCount = [RockOSFontKerning]::GetKerningPairs(
            $hdc, $nativeKerningCount, $nativeKerning
        )
        for ($index = 0; $index -lt $actualKerningCount; $index++) {
            $pair = $nativeKerning[$index]
            if ($pair.First -lt 32 -or $pair.First -gt 126 -or
                $pair.Second -lt 32 -or $pair.Second -gt 126) {
                continue
            }
            $kerningQ = $pair.Amount * $fontAdvanceScale
            if ($kerningQ -lt -128 -or $kerningQ -gt 127) {
                throw "Unexpected UI font kerning amount: $kerningQ"
            }
            $pairIndex = ($pair.First - 32) * 95 + ($pair.Second - 32)
            $fontKerning[$pairIndex] = $kerningQ
        }
    }
} finally {
    [void][RockOSFontKerning]::SelectObject($hdc, $oldFont)
    $fontGraphics.ReleaseHdc($hdc)
    [void][RockOSFontKerning]::DeleteObject($hfont)
}
$fontGraphics.Dispose()
$fontGraphicsBitmap.Dispose()

$sourceLogo = [System.Drawing.Bitmap]::FromFile($LogoPath)
$logo = New-Object System.Drawing.Bitmap(
    $logoWidth, $logoHeight, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
)
$logoGraphics = [System.Drawing.Graphics]::FromImage($logo)
$logoGraphics.Clear([System.Drawing.Color]::White)
$logoGraphics.InterpolationMode =
    [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$logoGraphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
$logoGraphics.DrawImage($sourceLogo, 0, 0, $logoWidth, $logoHeight)
$logoBytes = Get-PackedCoverage $logo $true

$builder = New-Object System.Text.StringBuilder
[void]$builder.Append("#ifndef ROCKOS_UI_ASSETS_H`n#define ROCKOS_UI_ASSETS_H`n`n")
[void]$builder.Append("#define ROCKOS_UI_FONT_CELL_WIDTH 24U`n")
[void]$builder.Append("#define ROCKOS_UI_FONT_HEIGHT 20U`n")
[void]$builder.Append("#define ROCKOS_UI_FONT_FIRST_CHAR 32U`n")
[void]$builder.Append("#define ROCKOS_UI_FONT_CHAR_COUNT 95U`n")
[void]$builder.Append("#define ROCKOS_UI_FONT_ADVANCE_SCALE 4U`n")
[void]$builder.Append("#define ROCKOS_LOGO_WIDTH 480U`n")
[void]$builder.Append("#define ROCKOS_LOGO_HEIGHT 116U`n`n")
Add-CArray $builder 'rockos_ui_font_2bpp' $fontBytes.ToArray()
Add-CArray $builder 'rockos_ui_font_advance_q' $fontAdvances.ToArray()
[void]$builder.Append("static const int8_t rockos_ui_font_kerning_q[] = {`n")
for ($index = 0; $index -lt $fontKerning.Count; $index++) {
    if (($index % 16) -eq 0) {
        [void]$builder.Append('    ')
    }
    [void]$builder.Append([string]$fontKerning[$index])
    if ($index + 1 -lt $fontKerning.Count) {
        [void]$builder.Append(', ')
    }
    if (($index % 16) -eq 15 -or $index + 1 -eq $fontKerning.Count) {
        [void]$builder.Append("`n")
    }
}
[void]$builder.Append("};`n`n")
Add-CArray $builder 'rockos_logo_2bpp' $logoBytes
[void]$builder.Append("#endif`n")

$outputDirectory = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$encoding = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($OutputPath, $builder.ToString(), $encoding)

$logoGraphics.Dispose()
$logo.Dispose()
$sourceLogo.Dispose()
$whiteBrush.Dispose()
$font.Dispose()
$fontCollection.Dispose()
$typographicFormat.Dispose()
