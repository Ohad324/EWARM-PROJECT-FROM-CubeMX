#  Axivion Suite
#  Copyright (C) 2021-2025 Axivion GmbH
#  Copyright (C) 2025 The Qt Company GmbH, a subsidiary of The Qt Group
#  https://www.qt.io


'''
Abstraction for generating PDF reports.
'''

import math
import os

from reportlab.graphics.charts.barcharts import HorizontalBarChart
from reportlab.graphics.charts.linecharts import HorizontalLineChart
from reportlab.graphics.shapes import Drawing, Rect
from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER, TA_LEFT, TA_RIGHT
from reportlab.lib.pagesizes import A4, landscape
from reportlab.lib.styles import ParagraphStyle, StyleSheet1
from reportlab.lib.units import cm, mm
from reportlab.platypus import (
    FrameBreak,
    NextPageTemplate,
    PageBreak,
    Paragraph,
    Spacer,
    Table,
    TableStyle,
)
from reportlab.platypus.doctemplate import BaseDocTemplate, PageTemplate
from reportlab.platypus.frames import Frame

AXIVION_WHITE = colors.Color(0.99, 0.99, 0.99)
AXIVION_BLACK = colors.Color(24.0 / 255, 23.0 / 255, 23.0 / 255)
AXIVION_DARKGREY = colors.Color(85.0 / 255, 86.0 / 255, 95.0 / 255)
AXIVION_LIGHTGREY = colors.Color(231.0 / 255, 230.0 / 255, 230.0 / 255)
AXIVION_RED = colors.Color(229.0 / 255, 59.0 / 255, 76.0 / 255)
AXIVION_RED_HEX = '#E53B4C'
AXIVION_BLUE = colors.Color(86.0 / 255, 161.0 / 255, 202.0 / 255)
AXIVION_DARK_GREEN = colors.Color(28.0 / 255, 149.0 / 255, 15.0 / 255)
AXIVION_DARK_GREEN_HEX = '#1C950F'

table_style = TableStyle(
    [
        (
            'BACKGROUND',
            (0, 0),
            (-1, 0),
            AXIVION_DARKGREY,
        ),
        ('GRID', (0, 0), (-1, -1), 1, AXIVION_BLACK),
        ('VALIGN', (0, 0), (-1, -1), 'TOP'),
        ('ROWBACKGROUNDS', (0, 1), (-1, -1), [AXIVION_LIGHTGREY, AXIVION_WHITE]),
        ('TEXTCOLOR', (0, 0), (-1, 0), AXIVION_WHITE),
    ]
)

font_family = 'Helvetica'


def getStyleSheet():
    """Returns a stylesheet object"""
    stylesheet = StyleSheet1()
    stylesheet.add(
        ParagraphStyle(
            name='Normal',
            fontName=font_family,
            fontSize=10,
            leading=12,
            spaceBefore=4,
            spaceAfter=4,
        )
    )

    stylesheet.add(
        ParagraphStyle(
            name='TableRight',
            fontName=font_family,
            fontSize=10,
            leading=12,
            spaceBefore=4,
            spaceAfter=4,
            alignment=TA_RIGHT,
        )
    )

    stylesheet.add(
        ParagraphStyle(
            name='TableLeft',
            fontName=font_family,
            fontSize=10,
            leading=12,
            spaceBefore=4,
            spaceAfter=4,
            alignment=TA_LEFT,
        )
    )

    stylesheet.add(
        ParagraphStyle(
            name='TableCenter',
            fontName=font_family,
            fontSize=10,
            leading=12,
            spaceBefore=4,
            spaceAfter=4,
            alignment=TA_CENTER,
        )
    )

    stylesheet.add(
        ParagraphStyle(name='BodyText', parent=stylesheet['Normal'], spaceBefore=6)
    )

    stylesheet.add(
        ParagraphStyle(name='Added', parent=stylesheet['Normal'], textColor=AXIVION_RED)
    )

    stylesheet.add(
        ParagraphStyle(
            name='Removed', parent=stylesheet['Normal'], textColor=AXIVION_DARK_GREEN
        )
    )

    stylesheet.add(
        ParagraphStyle(
            name='Italic',
            parent=stylesheet['BodyText'],
            fontName=font_family + '-Italic',
        )
    )

    stylesheet.add(
        ParagraphStyle(
            name='Heading1',
            parent=stylesheet['Normal'],
            fontName=font_family + '-Bold',
            fontSize=16,
            leading=16,
            spaceBefore=6,
            spaceAfter=0,
            keepWithNext=True,
        ),
        alias='h1',
    )

    stylesheet.add(
        ParagraphStyle(
            name='Heading2',
            parent=stylesheet['Normal'],
            fontName=font_family + '-Bold',
            fontSize=14,
            leading=14,
            spaceBefore=10,
            spaceAfter=6,
            keepWithNext=True,
        ),
        alias='h2',
    )

    stylesheet.add(
        ParagraphStyle(
            name='Heading3',
            parent=stylesheet['Normal'],
            fontName=font_family + '-Bold',
            fontSize=12,
            leading=16,
            spaceBefore=10,
            spaceAfter=6,
            keepWithNext=True,
        ),
        alias='h3',
    )

    stylesheet.add(
        ParagraphStyle(
            name='Title',
            parent=stylesheet['Normal'],
            fontName=font_family + '-Bold',
            fontSize=18,
            leading=24,
            spaceAfter=6,
            alignment=TA_CENTER,
            keepWithNext=True,
        ),
        alias='title',
    )

    stylesheet.add(
        ParagraphStyle(
            name='Subtitle',
            parent=stylesheet['Normal'],
            fontName=font_family + '-Bold',
            fontSize=16,
            leading=20,
            spaceAfter=6,
            alignment=TA_CENTER,
            keepWithNext=True,
        ),
        alias='subtitle',
    )

    stylesheet.add(
        ParagraphStyle(
            name='UnorderedList',
            parent=stylesheet['Normal'],
            firstLineIndent=0,
            leftIndent=9,
            bulletIndent=0,
            spaceBefore=0,
            bulletFontName=font_family,
            bulletFontSize=10,
        ),
        alias='ul',
    )

    stylesheet.add(
        ParagraphStyle(
            name='Definition',
            parent=stylesheet['Normal'],
            firstLineIndent=0,
            leftIndent=36,
            bulletIndent=0,
            spaceAfter=2,
            spaceBefore=2,
            bulletFontName=font_family + '-BoldItalic',
        ),
        alias='dl',
    )

    stylesheet.add(
        ParagraphStyle(name='OrderedList', parent=stylesheet['Definition']), alias='ol'
    )

    stylesheet.add(
        ParagraphStyle(
            name='Code',
            parent=stylesheet['Normal'],
            fontName='Courier',
            textColor=AXIVION_RED,
            fontSize=8,
            leading=8.8,
            leftIndent=36,
            firstLineIndent=0,
        )
    )

    stylesheet.add(
        ParagraphStyle(
            name='URL',
            parent=stylesheet['Normal'],
            fontName='Courier',
            textColor=AXIVION_RED,
            alignment=TA_CENTER,
        ),
        alias='u',
    )

    return stylesheet


class ReportDocTemplate(BaseDocTemplate):

    pageinfo = 'NO PAGEINFO SET'
    defaultPageSize = landscape(A4)
    border_x = 10 * mm
    border_y = 12 * mm

    @staticmethod
    def drawLogo(canvas):
        dir_path = os.path.dirname(os.path.realpath(__file__))
        canvas.drawImage(
            os.path.join(dir_path, 'axivion-LOGO_png8_sRGB.png'),
            ReportDocTemplate.defaultPageSize[0] - 60,
            ReportDocTemplate.defaultPageSize[1] - 30,
            width=50,
            height=21,
        )

    @staticmethod
    def onFirstPage(canvas, doc):
        ReportDocTemplate.onLaterPages(canvas, doc)

    @staticmethod
    def onLaterPages(canvas, doc):
        canvas.saveState()
        ReportDocTemplate.drawLogo(canvas)
        canvas.setFont(font_family, 9)
        canvas.drawString(
            ReportDocTemplate.border_x,
            ReportDocTemplate.border_y / 2,
            'Page %d -- %s -- Confidential' % (doc.page, ReportDocTemplate.pageinfo),
        )
        canvas.restoreState()

    @staticmethod
    def set_pageinfo(pageinfo):
        ReportDocTemplate.pageinfo = pageinfo

    def __init__(self, filename, **kw):
        BaseDocTemplate.__init__(self, filename, **kw)
        self.allowSplitting = True
        # for debugging set SHOW_BOUNDARIES to True
        SHOW_BOUNDARIES = False
        frames1up = [
            Frame(
                ReportDocTemplate.border_x,
                ReportDocTemplate.border_y,
                ReportDocTemplate.defaultPageSize[0] - 2 * ReportDocTemplate.border_x,
                ReportDocTemplate.defaultPageSize[1] - 2 * ReportDocTemplate.border_y,
                id='normal',
                showBoundary=SHOW_BOUNDARIES,
            ),
        ]
        frames2up = [
            Frame(
                ReportDocTemplate.border_x,
                ReportDocTemplate.border_y,
                (ReportDocTemplate.defaultPageSize[0] / 2)
                - ReportDocTemplate.border_x
                - 0.5 * ReportDocTemplate.border_x,
                ReportDocTemplate.defaultPageSize[1] - 2 * ReportDocTemplate.border_y,
                id='normal',
                showBoundary=SHOW_BOUNDARIES,
            ),
            Frame(
                (ReportDocTemplate.defaultPageSize[0] + 2 * ReportDocTemplate.border_x)
                / 2
                - 0.5 * ReportDocTemplate.border_x,
                ReportDocTemplate.border_y,
                (ReportDocTemplate.defaultPageSize[0] / 2)
                - ReportDocTemplate.border_x
                - 0.5 * ReportDocTemplate.border_x,
                ReportDocTemplate.defaultPageSize[1] - 2 * ReportDocTemplate.border_y,
                id='normal',
                showBoundary=SHOW_BOUNDARIES,
            ),
        ]
        self.addPageTemplates(
            [
                PageTemplate(
                    id='TitlePage',
                    frames=frames2up,
                    onPage=ReportDocTemplate.onFirstPage,
                    pagesize=ReportDocTemplate.defaultPageSize,
                ),
                PageTemplate(
                    id='1up',
                    frames=frames1up,
                    onPage=ReportDocTemplate.onLaterPages,
                    pagesize=ReportDocTemplate.defaultPageSize,
                ),
                PageTemplate(
                    id='2up',
                    frames=frames2up,
                    onPage=ReportDocTemplate.onLaterPages,
                    pagesize=ReportDocTemplate.defaultPageSize,
                ),
            ]
        )


class PDFStory:
    def __init__(self):
        self._styles = getStyleSheet()
        self._the_story: list = []
        self._doc = None

    def add_page_break(self):
        self._the_story.append(PageBreak())

    def add_frame_break(self):
        self._the_story.append(FrameBreak())

    def add_spacer(self, width, height):
        self._the_story.append(Spacer(width=width, height=height))

    def add_title(self, what):
        self._the_story.append(Paragraph(what, self._styles['Title']))

    def add_subtitle(self, what):
        self._the_story.append(Paragraph(what, self._styles['Subtitle']))

    def add_heading1(self, what):
        self._the_story.append(Paragraph(what, self._styles['Heading1']))

    def add_heading2(self, what):
        self._the_story.append(Paragraph(what, self._styles['Heading2']))

    def add_heading3(self, what):
        self._the_story.append(Paragraph(what, self._styles['Heading3']))

    def add_table(self, what: list, colWidths=None):
        self._the_story.append(Spacer(0, 2 * mm))
        if isinstance(what, list) and isinstance(what[0], tuple):
            data = []
            header = what[0]
            data.append(header)
            alignment = what[1]
            for r in what[2:]:
                line = []
                for cell in range(len(r)):
                    line.append(
                        Paragraph('%s' % r[cell], self._styles[alignment[cell]])
                    )
                data.append(line)
            self._the_story.append(
                Table(
                    data=data,
                    repeatRows=1,
                    splitByRow=True,
                    style=table_style,
                    colWidths=colWidths,
                )
            )
        else:
            raise Exception
        self._the_story.append(Spacer(0, 0.8 * cm))

    def add_normal_text(self, what):
        self._the_story.append(Paragraph(what, self._styles['Normal']))

    def switch_1up(self):
        self._the_story.append(NextPageTemplate('1up'))
        self.add_page_break()

    def switch_2up(self):
        self._the_story.append(NextPageTemplate('2up'))
        self.add_page_break()

    def add_code_text(self, what):
        self._the_story.append(Paragraph(what, self._styles['Code']))

    def initialize_template(self, output_filename, project_name, version):
        self._doc = ReportDocTemplate(output_filename)
        self._doc.set_pageinfo(f'Report for {project_name} in version "{version}"')

    def to_file(self):
        assert self._doc is not None
        self._doc.build(self._the_story)

    def add_barchart(self, what: list):
        the_width = 125 * mm
        the_height = (7 + 7 * len(what[0])) * mm
        drawing = Drawing(the_width, the_height)
        # the rectangle is useful for debugging (change fill/stroke accordingly)
        rect = Rect(0, 0, the_width, the_height)
        rect.fillColor = None
        rect.strokeColor = None
        bc = HorizontalBarChart()
        bc.data = what[1]
        bc.categoryAxis.categoryNames = what[0]

        bc.x = 60
        bc.y = 20
        bc.height = the_height - bc.y
        bc.width = the_width - bc.x
        bc.strokeColor = AXIVION_LIGHTGREY
        bc.fillColor = AXIVION_LIGHTGREY
        for bar in range(len(what[0])):
            bc.bars[bar].fillColor = AXIVION_RED
            bc.bars[bar].strokeColor = AXIVION_RED
        # bc.barLabels.fontSize = 8
        bc.barLabels.fontName = 'Helvetica'

        bc.valueAxis.valueMin = 0
        bc.valueAxis.valueMax = max(what[1][0]) * 1.1
        bc.valueAxis.valueStep = max(10, int(bc.valueAxis.valueMax / 10))
        bc.valueAxis.labels.fontName = 'Helvetica'
        bc.categoryAxis.labels.fontName = 'Helvetica'

        bc.barLabels.nudge = 10
        bc.barLabelFormat = '%d'
        # bc.categoryAxis.labels.dx = 8
        # bc.categoryAxis.labels.dy = -2
        drawing.add(rect)
        drawing.add(bc)
        self._the_story.append(drawing)

    def add_trendchart(self, categories: list, data: list):
        if len(data) < 2:
            self.add_normal_text("<i>Not enough data available</i>")
            return

        # floatify the input data
        def to_float(x) -> float:
            if x is None:
                return math.nan
            try:
                f = float(x)
                return f
            except ValueError:
                return math.nan

        data = [to_float(f) for f in data]

        valueMax = max(*data, 0)
        valueMin = min(*data, 1000000000000)
        if math.isinf(valueMax) or math.isnan(valueMax):
            valueMax = 1000000000000
        if math.isinf(valueMin) or math.isnan(valueMin):
            valueMin = -1000000000000

        the_width = 125 * mm
        the_height = 75 * mm
        drawing = Drawing(the_width, the_height)
        # the rectangle is useful for debugging (change fill/stroke accordingly)
        rect = Rect(0, 0, the_width, the_height)
        rect.fillColor = None
        rect.strokeColor = None
        lc = HorizontalLineChart()
        lc.data = [data]
        if len(data) < 30:
            lc.categoryAxis.categoryNames = [f'{v}' for v in range(len(data))]
        lc.lineLabels = categories
        lc.lines.strokeWidth = 2
        lc.x = 12 * mm
        lc.y = 7 * mm
        lc.height = the_height - lc.y
        lc.width = the_width - lc.x
        lc.strokeColor = AXIVION_LIGHTGREY
        lc.fillColor = AXIVION_LIGHTGREY

        if valueMax - valueMin < 10:
            valueMax = valueMin + 10
            lc.valueAxis.valueStep = 1
            lc.valueAxis.valueMin = valueMin - 1
            lc.valueAxis.valueMax = valueMax + 1
        else:
            lc.valueAxis.valueMin = max(0, valueMin / 1.025)
            lc.valueAxis.valueMax = max(10, valueMax * 1.025)
            lc.valueAxis.valueStep = max(10, int((valueMax - valueMin) / 12))
        lc.valueAxis.labels.fontName = 'Helvetica'
        lc.categoryAxis.labels.fontName = 'Helvetica'
        drawing.add(rect)
        drawing.add(lc)
        self._the_story.append(drawing)
