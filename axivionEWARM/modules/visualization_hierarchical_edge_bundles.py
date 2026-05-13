#  Axivion Suite
#  Copyright (C) 2022-2025 Axivion GmbH
#  Copyright (C) 2025 The Qt Company GmbH, a subsidiary of The Qt Group
#  https://www.qt.io


import json
import os
from dataclasses import dataclass
from typing import List, Mapping, Tuple

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
    return HEB()


class HEB(VisualizationWriter):
    def visualization_api_version(self) -> VisualizationApiVersion:
        return VisualizationApiVersion(1, 1)

    def get_cache_config(self) -> CacheConfig:
        return CacheConfig(
            additional_files=(
                'hierarchy.py',
                'utils.py',
                'hierarchical_edge_bundles.spec',
            ),
            across_users=True,
        )

    def get_description(self) -> str:
        return """Visualize clones between files with hierarchical edge
        bundles. There are two types of clones: i) internal clones and ii)
        external clones. Internal clones are clones within the same file whereas
        external clones are clones between different files. External clones are
        depicted using edges bundled along the hierarchical structure of the
        files and directories of the underlying software system. The total
        number of internal and external clones for a file is shown in a tooltip
        when hovering the file. For a better overview of the edges, the files to
        be visualized are arranged radially. However, the radial view doesn't
        scale very well with the number of files. In order to include only
        certain files into the visualization (e.g., only header files), a list
        of file extensions can be specified. More details on this visualization
        can be found in 'Hierarchical Edge Bundles: Visualization of Adjacency
        Relations in Hierarchical Data' by Danny Holten."""

    def get_options(self) -> Tuple[Option, ...]:
        return (
            Option.text(
                name="File Extension Filter",
                default="h, hxx, c, cpp, cxx",
                description="File extensions to be included as comma-separated list.",
            ),
            Option.integer(
                name="Tension",
                default=85,
                min_value=0,
                max_value=100,
                description="Default bundling strength.",
            ),
        )

    def write_visualization(self, context: VisualizationContext) -> None:
        project = context.get_project()
        start = context.query_version('0')
        end = context.query_version('latest')

        ########################################################################
        ### Fetch the source-code files and filter them by file extension.
        ########################################################################
        extensions = context.get_option_value("File Extension Filter").split(',')
        extensions = [ext.strip() for ext in extensions]  # remove spaces from values
        extensions = [ext for ext in extensions if len(ext) > 0]  # remove empty values
        files: List[str] = [
            x['path']
            for x in project.fetch_analyzed_files(version=end['date'])['rows']
            if any(x['path'].endswith('.' + ext) for ext in extensions)
        ]

        # Visualization scripts may set the output type (plain_text, svg, vega
        # etc.) depending on whether and which data are available in the
        # Dashboard. That is, it is not necessary for a visualization script to
        # always generate the same output type.
        if not files:
            with context.output().plain_text() as pt:
                pt.write('No relevant files found.')
            return  # we don't want to write anything else

        ########################################################################
        ### Create file hierarchy with metrics.
        ########################################################################
        @dataclass
        class LinkPathRow:
            row: hierarchy.PathRow
            internal: int
            external: int

        tree = hierarchy.build_tree_from_file_paths(files)
        df_files = {
            row.path: LinkPathRow(row, 0, 0)
            for row in hierarchy.build_frame_from_tree(tree)
        }

        df_links: List[Mapping[str, int]] = []
        for link in [
            (x['leftPath'], x['rightPath'])
            for x in project.fetch_issues(
                kind='CL', start=start['date'], end=end['date']
            )['rows']
        ]:
            source = link[0]
            target = link[1]
            if source == target and source in df_files:
                df_files[source].internal += 1
            if source != target and source in df_files and target in df_files:
                df_files[source].external += 1
                df_files[target].external += 1
                df_links.append(
                    {
                        'source': df_files[source].row.id,
                        'target': df_files[target].row.id,
                    }
                )

        ########################################################################
        ### Serialize Vega data.
        ########################################################################
        file_data = []
        for row in df_files.values():
            datum = {
                "id": row.row.id,
                "name": row.row.name,
                "path": row.row.path,
                "internal": row.internal,
                "external": row.external,
            }
            if row.row.type == 'file':
                filename = row.row.path
                datum['url'] = create_file_url(
                    ctx=context, filename=filename, version=end['date']
                )
            if row.row.parent >= 0:
                datum["parent"] = row.row.parent
            file_data.append(datum)

        input_spec = os.path.dirname(__file__) + '/hierarchical_edge_bundles.spec'
        with open(input_spec, mode='r', encoding='utf-8') as spec:
            vega_json = (
                spec.read()
                .replace('@ENTITIES@', json.dumps(file_data, indent=4))
                .replace('@RELATIONS@', json.dumps(df_links, indent=4))
            )
        with context.output().vega() as vega:
            vega.write_json(vega_json)
