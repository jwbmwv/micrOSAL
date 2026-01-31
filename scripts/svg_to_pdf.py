#!/usr/bin/env python3
"""Convert SVG files to PDF using available backends.
"""
import sys
import pathlib
import subprocess


def convert(svg_path, pdf_path):
    # Try cairosvg
    try:
        import cairosvg
        cairosvg.svg2pdf(url=str(svg_path), write_to=str(pdf_path))
        return True
    except Exception:
        pass

    # Try rsvg-convert
    try:
        subprocess.check_call(['rsvg-convert', '-f', 'pdf', '-o', str(pdf_path), str(svg_path)])
        return True
    except Exception:
        pass

    # Try inkscape
    try:
        subprocess.check_call(['inkscape', str(svg_path), '--export-type=pdf', '--export-filename', str(pdf_path)])
        return True
    except Exception:
        pass

    return False


def main():
    if len(sys.argv) < 3:
        print('Usage: svg_to_pdf.py <input.svg> <output.pdf>')
        sys.exit(1)
    svg = pathlib.Path(sys.argv[1])
    pdf = pathlib.Path(sys.argv[2])
    ok = convert(svg, pdf)
    if not ok:
        print('Failed to convert SVG to PDF: no backend available')
        sys.exit(2)


if __name__ == '__main__':
    main()
