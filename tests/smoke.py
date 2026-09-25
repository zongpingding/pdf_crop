#!/usr/bin/env python3
"""End-to-end export checks using a small PDF built with the standard library."""

import argparse
import subprocess
import tempfile
from pathlib import Path


def make_pdf(path: Path, rotated: bool = False, mixed_sizes: bool = False) -> None:
    streams = [
        b"BT /F1 24 Tf 80 700 Td (FIRST PAGE) Tj ET\n0 0 0 RG 80 680 220 2 re f\n",
        b"BT /F1 24 Tf 180 400 Td (SECOND PAGE) Tj ET\n0 0 0 RG 180 380 220 2 re f\n",
        b"BT /F1 24 Tf 80 700 Td (THIRD PAGE) Tj ET\n0 0 0 RG 80 680 220 2 re f\n",
    ]
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [4 0 R 6 0 R 8 0 R] /Count 3 >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    for index, stream in enumerate(streams):
        rotation = " /Rotate 90" if rotated and index == 2 else ""
        mediabox = "[0 0 400 400]" if mixed_sizes and index == 1 else "[0 0 612 792]"
        objects.append((
            f"<< /Type /Page /Parent 2 0 R /MediaBox {mediabox}{rotation} "
            f"/Resources << /Font << /F1 3 0 R >> >> /Contents {5 + 2*index} 0 R >>"
        ).encode())
        objects.append(f"<< /Length {len(stream)} >>\nstream\n".encode() + stream + b"endstream")
    pdf = bytearray(b"%PDF-1.4\n")
    offsets = [0]
    for number, obj in enumerate(objects, 1):
        offsets.append(len(pdf))
        pdf.extend(f"{number} 0 obj\n".encode() + obj + b"\nendobj\n")
    xref = len(pdf)
    pdf.extend(f"xref\n0 {len(objects)+1}\n0000000000 65535 f \n".encode())
    for offset in offsets[1:]:
        pdf.extend(f"{offset:010d} 00000 n \n".encode())
    pdf.extend(f"trailer\n<< /Size {len(objects)+1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode())
    path.write_bytes(pdf)


def run(*args: str) -> str:
    result = subprocess.run(args, check=True, text=True, capture_output=True)
    return result.stdout


def page_count(path: Path) -> int:
    for line in run("pdfinfo", str(path)).splitlines():
        if line.startswith("Pages:"):
            return int(line.split(":", 1)[1])
    raise AssertionError("pdfinfo returned no page count")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    binary = str(parser.parse_args().binary.resolve())
    with tempfile.TemporaryDirectory(prefix="prop-test-") as directory:
        root = Path(directory)
        source = root / "source.pdf"
        make_pdf(source)
        grid = root / "grid.pdf"
        run(binary, "--go", "--whichpages", "1,3", "--grid", "2x1",
            "-o", str(grid), str(source))
        assert page_count(grid) == 4
        text = run("pdftotext", str(grid), "-")
        assert "FIRST PAGE" in text and "THIRD PAGE" in text and "SECOND PAGE" not in text

        progression = root / "progression.pdf"
        run(binary, "--go", "--whichpages", "2x+1", "-o", str(progression), str(source))
        assert page_count(progression) == 1
        assert "THIRD PAGE" in run("pdftotext", str(progression), "-")

        separate = root / "separate"
        run(binary, "--go", "--separate", "--whichpages", "1,3", "--grid", "2x1",
            "-o", str(separate), str(source))
        outputs = sorted(separate.glob("*.pdf"))
        assert [item.name for item in outputs] == [
            "page-01-selection-01.pdf", "page-01-selection-02.pdf",
            "page-03-selection-01.pdf", "page-03-selection-02.pdf",
        ]
        assert all(page_count(item) == 1 for item in outputs)
        assert "THIRD PAGE" in run("pdftotext", str(outputs[-2]), "-")
        repeated = subprocess.run(
            [binary, "--go", "--separate", "--grid", "2x1", "-o", str(separate),
             str(source)], text=True, capture_output=True)
        assert repeated.returncode != 0 and len(list(separate.glob("*.pdf"))) == 4

        separate_raster = root / "separate-raster"
        run(binary, "--go", "--separate", "--strict", "--grid", "2x1",
            "--whichpages", "1", "-o", str(separate_raster), str(source))
        raster_outputs = sorted(separate_raster.glob("*.pdf"))
        assert len(raster_outputs) == 2
        assert all(page_count(item) == 1 for item in raster_outputs)
        assert all("FIRST PAGE" not in run("pdftotext", str(item), "-")
                   for item in raster_outputs)

        trimmed = root / "trimmed.pdf"
        run(binary, "--go", "--trim", "--trim-use", "all", "-o", str(trimmed), str(source))
        assert page_count(trimmed) == 3
        assert "612 x 792" not in run("pdfinfo", str(trimmed))

        raster = root / "raster.pdf"
        run(binary, "--go", "--strict", "--grid", "2x1", "--whichpages", "1,3",
            "-o", str(raster), str(source))
        assert page_count(raster) == 4
        assert "FIRST PAGE" not in run("pdftotext", str(raster), "-")

        device = root / "device.pdf"
        run(binary, "--go", "--device", "730x600", "--whichpages", "2",
            "-o", str(device), str(source))
        assert page_count(device) == 2

        rotated_source = root / "rotated-source.pdf"
        make_pdf(rotated_source, rotated=True)
        rotated = root / "rotated-output.pdf"
        run(binary, "--go", "--whichpages", "3", "--rotate", "90",
            "-o", str(rotated), str(rotated_source))
        assert page_count(rotated) == 1
        assert "Page rot:        180" in run("pdfinfo", str(rotated))
        assert "THIRD PAGE" in run("pdftotext", str(rotated), "-")
        complete_rotated = root / "complete-rotated.pdf"
        run(binary, "--go", "--complete-pages", "--whichpages", "3",
            "-o", str(complete_rotated), str(rotated_source))
        assert "Page rot:        90" in run("pdfinfo", str(complete_rotated))

        complete = root / "complete.pdf"
        run(binary, "--go", "--complete-pages", "--whichpages", "1,3",
            "-o", str(complete), str(source))
        assert page_count(complete) == 2
        assert "612 x 792" in run("pdfinfo", str(complete))
        complete_text = run("pdftotext", str(complete), "-")
        assert "FIRST PAGE" in complete_text and "THIRD PAGE" in complete_text
        assert "SECOND PAGE" not in complete_text

        mixed_source = root / "mixed-source.pdf"
        make_pdf(mixed_source, mixed_sizes=True)
        mixed_output = root / "mixed-output.pdf"
        run(binary, "--go", "--strict", "-o", str(mixed_output), str(mixed_source))
        assert "400 x 400" in run("pdfinfo", "-f", "2", "-l", "2", str(mixed_output))

        invalid = root / "invalid.pdf"
        result = subprocess.run([binary, "--go", "--whichpages", "0-2", "-o", str(invalid),
                                 str(source)], text=True, capture_output=True)
        assert result.returncode != 0 and not invalid.exists()


if __name__ == "__main__":
    main()
