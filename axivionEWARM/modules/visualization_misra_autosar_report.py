#  Axivion Suite
#  Copyright (C) 2022-2025 Axivion GmbH
#  Copyright (C) 2025 The Qt Company GmbH, a subsidiary of The Qt Group
#  https://www.qt.io


import json
import os
from dataclasses import asdict, dataclass
from typing import List, Mapping, Set, Tuple

from axivion.dashboard.report import Option, ReportRunner
from axivion.dashboard.visualization import (
    VisualizationApiVersion,
    VisualizationContext,
    VisualizationWriter,
)
from axivion.dashboard.visualization.cache_config import CacheConfig

from .utils import create_error_number_url, create_file_url

COMPLIANT: str = "data:image/svg+xml;base64,PD94bWwgdmVyc2lvbj0iMS4wIiBlbmNvZGluZz0iVVRGLTgiIHN0YW5kYWxvbmU9Im5vIj8+CjwhLS0gQ3JlYXRlZCB3aXRoIElua3NjYXBlIChodHRwOi8vd3d3Lmlua3NjYXBlLm9yZy8pIC0tPgoKPHN2ZwogICB2ZXJzaW9uPSIxLjEiCiAgIGlkPSJzdmcxOCIKICAgd2lkdGg9IjEyOCIKICAgaGVpZ2h0PSIxMjQiCiAgIHZpZXdCb3g9IjAgMCAxMjggMTI0IgogICBzb2RpcG9kaTpkb2NuYW1lPSJJY29uX1RpY2tNYXJrX2M2MG0weTg1azAuYWkiCiAgIHhtbG5zOmlua3NjYXBlPSJodHRwOi8vd3d3Lmlua3NjYXBlLm9yZy9uYW1lc3BhY2VzL2lua3NjYXBlIgogICB4bWxuczpzb2RpcG9kaT0iaHR0cDovL3NvZGlwb2RpLnNvdXJjZWZvcmdlLm5ldC9EVEQvc29kaXBvZGktMC5kdGQiCiAgIHhtbG5zPSJodHRwOi8vd3d3LnczLm9yZy8yMDAwL3N2ZyIKICAgeG1sbnM6c3ZnPSJodHRwOi8vd3d3LnczLm9yZy8yMDAwL3N2ZyI+CiAgPGRlZnMKICAgICBpZD0iZGVmczIyIj4KICAgIDxjbGlwUGF0aAogICAgICAgY2xpcFBhdGhVbml0cz0idXNlclNwYWNlT25Vc2UiCiAgICAgICBpZD0iY2xpcFBhdGgzNCI+CiAgICAgIDxwYXRoCiAgICAgICAgIGQ9Ik0gMCw5MyBIIDk2IFYgMCBIIDAgWiIKICAgICAgICAgaWQ9InBhdGgzMiIgLz4KICAgIDwvY2xpcFBhdGg+CiAgPC9kZWZzPgogIDxzb2RpcG9kaTpuYW1lZHZpZXcKICAgICBpZD0ibmFtZWR2aWV3MjAiCiAgICAgcGFnZWNvbG9yPSIjZmZmZmZmIgogICAgIGJvcmRlcmNvbG9yPSIjMDAwMDAwIgogICAgIGJvcmRlcm9wYWNpdHk9IjAuMjUiCiAgICAgaW5rc2NhcGU6c2hvd3BhZ2VzaGFkb3c9IjIiCiAgICAgaW5rc2NhcGU6cGFnZW9wYWNpdHk9IjAuMCIKICAgICBpbmtzY2FwZTpwYWdlY2hlY2tlcmJvYXJkPSIwIgogICAgIGlua3NjYXBlOmRlc2tjb2xvcj0iI2QxZDFkMSIKICAgICBzaG93Z3JpZD0iZmFsc2UiIC8+CiAgPGcKICAgICBpZD0iZzI2IgogICAgIGlua3NjYXBlOmdyb3VwbW9kZT0ibGF5ZXIiCiAgICAgaW5rc2NhcGU6bGFiZWw9IlBhZ2UgMSIKICAgICB0cmFuc2Zvcm09Im1hdHJpeCgxLjMzMzMzMzMsMCwwLC0xLjMzMzMzMzMsMCwxMjQpIj4KICAgIDxnCiAgICAgICBpZD0iZzI4Ij4KICAgICAgPGcKICAgICAgICAgaWQ9ImczMCIKICAgICAgICAgY2xpcC1wYXRoPSJ1cmwoI2NsaXBQYXRoMzQpIj4KICAgICAgICA8ZwogICAgICAgICAgIGlkPSJnMzYiCiAgICAgICAgICAgdHJhbnNmb3JtPSJ0cmFuc2xhdGUoNjguMjgzMyw3Ny4zODQxKSI+CiAgICAgICAgICA8cGF0aAogICAgICAgICAgICAgZD0ibSAwLDAgYyAtMC43NSwwLjEzIC0xLjU0NCwtMC4yODEgLTEuOTg4LC0xLjAyOCBsIC0yNS4zOTIsLTQyLjc5MyAtMTYuNzY0LDkuODIyIGMgLTEuMTU4LDAuNjU3IC0yLjU3OCwwLjQ3NSAtMy4yMjIsLTAuNDggbCAtMy40MjcsLTUuMDcyIGMgLTAuNjQ0LC0wLjk1NiAtMC4xOTgsLTIuMjkgMC45NiwtMi45NDggbCAyMi42MiwtMTIuODE4IGMgMC40NDUsLTAuMjUzIDAuOTAyLC0wLjM0NSAxLjM3MSwtMC4zNDMgMC44MDYsLTAuMjM4IDEuNzA5LDAuMDc2IDIuMTkzLDAuODkxIEwgNS4yNzgsLTYuMDMyIEMgNS44NywtNS4wMzUgNS42MzYsLTMuNzA2IDQuNzMsLTMuMDg1IEwgMC42ODUsLTAuMzQzIEMgMC40NTksLTAuMTg3IDAuMjUsLTAuMDQzIDAsMCIKICAgICAgICAgICAgIHN0eWxlPSJmaWxsOiM2ZWJmNWQ7ZmlsbC1vcGFjaXR5OjE7ZmlsbC1ydWxlOm5vbnplcm87c3Ryb2tlOm5vbmUiCiAgICAgICAgICAgICBpZD0icGF0aDM4IiAvPgogICAgICAgIDwvZz4KICAgICAgPC9nPgogICAgPC9nPgogIDwvZz4KPC9zdmc+Cg=="
"""
The icon to be shown when the code is compliant. The string (i.e., everything
after `data:image/svg+xml;base64,`) was generated from a SVG file using the
`base64` command line tool (the tool is part of the `GNU coreutils`
package). The SVG can be recreated with the following command:

    echo <DATA> | base64 -d

where <DATA> is the string after `data:image/svg+xml;base64,`.
"""

NOT_COMPLIANT: str = "data:image/svg+xml;base64,PD94bWwgdmVyc2lvbj0iMS4wIiBlbmNvZGluZz0iVVRGLTgiIHN0YW5kYWxvbmU9Im5vIj8+CjwhLS0gQ3JlYXRlZCB3aXRoIElua3NjYXBlIChodHRwOi8vd3d3Lmlua3NjYXBlLm9yZy8pIC0tPgoKPHN2ZwogICB2ZXJzaW9uPSIxLjEiCiAgIGlkPSJzdmcyMjQiCiAgIHdpZHRoPSIxMzguNjY2NjciCiAgIGhlaWdodD0iMTQwIgogICB2aWV3Qm94PSIwIDAgMTM4LjY2NjY3IDE0MCIKICAgc29kaXBvZGk6ZG9jbmFtZT0iSWNvbl8gWF9yZWQuYWkiCiAgIHhtbG5zOmlua3NjYXBlPSJodHRwOi8vd3d3Lmlua3NjYXBlLm9yZy9uYW1lc3BhY2VzL2lua3NjYXBlIgogICB4bWxuczpzb2RpcG9kaT0iaHR0cDovL3NvZGlwb2RpLnNvdXJjZWZvcmdlLm5ldC9EVEQvc29kaXBvZGktMC5kdGQiCiAgIHhtbG5zPSJodHRwOi8vd3d3LnczLm9yZy8yMDAwL3N2ZyIKICAgeG1sbnM6c3ZnPSJodHRwOi8vd3d3LnczLm9yZy8yMDAwL3N2ZyI+CiAgPGRlZnMKICAgICBpZD0iZGVmczIyOCIgLz4KICA8c29kaXBvZGk6bmFtZWR2aWV3CiAgICAgaWQ9Im5hbWVkdmlldzIyNiIKICAgICBwYWdlY29sb3I9IiNmZmZmZmYiCiAgICAgYm9yZGVyY29sb3I9IiMwMDAwMDAiCiAgICAgYm9yZGVyb3BhY2l0eT0iMC4yNSIKICAgICBpbmtzY2FwZTpzaG93cGFnZXNoYWRvdz0iMiIKICAgICBpbmtzY2FwZTpwYWdlb3BhY2l0eT0iMC4wIgogICAgIGlua3NjYXBlOnBhZ2VjaGVja2VyYm9hcmQ9IjAiCiAgICAgaW5rc2NhcGU6ZGVza2NvbG9yPSIjZDFkMWQxIgogICAgIHNob3dncmlkPSJmYWxzZSIgLz4KICA8ZwogICAgIGlkPSJnMjMyIgogICAgIGlua3NjYXBlOmdyb3VwbW9kZT0ibGF5ZXIiCiAgICAgaW5rc2NhcGU6bGFiZWw9IlBhZ2UgMSIKICAgICB0cmFuc2Zvcm09Im1hdHJpeCgxLjMzMzMzMzMsMCwwLC0xLjMzMzMzMzMsMCwxNDApIj4KICAgIDxnCiAgICAgICBpZD0iZzIzNCIKICAgICAgIHRyYW5zZm9ybT0idHJhbnNsYXRlKDIzLjI5MTMsNzMuNDE4MikiPgogICAgICA8cGF0aAogICAgICAgICBkPSJNIDAsMCAyMS4zNTMsLTIxLjM1OCAwLC00Mi43MjUgNi45ODIsLTQ5LjcxIDI4LjM0NiwtMjguMzUyIDQ5LjY5OSwtNDkuNzEgNTYuNjkzLC00Mi43MjUgMzUuMzI5LC0yMS4zNTggNTYuNjkzLDAgNDkuNjk5LDYuOTgzIDI4LjM0NiwtMTQuMzc0IDYuOTgyLDYuOTgzIFoiCiAgICAgICAgIHN0eWxlPSJmaWxsOiNmMDRlNTg7ZmlsbC1vcGFjaXR5OjE7ZmlsbC1ydWxlOm5vbnplcm87c3Ryb2tlOm5vbmUiCiAgICAgICAgIGlkPSJwYXRoMjM2IiAvPgogICAgPC9nPgogIDwvZz4KPC9zdmc+Cg=="
"""
The icon to be shown when the code is not compliant. The string (i.e.,
everything after `data:image/svg+xml;base64,`) was generated from a SVG file
using the `base64` command line tool (the tool is part of the `GNU coreutils`
package). The SVG can be recreated with the following command:

    echo <DATA> | base64 -d

where <DATA> is the string after `data:image/svg+xml;base64,`.
"""


def create_visualization_writer(_runner: ReportRunner) -> VisualizationWriter:
    '''Factory method called by the visualization framework module loader'''
    return MultiChart()


class MultiChart(VisualizationWriter):
    def visualization_api_version(self) -> VisualizationApiVersion:
        return VisualizationApiVersion(1, 1)

    def get_cache_config(self) -> CacheConfig:
        return CacheConfig(
            additional_files=('utils.py', 'misra_autosar_report.spec'),
            across_users=True,
        )

    def get_description(self) -> str:
        return "The Axivion Misra/Autosar Report"

    def get_options(self) -> Tuple[Option, ...]:
        return (
            Option.select(
                name="Rule Set",
                choices=["Misra", "Autosar"],
                default="Misra",
                description="Rule set to be processed.",
            ),
        )

    def write_visualization(self, context: VisualizationContext) -> None:
        project = context.get_project()
        rule_set = context.get_option_value('Rule Set')
        start = context.query_version('0')
        end = context.query_version('latest')

        ########################################################################
        ### Fetch checked rules.
        ########################################################################
        rules = {
            str(row['name'])
            for row in project.fetch_rules(version=end['date'])['rules']
            if rule_set in row['name']
        }

        # Visualization scripts may set the output type (plain_text, svg, vega
        # etc.) depending on whether and which data are available in the
        # Dashboard. That is, it is not necessary for a visualization script to
        # always generate the same output type.
        if not rules:
            with context.output().plain_text() as pt:
                pt.write(
                    f"The configured '{rule_set}' rule set has"
                    + " no active rules for this project."
                )
            return  # we don't want to write anything else

        ########################################################################
        ### Fetch rule violations.
        ########################################################################
        @dataclass
        class Issue:
            error_number: str
            severity: str
            suppressed: bool
            justification: str
            path: str
            kind: str = ""

        issues = [
            Issue(
                str(row['errorNumber']),  # type: ignore
                str(row['severity']),  # type: ignore
                bool(row.get('suppressed', False)),  # type: ignore
                str(row['justification']),  # type: ignore
                str(row['path']),  # type: ignore
            )
            for row in project.fetch_issues(
                kind='SV',
                start=start['date'],
                end=end['date'],
                column_filters={
                    'errorNumber': rule_set,
                    # Only the following severities have a formal definition in
                    # the Axivion Misra/Autosar report. Other severities are
                    # thus filtered out.
                    'severity': '"mandatory" | "required" | "advisory"',
                },
            )[
                'rows'
            ]  # type: ignore
        ]

        if not issues:
            with context.output().plain_text() as pt:
                pt.write(
                    "No violations found for the configured rules"
                    + f" in rule set: {rule_set}"
                )
            return

        ########################################################################
        ### Determine issue kind.
        ########################################################################
        # There are the following kinds of issues:
        # - Violation:  Issues of severity `mandatory` (cannot be suppressed)
        #               or issues (of any severity) that are not suppressed.
        # - Deviation:  Suppressed issues of severity `required`/`advisory`
        #               with justification.
        # - Suppressed: Suppressed issues of severity `required`/`advisory`
        #               without justification.
        for issue in issues:
            issue.kind = (
                'Violation'
                if issue.severity == 'mandatory' or not issue.suppressed
                else 'Deviation'
                if issue.justification
                else 'Suppressed'
            )

        ########################################################################
        ### Group issues.
        ########################################################################
        issue_kind_groups: Mapping[str, List[Issue]] = {}
        issue_name_groups: Mapping[Tuple[str, str], List[Issue]] = {}
        issue_path_groups: Mapping[str, List[Issue]] = {}
        issue_severity_groups: Mapping[str, Set[str]] = {}

        # Make sure each severity is present in `issue_severity_groups`.
        issue_severity_groups.setdefault('mandatory', set())
        issue_severity_groups.setdefault('required', set())
        issue_severity_groups.setdefault('advisory', set())

        for issue in issues:
            issue_kind_groups.setdefault(issue.kind, []).append(issue)
            issue_name_groups.setdefault(
                # It might be unexpected, but the same error number may occur
                # with different severities. Hence, both values form the key.
                (issue.error_number, issue.severity),
                [],
            ).append(issue)
            issue_path_groups.setdefault(issue.path, []).append(issue)
            issue_severity_groups[issue.severity].add(issue.error_number)

        ########################################################################
        ### Compliance check.
        ########################################################################
        @dataclass
        class IssueKindCount:
            kind: str
            count: int
            url: str

        issue_kind_counts = [
            IssueKindCount(
                kind,
                len(group),
                # Unfortunately, we cannot formulate `Violations` as style
                # violation filter, because it is not possible to ignore the
                # `suppressed` filter for issues of severity `mandatory`.
                # However, the following should give a good approximation.
                create_error_number_url(
                    ctx=context,
                    error_number=rule_set,
                    severity='"mandatory" | "required" | "advisory"',
                    start=start['date'],
                    end=end['date'],
                )
                + '&filter_suppressed=False'
                if kind == 'Violation'
                else create_error_number_url(
                    ctx=context,
                    error_number=rule_set,
                    severity='"required" | "advisory"',
                    start=start['date'],
                    end=end['date'],
                )
                + '&filter_suppressed=True&filter_justification=!""'
                if kind == 'Deviation'
                else create_error_number_url(
                    ctx=context,
                    error_number=rule_set,
                    severity='"required" | "advisory"',
                    start=start['date'],
                    end=end['date'],
                )
                + '&filter_suppressed=True&filter_justification=""',
            )
            for kind, group in issue_kind_groups.items()
        ]

        findings = sum(
            1
            for issue in issues
            if (issue.kind in {'Violation', 'Suppressed'})
            # Ignore issues of severity `advisory` globally.
            and issue.severity != 'advisory'
        )
        compliance_icon = NOT_COMPLIANT if findings > 0 else COMPLIANT
        compliance_icon_tooltip = (
            'Code is compliant'
            if findings == 0
            else '1 issue left before code is compliant'
            if findings == 1
            else f"{findings} issues left before code is compliant"
        )
        # Again, we can only approximate the style violation filter here. Note
        # that URL is embedded verbatim into the Vega JSON string. Accordingly,
        # the quotes of the empty justification string must be quoted.
        compliance_icon_url = (
            create_error_number_url(
                ctx=context,
                error_number=rule_set,
                severity='"mandatory" | "required"',
                start=start['date'],
                end=end['date'],
            )
            + '&filter_justification=\\"\\"'
        )

        ########################################################################
        ### Top issues.
        ########################################################################
        @dataclass
        class IssueNameCount:
            name: str
            severity: str
            count: int
            url: str

        issue_name_counts = [
            IssueNameCount(
                name,
                severity,
                count,
                create_error_number_url(
                    ctx=context,
                    error_number=name,
                    severity=severity,
                    start=start['date'],
                    end=end['date'],
                ),
            )
            for name, severity, count in sorted(
                [
                    (
                        key[0],
                        key[1],
                        len(grp),
                    )
                    for key, grp in issue_name_groups.items()
                ],
                key=lambda x: x[2],  # sort by count
                reverse=True,  # in descending order
            )[:5]
        ]

        ########################################################################
        ### Top files.
        ########################################################################
        @dataclass
        class IssuePathCount:
            path: str
            count: int
            url: str

        issue_path_counts = [
            IssuePathCount(
                path,
                count,
                create_file_url(ctx=context, filename=path),
            )
            for path, count in sorted(
                [(key, len(grp)) for key, grp in issue_path_groups.items()],
                key=lambda x: x[1],  # sort by count
                reverse=True,  # in descending order
            )[:5]
        ]

        ########################################################################
        ### Violated rules.
        ########################################################################
        @dataclass
        class IssueSeverityCount:
            severity: str
            count: int
            url: str

        issue_severity_counts = [
            IssueSeverityCount(
                severity,
                len(group),
                create_error_number_url(
                    ctx=context,
                    error_number=rule_set,
                    severity=f'"{severity}"',
                    start=start['date'],
                    end=end['date'],
                ),
            )
            for severity, group in issue_severity_groups.items()
        ]

        ########################################################################
        ### Serialize Vega data.
        ########################################################################
        input_spec = os.path.dirname(__file__) + '/misra_autosar_report.spec'
        with open(input_spec, mode='r', encoding='utf-8') as spec:
            vega_json = (
                spec.read()
                .replace(
                    '@KINDS@',
                    json.dumps(
                        [asdict(ikc) for ikc in issue_kind_counts],
                        indent=4,
                    ),
                )
                .replace(
                    '@COMPLIANCE_CHART_ICON@',
                    compliance_icon,
                )
                .replace(
                    '@COMPLIANCE_CHART_ICON_TOOLTIP@',
                    compliance_icon_tooltip,
                )
                .replace(
                    '@COMPLIANCE_CHART_ICON_URL@',
                    compliance_icon_url,
                )
                .replace(
                    '@PATHS@',
                    json.dumps(
                        [asdict(ipc) for ipc in issue_path_counts],
                        indent=4,
                    ),
                )
                .replace(
                    '@NAMES@',
                    json.dumps(
                        [asdict(inc) for inc in issue_name_counts],
                        indent=4,
                    ),
                )
                .replace('@NUM_RULES@', str(len(rules)))
                .replace(
                    '@SEVERITIES@',
                    json.dumps(
                        [asdict(isc) for isc in issue_severity_counts],
                        indent=4,
                    ),
                )
            )
        with context.output().vega() as vega:
            vega.write_json(vega_json)
