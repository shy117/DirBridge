#!/usr/bin/env python3
"""Generate DirBridge app PNG and ICO assets from the canonical SVG.

This script intentionally depends on Inkscape for SVG rendering and uses only
Python's standard library to package the generated PNG files into an ICO.
"""

from __future__ import annotations

import argparse
import shutil
import struct
import subprocess
import sys
from pathlib import Path


DEFAULT_SIZES = (16, 24, 32, 48, 64, 128, 256)


def find_inkscape(explicit_path: str | None) -> str:
    """Return an Inkscape executable path or stop with a clear error."""
    candidates = []
    if explicit_path:
        candidates.append(explicit_path)

    discovered = shutil.which("inkscape")
    if discovered:
        candidates.append(discovered)

    candidates.extend(
        [
            r"D:\Program Files\Inkscape\bin\inkscape.com",
            r"C:\Program Files\Inkscape\bin\inkscape.com",
            r"C:\Program Files\Inkscape\bin\inkscape.exe",
        ]
    )

    for candidate in candidates:
        path = Path(candidate)
        if path.exists():
            return str(path)

    raise SystemExit(
        "Inkscape was not found. Install Inkscape or pass --inkscape <path>."
    )


def render_png(inkscape: str, svg_path: Path, png_path: Path, size: int) -> None:
    """Render one square PNG from the SVG using Inkscape."""
    command = [
        inkscape,
        str(svg_path),
        "--export-type=png",
        f"--export-filename={png_path}",
        f"--export-width={size}",
        f"--export-height={size}",
    ]
    subprocess.run(command, check=True)


def write_png_ico(png_paths: list[Path], ico_path: Path) -> None:
    """Write an ICO file whose images are PNG-compressed entries."""
    entries = []
    image_data = []

    offset = 6 + 16 * len(png_paths)
    for png_path in png_paths:
        data = png_path.read_bytes()
        size = int(png_path.stem.rsplit("_", 1)[-1])
        width_byte = 0 if size >= 256 else size
        height_byte = 0 if size >= 256 else size
        entries.append(
            struct.pack(
                "<BBBBHHII",
                width_byte,
                height_byte,
                0,
                0,
                1,
                32,
                len(data),
                offset,
            )
        )
        image_data.append(data)
        offset += len(data)

    with ico_path.open("wb") as ico_file:
        ico_file.write(struct.pack("<HHH", 0, 1, len(entries)))
        for entry in entries:
            ico_file.write(entry)
        for data in image_data:
            ico_file.write(data)


def parse_sizes(raw_sizes: str) -> tuple[int, ...]:
    """Parse a comma-separated size list."""
    sizes = []
    for raw in raw_sizes.split(","):
        raw = raw.strip()
        if not raw:
            continue
        size = int(raw)
        if size <= 0 or size > 256:
            raise argparse.ArgumentTypeError("sizes must be in the range 1..256")
        sizes.append(size)
    if not sizes:
        raise argparse.ArgumentTypeError("at least one size is required")
    return tuple(dict.fromkeys(sizes))


def main() -> int:
    script_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(
        description="Generate DirBridge PNG and ICO app icons from dirbridge.svg."
    )
    parser.add_argument(
        "--svg",
        type=Path,
        default=script_dir / "dirbridge.svg",
        help="Source SVG path. Defaults to dirbridge.svg beside this script.",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=script_dir,
        help="Output directory. Defaults to this script's directory.",
    )
    parser.add_argument(
        "--sizes",
        type=parse_sizes,
        default=DEFAULT_SIZES,
        help="Comma-separated PNG/ICO sizes. Defaults to 16,24,32,48,64,128,256.",
    )
    parser.add_argument(
        "--inkscape",
        help="Optional explicit path to inkscape.com or inkscape.exe.",
    )
    args = parser.parse_args()

    svg_path = args.svg.resolve()
    out_dir = args.out_dir.resolve()
    if not svg_path.exists():
        raise SystemExit(f"SVG source does not exist: {svg_path}")

    out_dir.mkdir(parents=True, exist_ok=True)
    inkscape = find_inkscape(args.inkscape)

    png_paths = []
    for size in args.sizes:
        png_path = out_dir / f"dirbridge_{size}.png"
        render_png(inkscape, svg_path, png_path, size)
        png_paths.append(png_path)
        print(f"generated {png_path}")

    ico_path = out_dir / "dirbridge.ico"
    write_png_ico(png_paths, ico_path)
    print(f"generated {ico_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
