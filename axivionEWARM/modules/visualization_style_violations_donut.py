#  Axivion Suite
#  Copyright (C) 2022-2025 Axivion GmbH
#  Copyright (C) 2025 The Qt Company GmbH, a subsidiary of The Qt Group
#  https://www.qt.io


import functools
import itertools
import json
import os
from dataclasses import dataclass
from typing import List, MutableMapping, Tuple

from axivion.dashboard.report import Option, ReportRunner
from axivion.dashboard.visualization import (
    VisualizationApiVersion,
    VisualizationContext,
    VisualizationWriter,
)
from axivion.dashboard.visualization.cache_config import CacheConfig

from .utils import create_error_number_url


def create_visualization_writer(_runner: ReportRunner) -> VisualizationWriter:
    '''Factory method called by the visualization framework module loader'''
    return DonutChart()


class DonutChart(VisualizationWriter):
    def visualization_api_version(self) -> VisualizationApiVersion:
        return VisualizationApiVersion(1, 1)

    def get_cache_config(self) -> CacheConfig:
        return CacheConfig(
            additional_files=('utils.py', 'two_level_donut.spec'),
            across_users=True,
        )

    def get_description(self) -> str:
        return """Visualize the number of different style violations as
        two-level donut chart."""

    def get_options(self) -> Tuple[Option, ...]:
        return (
            Option.text(
                name="Error Number Filter",
                default="Misra",
                description="""Error Number to be filtered. The semantics of the
                filter correspond to the filter options of the issue table.""",
            ),
            Option.text(
                name="Severities",
                default="mandatory, required, advisory",
                description="""Severities to be included as comma-separated
                list. The listed values are matched exactly. The number of
                values must match to the number of values in option 'Colors'.""",
            ),
            Option.text(
                name="Colors",
                default="red, orange, blue",
                description="""The colors associated with the severities listed
                in option 'Severities' as comma-separated list. The number of
                values must match to the number of severities.""",
            ),
            Option.integer(
                name="Limit",
                default=3,
                min_value=1,
                max_value=10,
                description="""Top N violation types to be depicted explicitly
                in the second level. The remaining types are assigned to the
                group: 'others'.""",
            ),
        )

    def write_visualization(self, context: VisualizationContext) -> None:
        project = context.get_project()
        start = context.query_version('0')
        end = context.query_version('latest')

        ########################################################################
        ### Read and check options.
        ########################################################################
        severities = context.get_option_value('Severities').split(',')
        severities = [sev.strip() for sev in severities]  # remove spaces from values
        severities = [sev for sev in severities if len(sev) > 0]  # remove empty values

        colors = context.get_option_value('Colors').split(',')
        colors = [col.strip() for col in colors]  # remove spaces from values
        colors = [col for col in colors if len(col) > 0]  # remove empty values

        error_number_filter = context.get_option_value('Error Number Filter')
        error_number_filter = error_number_filter.strip()  # remove spaces

        # Visualization scripts may set the output type (plain_text, svg, vega
        # etc.) depending on whether and which data are available in the
        # Dashboard. That is, it is not necessary for a visualization script to
        # always generate the same output type.
        if len(severities) != len(colors):
            with context.output().plain_text() as pt:
                pt.write(
                    f"Number of severities ({len(severities)}) and"
                    + f" number of colors ({len(colors)}) don't match."
                )
            return  # we don't want to write anything else

        ########################################################################
        ### Fetch style violations.
        ########################################################################
        violations = [
            (
                str(row['errorNumber']),  # type: ignore
                str(row['severity']),  # type: ignore
            )
            for row in project.fetch_issues(
                kind='SV',
                start=start['date'],
                end=end['date'],
                column_filters={
                    'errorNumber': error_number_filter,
                    # Match severities exactly by enclosing the values, `v`,
                    # with: "v". The filter is set up by joining the terms with
                    # logical `OR` (|).
                    'severity': '"' + '" | "'.join(severities) + '"',
                },
            )[
                'rows'
            ]  # type: ignore
        ]

        if not violations:
            with context.output().plain_text() as pt:
                pt.write(
                    "No style violations found for error"
                    + f" number filter: {error_number_filter}"
                )
            return

        ########################################################################
        ### Count violation types.
        ########################################################################
        @dataclass
        class ViolationCount:
            error_number: str
            severity: str
            count: int
            url: str = ""

        violations.sort()  # `itertools.groupby` excepts a sorted sequence
        violation_counts = [
            ViolationCount(key[0], key[1], sum(1 for _ in group))
            for key, group in itertools.groupby(violations)
        ]
        violation_counts.sort(key=lambda x: x.count, reverse=True)

        # Create mapping: severity -> list of violation counts
        violation_counts_by_severity: MutableMapping[
            str,
            List[ViolationCount],
        ] = {severity: [] for severity in severities}
        for vc in violation_counts:
            violation_counts_by_severity[vc.severity].append(vc)

        # Aggregate all but the first `Limit` counts of each severity into a
        # group with artificial error number: 'others'.
        limit = int(context.get_option_value('Limit'))

        def _aggregate(x: ViolationCount, y: ViolationCount) -> ViolationCount:
            assert x.severity == y.severity, "incompatible severities"
            return ViolationCount('others', x.severity, x.count + y.count)

        for sev in severities:
            vcs = violation_counts_by_severity[sev]
            if len(vcs) > limit:
                violation_counts_by_severity[sev] = vcs[:limit] + [
                    functools.reduce(_aggregate, vcs[limit:])
                ]

        ########################################################################
        ### Add URL.
        ########################################################################
        for vcs in violation_counts_by_severity.values():
            disaggregated_error_number_filter = (
                " & ".join([f'!"{x.error_number}"' for x in vcs[:limit]])
                + f" & {error_number_filter}"
            )
            for vc in vcs:
                assert (
                    vc.error_number != 'others' or len(vcs) > limit
                ), "unexpected aggregation group 'others'"
                vc.url = create_error_number_url(
                    ctx=context,
                    error_number=disaggregated_error_number_filter
                    if vc.error_number == 'others'
                    else f'"{vc.error_number}"',
                    severity=f'"{vc.severity}"',
                    start=start['date'],
                    end=end['date'],
                )

        ########################################################################
        ### Serialize Vega data.
        ########################################################################
        violation_data = [
            {
                "category": vc.severity,
                "subcategory": vc.error_number,
                "count": vc.count,
                "url": vc.url,
            }
            for vc in itertools.chain.from_iterable(
                violation_counts_by_severity.values()
            )
        ]

        input_spec = os.path.dirname(__file__) + '/two_level_donut.spec'
        with open(input_spec, mode='r', encoding='utf-8') as spec:
            vega_json = (
                spec.read()
                .replace('@CATEGORY_TITLE@', 'Severity')
                .replace('@CATEGORIES@', '["' + '", "'.join(severities) + '"]')
                .replace('@CATEGORY_COLORS@', '["' + '", "'.join(colors) + '"]')
                .replace(
                    '@CATEGORY_URL@',
                    # If argument `severity` is empty (the default value), the
                    # generated link has no severity filter.
                    create_error_number_url(
                        ctx=context,
                        error_number=error_number_filter,
                        start=start['date'],
                        end=end['date'],
                    )
                    + '&filter_severity=',  # append severity filter
                )
                .replace('@VALUES@', json.dumps(violation_data, indent=4))
            )
        with context.output().vega() as vega:
            vega.write_json(vega_json)
