"""Convert MEMORY_MAP_AND_SW_BLOCKS.md to a .docx file.

Renders markdown headings, paragraphs, tables, and fenced code blocks
(used for ASCII block diagrams). Uses python-docx — no Pandoc needed.

Usage:
    python tools/md_to_docx.py docs/MEMORY_MAP_AND_SW_BLOCKS.md \
                               docs/MEMORY_MAP_AND_SW_BLOCKS.docx
"""

import re
import sys
from pathlib import Path

from docx import Document
from docx.shared import Pt, RGBColor, Inches
from docx.enum.text import WD_ALIGN_PARAGRAPH


def parse_markdown_to_blocks(text: str):
    """Split markdown into a list of (kind, payload) blocks.

    kind in {'heading', 'paragraph', 'code', 'table', 'list', 'hr'}
    """
    lines = text.splitlines()
    blocks = []
    i = 0
    while i < len(lines):
        line = lines[i]

        # Horizontal rule
        if re.match(r'^---+\s*$', line):
            blocks.append(('hr', None))
            i += 1
            continue

        # Heading
        m = re.match(r'^(#{1,6})\s+(.*)$', line)
        if m:
            level = len(m.group(1))
            text_h = m.group(2).rstrip('#').strip()
            blocks.append(('heading', (level, text_h)))
            i += 1
            continue

        # Fenced code block
        if line.startswith('```'):
            j = i + 1
            code_lines = []
            while j < len(lines) and not lines[j].startswith('```'):
                code_lines.append(lines[j])
                j += 1
            blocks.append(('code', '\n'.join(code_lines)))
            i = j + 1 if j < len(lines) else j
            continue

        # Table (line starts with | and next line is separator)
        if line.lstrip().startswith('|') and i + 1 < len(lines) and re.match(
                r'^\s*\|?[-\s|:]+\|?\s*$', lines[i + 1]):
            rows = []
            while i < len(lines) and lines[i].lstrip().startswith('|'):
                # skip the separator row
                if re.match(r'^\s*\|?[-\s|:]+\|?\s*$', lines[i]):
                    i += 1
                    continue
                cells = [c.strip() for c in lines[i].strip().strip('|').split('|')]
                rows.append(cells)
                i += 1
            blocks.append(('table', rows))
            continue

        # Bulleted / numbered list
        if re.match(r'^\s*[-*]\s+', line) or re.match(r'^\s*\d+\.\s+', line):
            list_lines = []
            while i < len(lines) and (
                    re.match(r'^\s*[-*]\s+', lines[i]) or
                    re.match(r'^\s*\d+\.\s+', lines[i]) or
                    (lines[i].strip() and list_lines)):
                # Stop list if we hit a blank line followed by non-list
                if not lines[i].strip():
                    break
                list_lines.append(lines[i])
                i += 1
            blocks.append(('list', list_lines))
            continue

        # Blank line: skip
        if not line.strip():
            i += 1
            continue

        # Paragraph: accumulate until blank or special block
        para_lines = [line]
        i += 1
        while i < len(lines) and lines[i].strip() and \
                not lines[i].startswith('#') and \
                not lines[i].startswith('```') and \
                not lines[i].lstrip().startswith('|') and \
                not re.match(r'^\s*[-*]\s+', lines[i]) and \
                not re.match(r'^\s*\d+\.\s+', lines[i]) and \
                not re.match(r'^---+\s*$', lines[i]):
            para_lines.append(lines[i])
            i += 1
        blocks.append(('paragraph', ' '.join(para_lines)))

    return blocks


def apply_inline_formatting(paragraph, text: str):
    """Render **bold**, *italic*, `code`, and ⭐ symbols into a docx paragraph."""
    # Split into segments by the inline markers
    # Order matters: handle code spans first
    pattern = re.compile(r'(`[^`]+`|\*\*[^*]+\*\*|\*[^*]+\*)')
    parts = pattern.split(text)
    for part in parts:
        if not part:
            continue
        if part.startswith('`') and part.endswith('`'):
            run = paragraph.add_run(part[1:-1])
            run.font.name = 'Consolas'
            run.font.size = Pt(9)
            run.font.color.rgb = RGBColor(0x8B, 0x00, 0x00)
        elif part.startswith('**') and part.endswith('**'):
            run = paragraph.add_run(part[2:-2])
            run.bold = True
        elif part.startswith('*') and part.endswith('*') and len(part) > 2:
            run = paragraph.add_run(part[1:-1])
            run.italic = True
        else:
            paragraph.add_run(part)


def render_docx(blocks, out_path: Path, title: str):
    doc = Document()

    # Set narrow margins so wide tables and ASCII art fit
    for section in doc.sections:
        section.left_margin = Inches(0.5)
        section.right_margin = Inches(0.5)
        section.top_margin = Inches(0.6)
        section.bottom_margin = Inches(0.6)

    # Default style: Calibri 10pt
    style = doc.styles['Normal']
    style.font.name = 'Calibri'
    style.font.size = Pt(10)

    # Document title
    title_p = doc.add_heading(title, level=0)
    title_p.alignment = WD_ALIGN_PARAGRAPH.CENTER

    for kind, payload in blocks:
        if kind == 'heading':
            level, text_h = payload
            # docx headings 1..9 (level 0 reserved for title)
            doc.add_heading(text_h, level=min(level, 5))

        elif kind == 'paragraph':
            p = doc.add_paragraph()
            apply_inline_formatting(p, payload)

        elif kind == 'code':
            # Monospace block, single paragraph keeping line breaks
            p = doc.add_paragraph()
            p.paragraph_format.left_indent = Inches(0.1)
            p.paragraph_format.space_before = Pt(4)
            p.paragraph_format.space_after = Pt(4)
            run = p.add_run(payload)
            run.font.name = 'Consolas'
            run.font.size = Pt(8)
            run.font.color.rgb = RGBColor(0x10, 0x10, 0x10)

        elif kind == 'table':
            rows = payload
            if not rows:
                continue
            n_cols = max(len(r) for r in rows)
            table = doc.add_table(rows=len(rows), cols=n_cols)
            table.style = 'Light Grid Accent 1'
            for r_idx, row in enumerate(rows):
                for c_idx in range(n_cols):
                    cell = table.rows[r_idx].cells[c_idx]
                    cell.text = ''
                    p = cell.paragraphs[0]
                    if c_idx < len(row):
                        apply_inline_formatting(p, row[c_idx])
                    # Header row bold
                    if r_idx == 0:
                        for run in p.runs:
                            run.bold = True
                            run.font.size = Pt(9)
                    else:
                        for run in p.runs:
                            run.font.size = Pt(9)

        elif kind == 'list':
            for raw in payload:
                bullet = re.match(r'^\s*[-*]\s+(.*)$', raw)
                numbered = re.match(r'^\s*(\d+)\.\s+(.*)$', raw)
                if bullet:
                    p = doc.add_paragraph(style='List Bullet')
                    apply_inline_formatting(p, bullet.group(1))
                elif numbered:
                    p = doc.add_paragraph(style='List Number')
                    apply_inline_formatting(p, numbered.group(2))
                else:
                    p = doc.add_paragraph()
                    apply_inline_formatting(p, raw.strip())

        elif kind == 'hr':
            # Insert a separator (empty paragraph with bottom border isn't worth
            # the XML hacking; just add a blank line).
            doc.add_paragraph()

    doc.save(out_path)


def main():
    if len(sys.argv) != 3:
        print("usage: md_to_docx.py <input.md> <output.docx>")
        sys.exit(1)

    in_path = Path(sys.argv[1])
    out_path = Path(sys.argv[2])
    text = in_path.read_text(encoding='utf-8')
    blocks = parse_markdown_to_blocks(text)

    # Use first H1 as document title, or fall back to filename stem
    title = in_path.stem.replace('_', ' ')
    for kind, payload in blocks:
        if kind == 'heading' and payload[0] == 1:
            title = payload[1]
            break

    render_docx(blocks, out_path, title)
    print(f"wrote {out_path} ({out_path.stat().st_size} bytes, {len(blocks)} blocks)")


if __name__ == '__main__':
    main()
