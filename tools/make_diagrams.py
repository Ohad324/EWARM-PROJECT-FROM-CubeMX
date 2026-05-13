"""Generate the two block diagrams for the STM32H7 blog as red-themed JPGs.

Outputs:
  docs/images/stm32h7-domains.jpg       — Section 2 power-domain diagram
  docs/images/stm32h7-sw-pipeline.jpg   — Section 5 SW component pipeline

Each image contains:
  - the diagram itself, drawn with Pillow (red boxes + dark text)
  - a descriptive caption rendered below the diagram

Run from project root:
  python tools/make_diagrams.py
"""

from __future__ import annotations
import os
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

# ── Palette ───────────────────────────────────────────────────────────────────
RED = (196, 50, 50)            # red theme: box border + header bar
RED_LIGHT = (255, 232, 232)    # red theme: box fill tint
BLUE = (35, 87, 175)           # blue theme: box border + header bar
BLUE_LIGHT = (228, 237, 252)   # blue theme: box fill tint
GREEN = (40, 130, 70)          # green theme: box border + header bar
GREEN_LIGHT = (228, 245, 232)  # green theme: box fill tint
PURPLE = (130, 90, 175)        # light purple theme: box border + header bar
PURPLE_LIGHT = (240, 232, 250) # light purple theme: box fill tint
BLACK = (24, 24, 24)
GRAY = (90, 90, 90)
WHITE = (255, 255, 255)


def palette(theme: str) -> dict:
    """Return the colour palette for a theme name ('red', 'blue', 'green', or 'purple')."""
    if theme == "blue":
        return {"primary": BLUE, "light": BLUE_LIGHT}
    if theme == "green":
        return {"primary": GREEN, "light": GREEN_LIGHT}
    if theme == "purple":
        return {"primary": PURPLE, "light": PURPLE_LIGHT}
    return {"primary": RED, "light": RED_LIGHT}

OUT_DIR = Path(__file__).resolve().parents[1] / "docs" / "images"
OUT_DIR.mkdir(parents=True, exist_ok=True)


def load_font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    """Load a sans-serif font from Windows or fall back to PIL default."""
    candidates = []
    if bold:
        candidates += [
            r"C:\Windows\Fonts\segoeuib.ttf",
            r"C:\Windows\Fonts\arialbd.ttf",
            r"C:\Windows\Fonts\calibrib.ttf",
        ]
    else:
        candidates += [
            r"C:\Windows\Fonts\segoeui.ttf",
            r"C:\Windows\Fonts\arial.ttf",
            r"C:\Windows\Fonts\calibri.ttf",
        ]
    for path in candidates:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def load_mono(size: int) -> ImageFont.FreeTypeFont:
    for path in (r"C:\Windows\Fonts\consola.ttf", r"C:\Windows\Fonts\cour.ttf"):
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def text_size(draw: ImageDraw.ImageDraw, text: str, font) -> tuple[int, int]:
    bbox = draw.textbbox((0, 0), text, font=font)
    return bbox[2] - bbox[0], bbox[3] - bbox[1]


def wrap_text(draw: ImageDraw.ImageDraw, text: str, font, max_w: int) -> list[str]:
    """Simple word-wrap helper for caption blocks."""
    words = text.split()
    lines, cur = [], ""
    for w in words:
        candidate = (cur + " " + w).strip()
        if text_size(draw, candidate, font)[0] <= max_w:
            cur = candidate
        else:
            if cur:
                lines.append(cur)
            cur = w
    if cur:
        lines.append(cur)
    return lines


def draw_block(
    draw: ImageDraw.ImageDraw,
    x: int, y: int, w: int, h: int,
    title: str,
    lines: list[str],
    header_font, body_font,
    primary=RED, light=RED_LIGHT,
) -> None:
    """Draw a coloured-border block with a coloured header bar and body text."""
    draw.rectangle([x, y, x + w, y + h], fill=light, outline=primary, width=3)
    header_h = text_size(draw, title, header_font)[1] + 12
    draw.rectangle([x, y, x + w, y + header_h], fill=primary, outline=primary, width=3)
    tw, th = text_size(draw, title, header_font)
    draw.text((x + (w - tw) / 2, y + (header_h - th) / 2 - 2), title,
              fill=WHITE, font=header_font)
    body_y = y + header_h + 8
    for ln in lines:
        draw.text((x + 12, body_y), ln, fill=BLACK, font=body_font)
        body_y += text_size(draw, ln, body_font)[1] + 4


def draw_arrow(
    draw: ImageDraw.ImageDraw,
    x1: int, y1: int, x2: int, y2: int,
    label: str | None = None,
    label_font=None,
    primary=RED,
) -> None:
    """Draw a vertical coloured arrow from (x1,y1) to (x2,y2), optional side label."""
    draw.line([(x1, y1), (x2, y2)], fill=primary, width=3)
    head = 8
    draw.polygon(
        [(x2 - head, y2 - head), (x2 + head, y2 - head), (x2, y2 + head // 2)],
        fill=primary,
    )
    if label and label_font is not None:
        tw, th = text_size(draw, label, label_font)
        draw.text((x1 + 12, (y1 + y2 - th) / 2), label, fill=GRAY, font=label_font)


def render_caption(img: Image.Image, caption: str, *, top_y: int, width: int,
                   font) -> int:
    """Render a multi-line caption centered under the diagram. Returns y past it."""
    draw = ImageDraw.Draw(img)
    max_w = width - 80
    lines = []
    for paragraph in caption.split("\n"):
        if paragraph.strip() == "":
            lines.append("")
        else:
            lines.extend(wrap_text(draw, paragraph.strip(), font, max_w))
    y = top_y
    line_h = text_size(draw, "Mg", font)[1] + 4
    for ln in lines:
        if ln:
            tw, _ = text_size(draw, ln, font)
            draw.text(((width - tw) / 2, y), ln, fill=BLACK, font=font)
        y += line_h
    return y


# ── Diagram 1: STM32H7 Power Domains ──────────────────────────────────────────

def make_domain_diagram(theme: str = "red") -> Path:
    """Render the D1/D2/D3 domain diagram. `theme` is 'red' or 'blue'."""
    pal = palette(theme)
    primary = pal["primary"]
    light = pal["light"]

    W, H = 1500, 1500
    img = Image.new("RGB", (W, H), WHITE)
    draw = ImageDraw.Draw(img)

    title_font = load_font(54, bold=True)   # bigger headline
    head_font = load_font(26, bold=True)
    body_font = load_font(20)
    caption_font = load_font(20)
    bridge_font = load_font(18)

    # Title — bigger, top of the page
    title = "STM32H7 Memory Architecture per RM0399 — Three Power Domains"
    tw, th = text_size(draw, title, title_font)
    draw.text(((W - tw) / 2, 35), title, fill=primary, font=title_font)

    # Three domain blocks, stacked vertically — narrower than the canvas
    # so they appear visually "centred" with breathing room on each side.
    box_w = 1200
    box_x = (W - box_w) / 2

    d1_lines = [
        "Resident CPU:   Cortex-M7 @ up to 480 MHz   (main application core)",
        "DMA:    MDMA (cross-domain),  DMA2D (graphics, D1-only)",
        "",
        "Bus segments in D1   (each peripheral is hard-wired to ONE bus):",
        "    AXI bus              M7's main load/store path; AXI SRAM lives here",
        "    AHB3   @ 200 MHz     MDMA, DMA2D, JPEG codec, FMC, QUADSPI, SDMMC1, LTDC",
        "",
        "RAM:",
        "    Flash      0x08000000   1 MB     code + read-only data",
        "    ITCM       0x00000000   64 KB    M7 instruction-side, no DMA reach",
        "    DTCM       0x20000000   128 KB   M7 data-side, no DMA reach",
        "    AXI SRAM   0x24000000   512 KB   reachable by M7, DMA1/DMA2, MDMA",
    ]
    d2_lines = [
        "Resident CPU:   Cortex-M4 @ up to 240 MHz   (companion core; unused on this project)",
        "M7 access:      Cortex-M7 reaches every D2 peripheral + SRAM via the bus matrix",
        "",
        "Bus segments in D2   (each peripheral is hard-wired to ONE bus):",
        "    AHB1   @ 200 MHz     DMA1, DMA2, DMAMUX1, ADC1/2, Ethernet MAC, USB OTG_HS",
        "    AHB2   @ 200 MHz     DCMI, CRYP, HASH, RNG, SDMMC2",
        "    APB1   @ 100 MHz     UART4-8, USART2/3, SPI2/3, I2C1-3, TIM2-7/12-14, DAC, FDCAN",
        "    APB2   @ 100 MHz     TIM1/8/15-17, USART1/6, SPI1/4/5, SAI1-3, DFSDM1, HRTIM",
        "",
        "RAM:",
        "    SRAM1   0x30000000   128 KB   DMA1/DMA2 reachable",
        "    SRAM2   0x30020000   128 KB   DMA1/DMA2 reachable",
        "    SRAM3   0x30040000   32 KB    DMA1/DMA2 reachable",
    ]
    d3_lines = [
        "Resident CPU:   none   (low-power domain — kept alive while D1+D2 sleep)",
        "M7 access:      Cortex-M7 reaches every D3 peripheral + SRAM via the bus matrix",
        "DMA:    BDMA  (D3-local — can ONLY reach D3 SRAM4)",
        "",
        "Bus segments in D3   (each peripheral is hard-wired to ONE bus):",
        "    AHB4   @ 200 MHz     GPIOA-K, CRC, BDMA, ADC3, HSEM",
        "    APB4   @ 100 MHz     LPUART1, SPI6, I2C4, LPTIM2-5, RTC, SAI4, COMP1/2, SYSCFG",
        "",
        "RAM:",
        "    SRAM4    0x38000000   64 KB    BDMA + M7 access",
        "    Backup   0x38800000   4 KB     battery-backed across power loss",
    ]

    y = 130   # push the first box below the (now bigger) title
    blocks = [
        ("D1 — CORE DOMAIN   (always-on while M7 runs)", d1_lines, 400),
        ("D2 — PERIPHERAL DOMAIN", d2_lines, 460),
        ("D3 — LOW-POWER DOMAIN   (stays alive when D1+D2 stop)", d3_lines, 380),
    ]

    block_centers = []
    for title_b, body, h in blocks:
        draw_block(draw, int(box_x), y, box_w, h, title_b, body, head_font, body_font,
                   primary=primary, light=light)
        block_centers.append((int(box_x + box_w / 2), y, y + h))
        y += h + 55  # tight gap; bus segments are now shown inside each box

    # Arrows + bridge labels between blocks
    bridge_labels = [
        "AHB bus matrix crossbar   (AHB3 in D1  ↔  AHB1/AHB2 + APB1/APB2 in D2)",
        "AHB bus matrix crossbar   (AHB1/AHB2 in D2  ↔  AHB4 + APB4 in D3)",
    ]
    for i in range(2):
        cx, top_block_bottom = block_centers[i][0], block_centers[i][2]
        next_block_top = block_centers[i + 1][1]
        arrow_y1 = top_block_bottom + 14
        arrow_y2 = next_block_top - 14
        draw_arrow(draw, cx, arrow_y1, cx, arrow_y2, primary=primary)
        label = bridge_labels[i]
        tw, th = text_size(draw, label, bridge_font)
        draw.text(((W - tw) / 2, (arrow_y1 + arrow_y2 - th) / 2 - 2),
                  label, fill=primary, font=bridge_font)

    # Caption — Option 1 clarification
    caption_y = y + 15
    draw.line([(80, caption_y), (W - 80, caption_y)], fill=primary, width=2)
    caption_y += 15
    caption = (
        "Figure 1 — STM32H747 memory architecture as published by ST in RM0399. "
        "Not every region shown is used by every project; see Section 6 for our project's actual occupancy."
    )
    render_caption(img, caption, top_y=caption_y, width=W, font=caption_font)

    # Filenames carry the theme so red and blue versions coexist on disk.
    suffix = f"-{theme}" if theme != "red" else "-red"
    png_path = OUT_DIR / f"three-domain-block-diagram{suffix}.png"
    jpg_path = OUT_DIR / f"three-domain-block-diagram{suffix}.jpg"
    img.save(png_path, "PNG", optimize=True)
    img.save(jpg_path, "JPEG", quality=95, subsampling=0)
    return png_path


# ── Diagram 2: SW Component Block Diagram (Audio + UART pipeline) ─────────────

def make_sw_pipeline_diagram() -> Path:
    W, H = 1700, 1750
    img = Image.new("RGB", (W, H), WHITE)
    draw = ImageDraw.Draw(img)

    title_font = load_font(34, bold=True)
    branch_font = load_font(22, bold=True)
    head_font = load_font(18, bold=True)
    body_font = load_font(15)
    caption_font = load_font(20)

    # Title
    title = "STM32H7 Voice-Command Music Player — Software Component Block Diagram"
    tw, th = text_size(draw, title, title_font)
    draw.text(((W - tw) / 2, 30), title, fill=RED, font=title_font)

    # Two parallel branch labels
    left_x = 80
    right_x = 900
    branch_w = 720
    branch_y = 95

    draw.text((left_x + 200, branch_y), "Audio capture path (D2)",
              fill=RED, font=branch_font)
    draw.text((right_x + 200, branch_y), "UART RX path (D1 AXI)",
              fill=RED, font=branch_font)

    box_h = 90
    gap = 38

    # Left branch boxes (audio)
    left_boxes = [
        ("MP34DT05-A PDM mic", ["CLK on PC2 (2 MHz)", "Data on PC1"]),
        ("DFSDM1 (D2 peripheral)", ["Sinc3 filter, OSR = 125", "Output: 16 kHz PCM"]),
        ("s_DfsdmBuf  @ D2 SRAM1", ["0x30004000, 512 B", "MPU Non-Cacheable / Shareable"]),
        ("StoreDmaChunk  (CPU)", ["Runs in DMA HT/TC ISR", "Shift +pack 32-bit -> int16"]),
        ("g_AudioBuf  @ D2 SRAM2", ["0x30020000, 96 KB", "48000 x int16 = 3 s of audio"]),
        ("VoiceRecTask", ["FreeRTOS priority 32", "Signals SDWriteTask"]),
    ]
    # Right branch boxes (UART RX)
    right_boxes = [
        ("u-blox NORA-W106-00B Wi-Fi", ["UART8 @ 921 600 baud", "(opaque endpoint)"]),
        ("UART8 + DMA1 Stream 0", ["DMA-IDLE pattern", "D2 peripheral, AXI target"]),
        ("s_dma_rx_buf  @ D1 AXI", ["Cacheable region", "ISR invalidates cache before read"]),
        ("HAL_UARTEx_RxEventCallback", ["Copies bytes -> BleRawMsg_t", "xQueueSendFromISR"]),
        ("xRawBleQueue  @ D1 AXI", ["StaticQueue_t + 4-slot storage", "Hard Rule #1 (static alloc)"]),
        ("UARTReceiveTask", ["FreeRTOS priority 26", "Routes lines; sets s_audioReady"]),
    ]

    y = branch_y + 50
    left_centers = []
    right_centers = []
    for (lt, ll), (rt, rl) in zip(left_boxes, right_boxes):
        draw_block(draw, left_x, y, branch_w, box_h, lt, ll, head_font, body_font)
        draw_block(draw, right_x, y, branch_w, box_h, rt, rl, head_font, body_font)
        left_centers.append((left_x + branch_w // 2, y + box_h))
        right_centers.append((right_x + branch_w // 2, y + box_h))
        y += box_h + gap

    # Vertical arrows between consecutive boxes
    for i in range(len(left_centers) - 1):
        lx, ly_bot = left_centers[i]
        ly_top = ly_bot + gap
        draw_arrow(draw, lx, ly_bot + 4, lx, ly_top - 8)
        rx, ry_bot = right_centers[i]
        ry_top = ry_bot + gap
        draw_arrow(draw, rx, ry_bot + 4, rx, ry_top - 8)

    # SDWriteTask box spans both columns
    spanning_y = y + 8
    spanning_w = branch_w * 2 + (right_x - left_x - branch_w)
    span_h = 140
    sd_lines = [
        "1. Write WAV header + PCM to SD card (SDMMC1 IDMA, bounce via AXI)",
        "2. Send  AUDIO:FILE <name> <size>  over UART8 TX",
        "3. Wait for AUDIO:READY from the NORA Wi-Fi companion",
        "4. Stream the 96 KB PCM payload over UART8 TX",
    ]
    draw_block(draw, left_x, spanning_y, spanning_w, span_h,
               "SDWriteTask   (FreeRTOS priority 20, stack 2048 W in AXI)",
               sd_lines, head_font, body_font)

    # Arrows from VoiceRecTask + UARTReceiveTask (notify) into SDWriteTask
    last_left = left_centers[-1]
    last_right = right_centers[-1]
    draw_arrow(draw, last_left[0], last_left[1] + 4, last_left[0], spanning_y - 4)
    draw_arrow(draw, last_right[0], last_right[1] + 4, last_right[0], spanning_y - 4)

    # Parallel-tasks row at the bottom
    parallel_y = spanning_y + span_h + 50
    parallel_w = 780
    pg_x_left = (W - 2 * parallel_w - 60) // 2
    pg_x_right = pg_x_left + parallel_w + 60

    draw_block(
        draw, pg_x_left, parallel_y, parallel_w, 130,
        "TouchGFXTask (LCD)",
        [
            "FreeRTOS priority High (~24)",
            "Partial framebuffer: 4 strips x 120 rows",
            "Framebuffer at 0x24000000 (D1 AXI)",
            "DMA2D blit + LTDC scan to DSI panel",
        ],
        head_font, body_font,
    )
    draw_block(
        draw, pg_x_right, parallel_y, parallel_w, 130,
        "SEGGER RTT diagnostic channel",
        [
            "Up-buffer + down-buffer + control block",
            "Pinned at 0x38000000 (D3 SRAM4)",
            "J-Link probe reads over SWD",
            "Viewable in RTT Viewer / RTT Logger",
        ],
        head_font, body_font,
    )

    # Caption
    caption_y = parallel_y + 130 + 50
    draw.line([(80, caption_y), (W - 80, caption_y)], fill=RED, width=2)
    caption_y += 20
    caption = (
        "Figure 2 — Software component block diagram for the Music Player by Voice Command. "
        "Two pipelines run in parallel on the STM32H747's Cortex-M7: the audio capture path on the left (mic -> DFSDM -> "
        "DMA1 -> D2 SRAM -> VoiceRecTask) and the UART RX path on the right (UART8 wire -> DMA1 -> AXI buffer -> ISR -> static "
        "queue -> UARTReceiveTask). Both branches converge into SDWriteTask, which persists the WAV file to SD card and then "
        "streams the PCM payload back over UART8 TX to the u-blox NORA-W106-00B Wi-Fi companion for cloud upload. "
        "TouchGFXTask renders the LCD UI from the framebuffer in D1 AXI on a partial-framebuffer schedule, completely "
        "isolated from the audio path in D2. SEGGER RTT provides zero-overhead real-time logging via a buffer pinned in D3 "
        "SRAM4, leaving AXI free for the framebuffer and the application's working set."
    )
    render_caption(img, caption, top_y=caption_y, width=W, font=caption_font)

    png_path = OUT_DIR / "software-pipeline-block-diagram.png"
    jpg_path = OUT_DIR / "software-pipeline-block-diagram.jpg"
    img.save(png_path, "PNG", optimize=True)
    img.save(jpg_path, "JPEG", quality=95, subsampling=0)
    return png_path


# ── Generic styled-table renderer (used for Table 3.10 audio peripherals) ─────

def render_styled_table(
    *,
    title: str,
    subtitle: str,
    headers: list[str],
    rows: list[list[str]],
    col_widths: list[int],
    theme: str,
    output_basename: str,
    caption: str,
) -> Path:
    """Render a table with a coloured header bar and alternating row stripes.

    col_widths is in pixels; total table width = sum(col_widths).
    """
    pal = palette(theme)
    primary = pal["primary"]
    light = pal["light"]

    title_font = load_font(40, bold=True)
    subtitle_font = load_font(22, bold=False)
    header_font = load_font(18, bold=True)
    cell_font = load_font(16)
    caption_font = load_font(17)

    margin = 60
    table_w = sum(col_widths)
    W = table_w + 2 * margin
    # Heights: we measure each row by wrapping cell text per col_width
    img_tmp = Image.new("RGB", (10, 10), WHITE)
    draw_tmp = ImageDraw.Draw(img_tmp)

    def measure_row(cells, font, padding=12):
        max_lines = 1
        for cell, cw in zip(cells, col_widths):
            wrapped = wrap_text(draw_tmp, cell, font, cw - 2 * padding)
            if len(wrapped) > max_lines:
                max_lines = len(wrapped)
        line_h = text_size(draw_tmp, "Mg", font)[1] + 5
        return max_lines * line_h + 2 * padding, max_lines, line_h

    header_h, _, _ = measure_row(headers, header_font)
    body_row_heights = [measure_row(r, cell_font)[0] for r in rows]
    total_table_h = header_h + sum(body_row_heights)

    # Caption height estimate
    caption_lines = wrap_text(draw_tmp, caption, caption_font, W - 2 * margin)
    caption_h = len(caption_lines) * (text_size(draw_tmp, "Mg", caption_font)[1] + 5) + 30

    # Empty title/subtitle => skip that block entirely (no reserved space).
    title_h = text_size(draw_tmp, title, title_font)[1] if title else 0
    subtitle_h = text_size(draw_tmp, subtitle, subtitle_font)[1] if subtitle else 0
    top_padding = 40 if (title or subtitle) else 20
    gap_t_s = 15 if (title and subtitle) else 0
    bottom_padding = 35 if (title or subtitle) else 15
    top_h = top_padding + title_h + gap_t_s + subtitle_h + bottom_padding

    H = top_h + total_table_h + 40 + caption_h + 40
    img = Image.new("RGB", (W, H), WHITE)
    draw = ImageDraw.Draw(img)

    # Title + subtitle (drawn only when non-empty)
    if title:
        tw, _ = text_size(draw, title, title_font)
        draw.text(((W - tw) / 2, top_padding), title, fill=primary, font=title_font)
    if subtitle:
        sw, _ = text_size(draw, subtitle, subtitle_font)
        draw.text(((W - sw) / 2, top_padding + title_h + gap_t_s), subtitle, fill=BLACK, font=subtitle_font)

    # Header row
    y = top_h
    x = margin
    draw.rectangle([margin, y, margin + table_w, y + header_h], fill=primary, outline=primary, width=2)
    for cell, cw in zip(headers, col_widths):
        wrapped = wrap_text(draw, cell, header_font, cw - 24)
        line_h = text_size(draw, "Mg", header_font)[1] + 5
        text_y = y + (header_h - line_h * len(wrapped)) / 2
        for line in wrapped:
            lw, _ = text_size(draw, line, header_font)
            draw.text((x + (cw - lw) / 2, text_y), line, fill=WHITE, font=header_font)
            text_y += line_h
        x += cw

    # Body rows with alternating stripes
    y += header_h
    for r_idx, (row, row_h) in enumerate(zip(rows, body_row_heights)):
        fill = light if r_idx % 2 == 0 else WHITE
        draw.rectangle([margin, y, margin + table_w, y + row_h], fill=fill, outline=primary, width=1)
        x = margin
        for cell, cw in zip(row, col_widths):
            wrapped = wrap_text(draw, cell, cell_font, cw - 24)
            line_h = text_size(draw, "Mg", cell_font)[1] + 5
            text_y = y + (row_h - line_h * len(wrapped)) / 2
            for line in wrapped:
                draw.text((x + 12, text_y), line, fill=BLACK, font=cell_font)
                text_y += line_h
            x += cw
        y += row_h

    # Caption (Table N — ...)
    y += 25
    draw.line([(margin, y), (W - margin, y)], fill=primary, width=2)
    y += 12
    cap_line_h = text_size(draw, "Mg", caption_font)[1] + 5
    for line in caption_lines:
        lw, _ = text_size(draw, line, caption_font)
        draw.text(((W - lw) / 2, y), line, fill=BLACK, font=caption_font)
        y += cap_line_h

    png_path = OUT_DIR / f"{output_basename}.png"
    jpg_path = OUT_DIR / f"{output_basename}.jpg"
    img.save(png_path, "PNG", optimize=True)
    img.save(jpg_path, "JPEG", quality=95, subsampling=0)
    return png_path


def make_audio_peripherals_table() -> Path:
    """Render Table 3.10 (audio peripherals) as a green-themed image."""
    headers = [
        "Audio peripheral",
        "Base address",
        "Bus",
        "Domain",
        "What it does",
        "Used in this project?",
    ]
    rows = [
        ["DFSDM1", "0x4001_7000", "APB2", "D2",
         "Digital Filter for Sigma-Delta Modulators — Sinc1/2/3 + decimation. Ideal for PDM mics.",
         "PDM mic capture → Sinc3 OSR=125 → 16 kHz PCM, DMA1 → D2 SRAM1"],
        ["SAI1, SAI2, SAI3", "0x4001_5800+", "APB2", "D2",
         "Serial Audio Interface — I2S, TDM, AC97 and PDM modes; pairs with external codec / DAC.",
         "Unused on this build; would drive an I2S codec for audio playback"],
        ["SAI4", "0x5801_5400", "APB4", "D3",
         "Same SAI block but in the low-power domain — can run while D1+D2 are halted.",
         "Retired in favour of DFSDM-master clock path; D3 stays idle"],
        ["SPDIFRX", "0x4000_4000", "APB1", "D2",
         "Sony/Philips Digital Interface receiver — decodes S/PDIF biphase-mark digital audio.",
         "Unused"],
        ["DAC1 (channels 1+2)", "0x4000_7400", "APB1", "D2",
         "12-bit dual-channel analogue DAC — line-level audio out, control voltages, etc.",
         "Unused"],
        ["PDM2PCM (ST library)", "(software)", "—", "—",
         "ST-provided software library that converts raw PDM bitstreams to PCM when SAI is in PDM mode.",
         "Not used; DFSDM's hardware Sinc filter does this natively"],
    ]
    col_widths = [220, 170, 110, 110, 580, 540]
    return render_styled_table(
        title="STM32H7 Audio Peripherals — One-Page Reference",
        subtitle="Table 3.10 — Bus, domain, role, and project usage",
        headers=headers,
        rows=rows,
        col_widths=col_widths,
        theme="green",
        output_basename="table-3-10-audio-peripherals-subsystem",
        caption=(
            "Table 3.10 — All audio-oriented peripherals on the STM32H7. "
            "All audio peripherals live on APB segments (APB1, APB2, or APB4) — never on AHB — "
            "because their register interface doesn't need 200 MHz. The bandwidth-critical part is the DMA path."
        ),
    )


def make_power_domains_table_green() -> Path:
    """Render Table 2.1 (power domains side-by-side) as a green-themed image."""
    headers = ["Aspect", "D1 — Core Domain", "D2 — Peripheral Domain", "D3 — Low-Power Domain"]
    rows = [
        ["Always-on?",
         "Yes, while M7 runs",
         "Active when a D2 peripheral runs",
         "Can stay alive when D1 + D2 are halted"],
        ["Resident CPU",
         "Cortex-M7 @ up to 480 MHz",
         "Cortex-M4 @ up to 240 MHz (shut down on this project)",
         "None"],
        ["Active master on this project",
         "Cortex-M7 — main application core",
         "Cortex-M7 — reaches every D2 peripheral + SRAM via the bus matrix (M4 held in stop-mode stub)",
         "Cortex-M7 — reaches every D3 peripheral + SRAM via the bus matrix"],
        ["DMA controllers",
         "MDMA (cross-domain), DMA2D (graphics, D1-only)",
         "DMA1 (audio path), DMA2 (SDMMC bounce, generic)",
         "BDMA (D3-local only)"],
        ["Key peripherals",
         "JPEG codec, FMC, QUADSPI, LTDC",
         "DFSDM1, SDMMC1/2, SAI1-3, SPI, I2C, USART, USB, UART8",
         "SAI4, LPTIM, RTC, LPUART, ADC3, DAC, I2C4, SPI6"],
        ["Flash / ROM",
         "0x0800_0000, 1 MB, code + read-only data",
         "—",
         "—"],
        ["ITCM",
         "0x0000_0000, 64 KB, M7 instruction-side, no DMA reach",
         "—",
         "—"],
        ["DTCM",
         "0x2000_0000, 128 KB, M7 data-side, no DMA reach",
         "—",
         "—"],
        ["AXI SRAM",
         "0x2400_0000, 512 KB, DMA1/DMA2/MDMA accessible",
         "—",
         "—"],
        ["D2 SRAM1",
         "—",
         "0x3000_0000, 128 KB, DMA1/DMA2 reachable",
         "—"],
        ["D2 SRAM2",
         "—",
         "0x3002_0000, 128 KB, DMA1/DMA2 reachable",
         "—"],
        ["D2 SRAM3",
         "—",
         "0x3004_0000, 32 KB, DMA1/DMA2 reachable",
         "—"],
        ["D3 SRAM4",
         "—",
         "—",
         "0x3800_0000, 64 KB, BDMA-only"],
        ["Backup SRAM",
         "—",
         "—",
         "0x3880_0000, 4 KB, battery-backed across power loss"],
        ["Connects via",
         "AXI bus + AHB bus matrix crossbar to D2 and D3",
         "AHB bus matrix crossbar to D1 and to D2 SRAMs",
         "Silicon bridges where they exist; otherwise reachable via the bus matrix"],
    ]
    # Aspect column slim; the three domain columns equal-width
    col_widths = [240, 530, 530, 530]
    return render_styled_table(
        title="STM32H7 Power Domains — Side by Side",
        subtitle="Table 2.1 — Aspect-by-aspect comparison of D1, D2, and D3",
        headers=headers,
        rows=rows,
        col_widths=col_widths,
        theme="green",
        output_basename="table-2-1-power-domains-side-by-side",
        caption=(
            "Table 2.1 — STM32H7 power domains compared aspect by aspect. "
            "Every SRAM region is shown in the column of its owning domain; an em-dash means "
            "that aspect isn't present in that domain. Connections between domains go through the "
            "AHB bus matrix crossbar described in Section 4."
        ),
    )


def make_peripheral_memory_map_table_green() -> Path:
    """Render Table 3 (peripheral memory map by bus + domain) as a green-themed image."""
    headers = ["#", "Address range", "Bus", "Domain", "Key peripherals living in this segment"]
    rows = [
        ["3.1", "0x4000_0000 – 0x4000_FFFF", "APB1", "D2",
         "TIM2-7, TIM12-14, LPTIM1, SPI2/3, SPDIFRX, USART2/3, UART4-8, I2C1-3, DAC1, FDCAN"],
        ["3.2", "0x4001_0000 – 0x4001_FFFF", "APB2", "D2",
         "TIM1, TIM8, TIM15-17, USART1/6, SPI1/4/5, SAI1/2/3, DFSDM1, HRTIM"],
        ["3.3", "0x4002_0000 – 0x4007_FFFF", "AHB1", "D2",
         "DMA1, DMA2, DMAMUX1, ADC1/2, Ethernet MAC, USB1 OTG_HS"],
        ["3.4", "0x4800_0000 – 0x4802_FFFF", "AHB2", "D2",
         "DCMI, CRYP, HASH, RNG, SDMMC2"],
        ["3.5", "0x5000_0000 – 0x53FF_FFFF", "AHB3", "D1",
         "LTDC, MDMA, DMA2D, JPEG, FMC, QUADSPI, SDMMC1, FLASH interface"],
        ["3.6", "0x5800_0000 – 0x5800_3FFF", "APB4", "D3",
         "SYSCFG, LPUART1, SPI6, I2C4, LPTIM2/3/4/5, COMP1/2, VREFBUF, RTC, SAI4"],
        ["3.7", "0x5802_0000 – 0x5806_FFFF", "AHB4", "D3",
         "GPIOA-K, CRC, BDMA, ADC3, HSEM, RCC, PWR"],
        ["3.8", "0xE000_0000 – 0xE00F_FFFF", "Private", "M7 core",
         "NVIC, SCB, SysTick, MPU, DWT, ITM, FPB, ETM, DAP"],
    ]
    col_widths = [80, 360, 130, 130, 900]
    return render_styled_table(
        title="STM32H7 Peripheral Memory Map — Bus + Domain",
        subtitle="Table 3 — Every peripheral lives on exactly one bus segment, decided at silicon design time",
        headers=headers,
        rows=rows,
        col_widths=col_widths,
        theme="green",
        output_basename="table-3-peripheral-memory-map",
        caption=(
            "Table 3 — STM32H7 peripherals grouped by their fixed bus segment and power domain. "
            "Sections 3.1–3.8 of the blog walk each row in detail. Addresses are from RM0399 Chapter 2."
        ),
    )


def make_audio_pipeline_table_blue() -> Path:
    """Render Table 5.1 (audio capture pipeline, left branch) as a blue-themed image."""
    headers = ["#", "Stage", "Hardware / Software", "Address / domain", "Notes"]
    rows = [
        ["5.1.1", "PDM bitstream source", "MP34DT05-A PDM microphone",
         "PC2 (CLK out, 2 MHz) + PC1 (data in)",
         "On-board microphone on the STM32H747I-DISCO"],
        ["5.1.2", "PDM-to-PCM conversion", "DFSDM1 (peripheral)",
         "D2 (0x4001_7000)",
         "Sinc3 filter, OSR = 125 → 16 kHz, 16-bit PCM"],
        ["5.1.3", "DMA path", "DMA1 Stream 1",
         "D2 master",
         "Half-complete / complete interrupt drives StoreDmaChunk"],
        ["5.1.4", "DFSDM DMA ring buffer", "s_DfsdmBuf",
         "D2 SRAM1 (0x3000_4000, 512 B)",
         "MPU Non-Cacheable Shareable; bypasses M7 cache"],
        ["5.1.5", "Sample post-processing", "StoreDmaChunk (CPU)",
         "Runs in DMA HT/TC ISR context",
         "Shifts 32-bit DFSDM word right by 8, packs into int16"],
        ["5.1.6", "PCM accumulator", "g_AudioBuf",
         "D2 SRAM2 (0x3002_0000, 96 KB)",
         "48 000 × int16 = 3 seconds at 16 kHz"],
        ["5.1.7", "Recording orchestrator", "VoiceRecTask",
         "Stack in AXI, priority 32",
         "Highest task priority; signals SDWriteTask when 3-sec window completes"],
    ]
    col_widths = [90, 280, 320, 360, 580]
    return render_styled_table(
        title="Audio Capture Pipeline — Stage by Stage",
        subtitle="Table 5.1 — Left branch of the SW block diagram (PDM mic → PCM accumulator → VoiceRecTask)",
        headers=headers,
        rows=rows,
        col_widths=col_widths,
        theme="blue",
        output_basename="table-5-1-audio-capture-pipeline-stages",
        caption=(
            "Table 5.1 — The audio capture path runs entirely inside the D2 domain. "
            "DFSDM1 produces 16 kHz PCM, DMA1 deposits samples into D2 SRAM1, the CPU "
            "shifts and packs them into D2 SRAM2, and VoiceRecTask orchestrates the 3-second window."
        ),
    )


def make_uart_rx_pipeline_table_blue() -> Path:
    """Render Table 5.2 (UART RX pipeline, right branch) as a blue-themed image."""
    headers = ["#", "Stage", "Hardware / Software", "Address / domain", "Notes"]
    rows = [
        ["5.2.1", "UART link to companion", "UART8 @ 921 600 baud",
         "APB1 (0x4000_7C00), D2",
         "Talks to the u-blox NORA-W106-00B Wi-Fi companion (opaque endpoint for this post)"],
        ["5.2.2", "DMA reception", "DMA1 Stream 0, DMA-IDLE",
         "D2 master, writes into AXI",
         "DMA + IDLE-line pattern so messages of any length are demarcated by the line going idle"],
        ["5.2.3", "DMA RX buffer", "s_dma_rx_buf",
         "D1 AXI SRAM, cacheable",
         "ISR calls SCB_InvalidateDCache_by_Addr before reading"],
        ["5.2.4", "ISR → queue handoff", "HAL_UARTEx_RxEventCallback",
         "ISR context",
         "Copies bytes into BleRawMsg_t and xQueueSendFromISR posts to the queue"],
        ["5.2.5", "ISR-to-task message queue", "xRawBleQueue",
         "D1 AXI, StaticQueue_t + storage",
         "Statically allocated via #pragma location = \".axi_sram\" — Hard Rule #1"],
        ["5.2.6", "Receive task", "UARTReceiveTask",
         "Stack in AXI, priority 26",
         "Routes each ASCII line to the right handler"],
        ["5.2.7", "AUDIO:READY signal", "AudioSD_NotifyReady()",
         "CPU",
         "Sets s_audioReady = true, which unblocks SDWriteTask's wait loop"],
    ]
    col_widths = [90, 290, 320, 320, 600]
    return render_styled_table(
        title="UART RX Pipeline — Stage by Stage",
        subtitle="Table 5.2 — Right branch of the SW block diagram (UART wire → static queue → UARTReceiveTask)",
        headers=headers,
        rows=rows,
        col_widths=col_widths,
        theme="blue",
        output_basename="table-5-2-uart-rx-pipeline-stages",
        caption=(
            "Table 5.2 — The UART RX path lives entirely in D1 AXI on the producer + consumer side. "
            "DMA1 Stream 0 deposits incoming bytes; the ISR invalidates the cache and pushes them "
            "into a statically-allocated FreeRTOS queue; UARTReceiveTask routes each ASCII line."
        ),
    )


def make_collision_audit_table_blue() -> Path:
    """Render Table 7 (Collision audit) as a blue-themed image."""
    headers = ["#", "Check", "Buffers", "Domain", "Status"]
    rows = [
        ["7.1", "Audio buffers in their own domain",
         "s_DfsdmBuf (D2 SRAM1) + g_AudioBuf (D2 SRAM2)",
         "D2",
         "PASS  -  DMA1 writes SRAM1, CPU writes SRAM2; 124 KB gap between them"],
        ["7.2", "UART RX pipeline same domain",
         "s_dma_rx_buf, s_rawBleQueueCB, s_rawBleQueueStorage, s_bleHistMutexCB",
         "D1 AXI",
         "PASS  -  no cross-domain traffic per message"],
        ["7.3", "Framebuffer isolated from audio",
         "TouchGFX_Framebuffer (D1 AXI) vs audio (D2)",
         "mixed",
         "PASS  -  different domains - eliminates FUIF"],
        ["7.4", "Heap vs DMA buffer",
         "ucHeap (D1 AXI) vs s_DfsdmBuf (D2 SRAM1)",
         "mixed",
         "PASS  -  isolated by domain"],
        ["7.5", "Stack vs DMA buffers",
         "CSTACK (DTCM)",
         "D1 DTCM",
         "PASS  -  DMA cannot reach DTCM, safe by design"],
        ["7.6", "Free-space margins",
         "D1 AXI 97.5 % used, D2 SRAM1 99 % free, D3 100 % unused",
         "-",
         "WARN  -  AXI is tight; move next big buffer to D2"],
    ]
    col_widths = [90, 360, 540, 150, 540]
    return render_styled_table(
        title="",
        subtitle="",
        headers=headers,
        rows=rows,
        col_widths=col_widths,
        theme="blue",
        output_basename="table-7-collision-audit",
        caption=(
            "Table 7 — Each row is a quick check run after every significant build: "
            "is the buffer reachable by its DMA master, is it isolated from contenders, "
            "and is there enough free space in its domain to grow?"
        ),
    )


def make_hard_rules_table_blue() -> Path:
    """Render Table 8 (Hard Rules — buffer class placement) as a blue-themed image."""
    headers = ["#", "Buffer class", "Required region", "Why"]
    rows = [
        ["8.1", "LTDC framebuffer",
         "D1 AXI SRAM (0x24000000)",
         "LTDC scans on AHB; AXI removes contention with FMC/SDRAM"],
        ["8.2", "DFSDM DMA ring",
         "D2 SRAM1 (0x30000000+)",
         "DMA1 lives in D2; D2 SRAM avoids AXI contention with LTDC"],
        ["8.3", "PCM accumulator (g_AudioBuf in our project)",
         "D2 SRAM2 (0x30020000+)",
         "CPU-only writes, large (96 KB) — keeps D1 AXI free for graphics"],
        ["8.4", "SDMMC IDMA bounce",
         "D1 AXI SRAM",
         "SDMMC1 IDMA prefers AXI; bounce-buffer copy from D2 is the standard pattern"],
        ["8.5", "UART RX DMA buffer",
         "D1 AXI SRAM",
         "DMA1/2 reachable; same domain as ISR-driven queue means no cross-domain hop per message"],
        ["8.6", "FreeRTOS queues / mutexes / TCBs",
         "D1 AXI SRAM (default)",
         "CPU-only access; keep with task stacks. Static allocation, no xQueueCreate from heap."],
        ["8.7", "FreeRTOS heap (ucHeap)",
         "D1 AXI SRAM",
         "Default placement is correct. Never place adjacent to DMA ring buffers."],
        ["8.8", "CSTACK + ISR stack",
         "DTCM (0x20000000)",
         "Zero wait, no DMA can corrupt it"],
        ["8.9", "JPEG decode intermediates",
         "External SDRAM",
         "Multi-megabyte, latency-tolerant"],
        ["8.10", "TouchGFX assets",
         "QSPI Flash (XIP) / SDRAM",
         "Read-only LUTs at runtime"],
        ["8.11", "SAI4 BDMA buffers (if ever used)",
         "D3 SRAM4 (0x38000000)",
         "BDMA is D3-only; will fail silently anywhere else"],
    ]
    col_widths = [90, 460, 360, 760]
    return render_styled_table(
        title="Hard Rules — Where Each Buffer Class Belongs",
        subtitle="Table 8 — One-line placement rule per buffer class on STM32H7",
        headers=headers,
        rows=rows,
        col_widths=col_widths,
        theme="blue",
        output_basename="table-8-buffer-class-placement-rules",
        caption=(
            "Table 8 — Place each buffer in the same domain as the master that drives it most often. "
            "Cross-domain access works through the bus matrix but costs cycles and creates contention. "
            "These rules are derived from real-world placement bugs observed on this project."
        ),
    )


def make_cache_types_table_purple() -> Path:
    """Render the cache-types primer table as a light-purple-themed image."""
    headers = ["Cache level", "Typical size", "Typical latency", "Where it lives", "What it's for"]
    rows = [
        ["L1", "16-64 KB", "1-2 cycles",
         "Inside the CPU core, often split into I-cache + D-cache",
         "Hot working set: the instructions and data the CPU is touching right now. The first thing every load/store hits."],
        ["L2", "256 KB – 4 MB", "8-20 cycles",
         "On-chip, sometimes per-core, sometimes shared",
         "Catches L1 misses. Larger and unified (instructions and data share it). Common on application processors (Cortex-A, x86)."],
        ["L3", "4 MB – 64 MB", "30-60 cycles",
         "On-chip, shared across all cores",
         "Last-level cache before main RAM. Standard on server-class and desktop CPUs; absent from most embedded MCUs."],
        ["TLB", "32-1024 entries", "1 cycle",
         "Inside the MMU (if present)",
         "Translates virtual to physical addresses. Not present on Cortex-M which uses an MPU, not an MMU."],
        ["Branch / BTB", "64-4096 entries", "1 cycle",
         "Inside the CPU front-end",
         "Caches recently-taken branch targets for the branch predictor. Reduces pipeline flush cost on taken branches."],
    ]
    col_widths = [180, 220, 220, 440, 700]
    return render_styled_table(
        title="Caches — A Quick Primer",
        subtitle="Cache level / size / latency / location / role — what every CPU cache type does and where it lives",
        headers=headers,
        rows=rows,
        col_widths=col_widths,
        theme="purple",
        output_basename="cache-types-primer",
        caption=(
            "Cache hierarchy summary. The Cortex-M7 inside the STM32H747 has L1 only "
            "(16 KB I-cache + 16 KB D-cache); no L2, no L3, no MMU/TLB. Lower-end "
            "Cortex-M cores (M0, M3, M4) have no cache at all."
        ),
    )


if __name__ == "__main__":
    # Domain diagram in both red and blue themes
    p1_red = make_domain_diagram(theme="red")
    p1_blue = make_domain_diagram(theme="blue")
    # SW pipeline stays in red for now
    p2 = make_sw_pipeline_diagram()
    # Audio peripherals table in green
    p3 = make_audio_peripherals_table()
    # Power-domains side-by-side comparison in green
    p4 = make_power_domains_table_green()
    # Peripheral memory map (Section 3 master table) in green
    p5 = make_peripheral_memory_map_table_green()
    # Audio + UART pipeline tables in blue
    p6 = make_audio_pipeline_table_blue()
    p7 = make_uart_rx_pipeline_table_blue()
    # Collision audit in blue
    p8 = make_collision_audit_table_blue()
    # Hard rules in blue
    p9 = make_hard_rules_table_blue()
    # Cache primer in light purple
    p10 = make_cache_types_table_purple()
    print(f"wrote {p1_red}")
    print(f"wrote {p1_blue}")
    print(f"wrote {p2}")
    print(f"wrote {p3}")
    print(f"wrote {p4}")
    print(f"wrote {p5}")
    print(f"wrote {p6}")
    print(f"wrote {p7}")
    print(f"wrote {p8}")
    print(f"wrote {p9}")
    print(f"wrote {p10}")
