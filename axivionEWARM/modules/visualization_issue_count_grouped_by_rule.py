#  Axivion Suite
#  Copyright (C) 2025 Axivion GmbH
#  Copyright (C) 2025-2026 The Qt Company GmbH, a subsidiary of The Qt Group
#  https://www.qt.io


from typing import Any, Dict, List, Mapping, Optional, Tuple, cast

from axivion.dashboard import escape_column_filter
from axivion.dashboard.report import Option, ReportRunner
from axivion.dashboard.visualization import (
    VisualizationApiVersion,
    VisualizationContext,
    VisualizationWriter,
)
from axivion.dashboard.visualization.cache_config import CacheConfig


def create_visualization_writer(_runner: ReportRunner) -> VisualizationWriter:
    '''Factory method called by the visualization framework module loader'''
    return GroupedIssueCountTable()


Row = Dict[str, Any]


class GroupedIssueCountTable(VisualizationWriter):
    def visualization_api_version(self) -> VisualizationApiVersion:
        return VisualizationApiVersion(1, 1)

    def get_cache_config(self) -> CacheConfig:
        return CacheConfig(across_users=True)

    def get_description(self) -> str:
        return "Example of a visualization using a table that shows issue counts per rule in a drill-down manner similar to how they are shown in axivion_config"

    def get_options(self) -> Tuple[Option, ...]:
        return (
            Option.multi_select(
                name="Issue Kinds",
                description="The issue kinds to be included in the table.",
                choices=("AV", "CL", "CY", "DE", "MV", "SV"),
                defaults=("SV",),
            ),
            Option.boolean(
                name="Show Suppressed Issues",
                default=False,
            ),
        )

    def write_visualization(self, context: VisualizationContext) -> None:
        project = context.get_project()
        latest = context.query_version('latest')
        end = latest["date"]
        issue_kinds = context.get_option_value("Issue Kinds")
        show_suppressed_issues = context.get_option_value("Show Suppressed Issues")

        name_key = "Rule Name"
        count_key = "Count"
        children_key = "children"
        link_key = 'url'

        kind_roots: List[List[Row]] = []

        for kind, kind_name in (("CL", "Clone"), ("CY", "Cycle"), ("DE", "DeadEntity")):
            if not kind in issue_kinds:
                continue
            count = cast(
                Mapping[str, Any],
                project.fetch_issues(
                    kind=kind,
                    end=end,
                    columns=(),
                    named_filter=(
                        "p_show_all" if show_suppressed_issues else "p_axivion_default"
                    ),
                    compute_total_row_count=True,
                ),
            )["totalRowCount"]
            kind_roots.append(
                [
                    {
                        name_key: kind_name,
                        count_key: count,
                        link_key: project.create_issue_table_url(
                            kind,
                            end=end,
                        ),
                    }
                ]
            )

        for kind in ("AV", "MV", "SV"):
            if not kind in issue_kinds:
                continue
            roots: List[Row] = []
            leaves: Dict[str, Row] = {}

            for issue in cast(
                Mapping[str, Any],
                project.fetch_issues(
                    kind=kind,
                    end=end,
                    column_sorters=(("errorNumber", "ASC"),),
                ),
            )['rows']:
                rulename = issue.get("errorNumber", "") or ""
                if rulename in leaves:
                    leaves[rulename][count_key] += 1
                else:
                    leaf: Row = {}
                    nodes: Optional[List[Row]] = roots
                    prefix = ""
                    for part in rulename.split("-"):
                        prefix += part + "-"
                        if nodes is None:
                            nodes = []
                            leaf[children_key] = nodes
                        for node in nodes:
                            if node[name_key] == part:
                                nodes = node.get(children_key, None)
                                leaf = node
                                break
                        else:
                            leaf = {
                                name_key: part,
                                link_key: project.create_issue_table_url(
                                    kind,
                                    column_filters={
                                        "errorNumber": f'"{escape_column_filter(prefix)}*"'
                                    },
                                    end=end,
                                )
                                if len(prefix) <= len(rulename)
                                else project.create_issue_table_url(
                                    kind,
                                    column_filters={
                                        "errorNumber": f'"{escape_column_filter(rulename)}"'
                                    },
                                    end=end,
                                ),
                            }
                            nodes.append(leaf)
                            nodes = None
                    leaf[count_key] = 1
                    leaves[rulename] = leaf
            kind_roots.append(roots)

        combined_roots = sorted(
            [row for root in kind_roots for row in root], key=lambda row: row[name_key]
        )

        def add_count(row: Row) -> int:
            if count_key in row:
                return row[count_key]
            count = sum((add_count(child) for child in row.get(children_key, [])))
            row[count_key] = count
            return count

        for root in combined_roots:
            add_count(root)

        with context.output().table(
            {"subRowsProp": children_key, "maxHeight": 15}
        ) as table_info:
            table_info.columns(
                [
                    {
                        "key": name_key,
                        "canFilter": True,
                        "canSort": True,
                        "width": 0.66,
                        "alignment": "left",
                        "type": "string",
                    },
                    {
                        "key": count_key,
                        "canFilter": False,
                        "canSort": True,
                        "width": 0.33,
                        "alignment": "right",
                        "type": "number",
                        "linkKey": link_key,
                    },
                ]
            )
            table_info.rows(combined_roots)
