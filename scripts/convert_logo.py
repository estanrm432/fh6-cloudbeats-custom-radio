#!/usr/bin/env python3
"""
Convert a PNG logo into the Horizon CloudBeats radio-station swatchbin texture
and inject it into both Anthem.zip atlases (normal + HiRes).

Usage:
    python scripts/convert_logo.py path/to/logo.png

Requirements:
    - Pillow  (pip install Pillow)
    - texconv.exe  in the repo root or on PATH
      (download: https://github.com/microsoft/DirectXTex/releases/latest/download/texconv.exe)

How it works
------------
The radio-mod's original Streamer_Mode carrier was 196x196 / 392x392 (square)
with a non-standard 224/236-byte header and a full mip chain. The game renders
radio logos at their native aspect ratio, but that square, oddly-structured
carrier displayed stretched and blurry.

Instead we rebuild Streamer_Mode.swatchbin from a *stock* game logo whose layout
the engine handles correctly: a 140-byte header, single-mip BC7. We pick
Epitaph Records (210x180 / 420x360, aspect ~1.17 — the closest stock logo to a
square) as the structural template, then splice in the user's logo:

    1. Read the template logo's header (140 bytes) and exact dimensions.
    2. Crop the source PNG to content, pad to the template's aspect ratio
       (so nothing is stretched), resize to the template dimensions.
    3. BC7-compress as a SINGLE mip (matching the stock layout) via texconv.
    4. Concatenate: template header + new BC7 payload (identical byte length).
    5. Write it back as Streamer_Mode.swatchbin in both atlases.
"""

import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

REPO_ROOT = Path(__file__).resolve().parent.parent
MEDIA_DIR = REPO_ROOT / "media" / "UI" / "Textures"
ANTHEM_NORMAL = MEDIA_DIR / "Anthem.zip"
ANTHEM_HIRES  = MEDIA_DIR / "HiRes" / "Anthem.zip"

TARGET = "HUD/RadioLogos/Streamer_Mode.swatchbin"
# Structural template: a stock logo with a clean 140-byte single-mip layout
# and a near-square aspect ratio (210x180 / 420x360, ~1.17).
TEMPLATE = "HUD/RadioLogos/Epitaph_Records.swatchbin"

DDS_HEADER_SIZE       = 4 + 124   # 'DDS ' + DDS_HEADER
DDS_HEADER_DXT10_SIZE = 20        # DDS_HEADER_DXT10 (present for BC7)

# Fraction of the texture frame the logo content should occupy. <1 leaves a
# transparent margin so the logo renders a bit smaller in-game.
LOGO_SCALE = 0.87
# Per-axis tweak. Keep 1.0/1.0 to preserve the source aspect (a text logo
# must not be distorted); adjust only to pre-compensate game-side stretching.
LOGO_W_FACTOR = 0.93
LOGO_H_FACTOR = 1.00


def find_texconv() -> Path:
    candidate = REPO_ROOT / "texconv.exe"
    if candidate.exists():
        return candidate
    found = shutil.which("texconv") or shutil.which("texconv.exe")
    if found:
        return Path(found)
    sys.exit(
        "texconv.exe not found.\n"
        "Download from https://github.com/microsoft/DirectXTex/releases/latest/download/texconv.exe\n"
        f"and place it at {candidate}"
    )


def swatchbin_dims(data: bytes) -> tuple[int, int, int]:
    """Return (width, height, header_size) parsed from a swatchbin."""
    header_size = struct.unpack_from("<I", data, 8)[0]
    hcxt = data.find(b"HCXT")
    if hcxt < 0:
        sys.exit("template swatchbin has no HCXT block")
    w = struct.unpack_from("<I", data, hcxt + 0x20)[0]
    h = struct.unpack_from("<I", data, hcxt + 0x24)[0]
    return w, h, header_size


def png_to_bc7_single_mip(png_path: Path, w: int, h: int, texconv: Path) -> bytes:
    """Crop→pad-to-aspect→resize to wxh, BC7 single-mip, return raw payload."""
    with Image.open(png_path) as img:
        img = img.convert("RGBA")
        # Trim transparent margins so the logo isn't biased to one edge.
        bbox = img.getbbox()
        if bbox:
            img = img.crop(bbox)
        # Fit the content into LOGO_SCALE of the wxh frame, preserving aspect,
        # then centre it on a transparent canvas. This avoids distortion and
        # leaves a uniform margin so the logo renders slightly smaller in-game.
        cw, ch = img.size
        box_w, box_h = w * LOGO_SCALE, h * LOGO_SCALE
        ratio = min(box_w / cw, box_h / ch)
        # Apply per-axis pre-compensation on top of the uniform fit.
        sw = max(1, round(cw * ratio * LOGO_W_FACTOR))
        sh = max(1, round(ch * ratio * LOGO_H_FACTOR))
        # Never exceed the frame.
        sw = min(sw, w)
        sh = min(sh, h)
        scaled = img.resize((sw, sh), Image.LANCZOS)
        canvas = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        canvas.paste(scaled, ((w - sw) // 2, (h - sh) // 2))
        img = canvas

        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            src_png = tmp / "logo.png"
            img.save(src_png)
            result = subprocess.run(
                [
                    str(texconv),
                    "-f", "BC7_UNORM",
                    "-m", "1",        # SINGLE mip — matches the stock logo layout
                    "-bc", "x",       # exhaustive BC7 search (max quality)
                    "-y",
                    "-o", str(tmp),
                    str(src_png),
                ],
                capture_output=True, text=True,
            )
            if result.returncode != 0:
                print(result.stdout)
                print(result.stderr)
                sys.exit(f"texconv failed (exit {result.returncode})")
            dds_path = tmp / "logo.DDS"
            if not dds_path.exists():
                sys.exit("texconv did not produce a DDS file.")
            dds = dds_path.read_bytes()

    if dds[:4] != b"DDS ":
        sys.exit(f"Unexpected DDS magic: {dds[:4]!r}")
    return dds[DDS_HEADER_SIZE + DDS_HEADER_DXT10_SIZE:]


def rebuild_from_template(template: bytes, new_bc7: bytes) -> bytes:
    """Template header (140B) + new single-mip BC7 payload of identical length."""
    _, _, header_size = swatchbin_dims(template)
    template_payload = len(template) - header_size
    if template_payload != len(new_bc7):
        sys.exit(
            f"payload size mismatch: template expects {template_payload}, "
            f"got {len(new_bc7)} — dimensions must match the template exactly."
        )
    header = bytearray(template[:header_size])
    # Total-file-size field at offset 12 stays the same (identical payload size),
    # but rewrite it defensively.
    struct.pack_into("<I", header, 12, header_size + len(new_bc7))
    return bytes(header) + new_bc7


def update_zip(zip_path: Path, entry: str, data: bytes, template_info: zipfile.ZipInfo) -> None:
    tmp_path = zip_path.with_suffix(".zip.tmp")
    with zipfile.ZipFile(zip_path, "r") as src, \
         zipfile.ZipFile(tmp_path, "w", compression=zipfile.ZIP_STORED) as dst:
        wrote = False
        for item in src.infolist():
            if item.filename == entry:
                dst.writestr(item, data)
                wrote = True
            else:
                dst.writestr(item, src.read(item.filename))
        if not wrote:
            # TARGET wasn't present (shouldn't happen) — add it using template meta.
            info = zipfile.ZipInfo(entry)
            dst.writestr(info, data)
    tmp_path.replace(zip_path)
    print(f"  Rebuilt {zip_path.name} -> {entry}")


def main() -> None:
    if len(sys.argv) < 2:
        sys.exit(f"Usage: python {sys.argv[0]} path/to/logo.png")
    png_path = Path(sys.argv[1])
    if not png_path.exists():
        sys.exit(f"File not found: {png_path}")

    texconv = find_texconv()
    print(f"Using texconv: {texconv}")

    for zip_path in (ANTHEM_NORMAL, ANTHEM_HIRES):
        if not zip_path.exists():
            print(f"Warning: {zip_path} not found, skipping.")
            continue
        with zipfile.ZipFile(zip_path, "r") as z:
            if TEMPLATE not in z.namelist():
                sys.exit(f"template {TEMPLATE} not found in {zip_path.name}")
            template = z.read(TEMPLATE)
            tinfo = z.getinfo(TEMPLATE)

        w, h, hdr = swatchbin_dims(template)
        print(f"\nProcessing {zip_path.name}  (template {w}x{h}, header {hdr})…")

        new_bc7 = png_to_bc7_single_mip(png_path, w, h, texconv)
        swatch  = rebuild_from_template(template, new_bc7)
        update_zip(zip_path, TARGET, swatch, tinfo)

    print("\nDone! Rebuild/copy the mod and the new logo will be included.")


if __name__ == "__main__":
    main()
