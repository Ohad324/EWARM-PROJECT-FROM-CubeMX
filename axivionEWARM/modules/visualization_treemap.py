#  Axivion Suite
#  Copyright (C) 2022-2025 Axivion GmbH
#  Copyright (C) 2025 The Qt Company GmbH, a subsidiary of The Qt Group
#  https://www.qt.io


import json
import os
from dataclasses import dataclass
from typing import MutableMapping, Tuple

from axivion.dashboard.report import Option, ReportRunner
from axivion.dashboard.visualization import (
    VisualizationApiVersion,
    VisualizationContext,
    VisualizationWriter,
)
from axivion.dashboard.visualization.cache_config import CacheConfig

from . import hierarchy
from .utils import create_file_url


def create_visualization_writer(_runner: ReportRunner) -> VisualizationWriter:
    '''Factory method called by the visualization framework module loader'''
    return TreeMap()


class TreeMap(VisualizationWriter):
    def visualization_api_version(self) -> VisualizationApiVersion:
        return VisualizationApiVersion(1, 1)

    def get_cache_config(self) -> CacheConfig:
        return CacheConfig(
            additional_files=('hierarchy.py', 'utils.py', 'treemap.spec'),
            across_users=True,
        )

    def get_description(self) -> str:
        return """Visualize the file hierarchy as Treemap (recursively nested
        rectangles) with a Heatmap overlay. The size of the rectangles is
        proportional to the metric 'Lines of Code'---this way expressing LOC as
        a proportion of the available space. The coloring (from white to red) of
        the rectangles correlates with the metric 'McCabe Cyclomatic
        Complexity'. That is, the higher MCC, the more intense the red
        coloration. The options 'White' and 'Red' can be used to specify when
        MCC is considered minimum (i.e., the rectangle becomes white) and when
        MCC is considered maximum (i.e., the rectangle becomes red)."""

    def get_options(self) -> Tuple[Option, ...]:
        return (
            Option.integer(
                name="White",
                default=0,
                min_value=0,
                max_value=4999,
                description="The value mapped to the color 'white'.",
            ),
            Option.integer(
                name="Red",
                default=1000,
                min_value=1,
                max_value=5000,
                description="The value mapped to the color 'red'.",
            ),
        )

    def write_visualization(self, context: VisualizationContext) -> None:
        project = context.get_project()
        version = context.query_version('latest')

        ########################################################################
        ### Fetch the source-code files and their associated metrics.
        ########################################################################
        analyzed_files = {
            analyzed_file['path']
            for analyzed_file in project.fetch_analyzed_files(version=version['date'])[
                'rows'
            ]
            # We don't want to show system header files in our Treemap.
            if not analyzed_file.get('isSystemHeader', False)
        }
        # Visualization scripts may set the output type (plain_text, svg, vega
        # etc.) depending on whether and which data are available in the
        # Dashboard. That is, it is not necessary for a visualization script to
        # always generate the same output type.
        if not analyzed_files:
            with context.output().plain_text() as pt:
                pt.write('No analyzed files available.')
            return  # we don't want to write anything else

        metrics = project.fetch_entity_metric_values(version=version['date'])['rows']
        files: MutableMapping[str, int] = {}  # maps path -> LOC
        for m in metrics:  # MCC will be assigned later
            if m['metric'] == ('Metric.Lines.File.LOC'):
                loc = int(m.get('value', 0))
                path = m.get('path')
                if loc > 0 and path in analyzed_files:
                    files[path] = loc
        if not files:
            with context.output().plain_text() as pt:
                pt.write(
                    f"""There are {len(analyzed_files)} analyzed files, but none of them include the
metric 'Metric.Lines.File.LOC', resulting in an empty Treemap.
Please adjust the configured limits for this metric in the
project configuration or enable the 'report_all_values' option."""
                )
            return  # we don't want to write anything else

        ########################################################################
        ### Create file hierarchy with metrics.
        ########################################################################
        @dataclass
        class SizeColorPathRow:
            row: hierarchy.PathRow
            size: int
            color: int

        tree = hierarchy.build_tree_from_file_paths(list(files.keys()))
        df_files = {
            row.path: SizeColorPathRow(row, 0, 0)
            for row in hierarchy.build_frame_from_tree(tree)
        }
        for path, loc in files.items():
            df_files[path].size = loc
        for m in metrics:
            if m['metric'] == ('Metric.McCabe_Complexity'):
                path = m.get('path')
                if path in df_files:  # may have been filtered out earlier
                    mcc = int(m.get('value', 0))
                    df_files[path].color += mcc + 1

        ########################################################################
        ### Serialize Vega data.
        ########################################################################
        file_data = []
        for row in df_files.values():
            datum = {
                "id": row.row.id,
                "name": row.row.name,
                "path": row.row.path,
                "size": row.size,
                "color": row.color,
            }
            if row.row.type == 'file':
                filename = row.row.path
                datum['url'] = create_file_url(
                    ctx=context, filename=filename, version=version['date']
                )
            if row.row.parent >= 0:
                datum['parent'] = row.row.parent
            file_data.append(datum)

        input_spec = os.path.dirname(__file__) + '/treemap.spec'
        with open(input_spec, mode='r', encoding='utf-8') as spec:
            vega_json = (
                spec.read()
                .replace('@WHITE@', str(context.get_option_value('White')))
                .replace('@RED@', str(context.get_option_value('Red')))
                .replace('@SIZE_TOOLTIP@', 'LOC')
                .replace('@COLOR_TOOLTIP@', 'MCC')
                .replace('@VALUES@', json.dumps(file_data, indent=4))
            )
        with context.output().vega() as vega:
            vega.write_json(vega_json)
