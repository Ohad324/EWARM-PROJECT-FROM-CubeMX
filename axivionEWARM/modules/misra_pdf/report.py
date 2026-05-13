#  Axivion Suite
#  Copyright (C) 2021-2025 Axivion GmbH
#  Copyright (C) 2025 The Qt Company GmbH, a subsidiary of The Qt Group
#  https://www.qt.io


'''
This class collects the report stories.
'''

try:
    import reportlab

    del reportlab

except ImportError:
    import sys

    sys.stderr.write('You have to install the reportlab package.\n')
    sys.exit(1)

import logging
import os
import time
from enum import Enum
from html import escape as htmlescape
from operator import itemgetter

from bauhaus.axitertools import natsorted, os_sorted
from bauhaus.dashboard import Project
from bauhaus.dashboard.exceptions import DashboardServerException

from .styles import AXIVION_DARK_GREEN_HEX, AXIVION_RED_HEX, PDFStory, cm

# pylint: enable=import-error


def escape(somestr: str) -> str:
    if len(somestr) > 100:
        return htmlescape(somestr[:100])
    else:
        return htmlescape(somestr)


def is_metric_in_scope(check: str):
    return check.startswith('Metric-')


def is_stylecheck_in_scope(check: str):
    return check.startswith(
        (
            'AutosarC++',
            'CertC-',
            'CertC++-',
            'CodingStyle-',
            'CWE-',
            'FaultDetection-',
            'Generic-',
            'MisraC',
            'Qt-',
            'SecureCoding-',
        )
    )


class Severity(Enum):
    mandatory = 'mandatory'
    required = 'required'
    advisory = 'advisory'
    dontcare = 'other'


class MISRA_State(Enum):
    violation = 0
    suppression = 1
    deviation = 2


class InternalError(Exception):
    pass


def create_tools_version_string(analysis_version) -> str:
    tools_version = analysis_version.get('toolsVersion')
    if not tools_version:
        return '<Unknown Axivion Suite Version>'
    return tools_version['name'] + ' (' + tools_version['number'] + ')'


class TheReport:
    _story: PDFStory = None
    _filters = None
    _output_file = None
    _issuekinds = None
    _all_issues = None
    _files_to_revisions = None
    _files_with_issues: set
    _sorted_files_under_analysis: list
    _rules_to_issues: dict

    # file based summary
    _path_to_issues: dict

    # module based summary
    _module_to_issues: dict

    # versioning information for axivion and project
    _end_version = None
    _readable_end_version = None
    _readable_start_version = None
    _start_version = None
    _tools_start_version = None
    _tools_end_version = None

    # issues grouped by severity
    _mandatory_issues: list
    _required_issues: list
    _advisory_issues: list
    _other_issues: list

    # for system trend report
    _available_metrics: dict
    _metric_to_value_range: dict
    _requested_metrics: tuple = (
        'Metric.Lines.LOC.sum',
        'Metric.Violations.Metric',
        'Metric.Violations.Style',
        'Metric.McCabe_Complexity.sum',
        'Metric.Number_Of_Statements.sum',
    )

    # for metric values overview table
    _metric_min_values: dict
    _metric_max_values: dict

    def __init__(
        self,
        the_project: Project,
        the_user: str,
        end_version: str,
        start_version: str,
        mvfilter: str,
        svfilter: str,
        logger: logging.Logger,
        output_file: str,
        show_details: bool,
    ):
        self._logger = logger
        self._project = the_project
        self._user = the_user
        self._filters = {'MV': mvfilter, 'SV': svfilter}
        self._story = PDFStory()
        self._output_file = output_file
        self._show_details = show_details
        self._logger.info('Initialising report start.')

        self._files_with_issues = set()
        self._rules_to_issues = {}
        self._path_to_issues = {}
        self._module_to_issues = {}
        self._mandatory_issues = []
        self._required_issues = []
        self._advisory_issues = []
        self._other_issues = []
        self._available_metrics = {}
        self._metric_to_value_range = {}
        self._metric_min_values = {}
        self._metric_max_values = {}

        # use the version names from the dashboard
        query = self._project.query_version(start_version)
        self._readable_start_version = query['name']
        self._tools_start_version = create_tools_version_string(query)
        self._start_version = start_version
        query = self._project.query_version(end_version)
        self._readable_end_version = query['name']
        self._tools_end_version = create_tools_version_string(query)
        self._end_version = end_version

        self._issuekinds = [
            kind['prefix']
            for kind in self._project.issuekinds()
            if kind['prefix'] in ['SV', 'MV']
        ]

        self._logger.info('Fetching metrics from dashboard.')
        for m in self._project.get_metrics()['metrics']:
            self._available_metrics[m['name']] = m
        self._logger.debug(
            'Available metrics: %s'
            % '\n'.join(natsorted(self._available_metrics.keys()))
        )

        entity = self._project.get_system_entity()['entities'][0]
        for metric in self._requested_metrics:
            if metric in self._available_metrics:
                self._metric_to_value_range[
                    metric
                ] = self._project.fetch_metric_value_range(
                    start=self._start_version,
                    end=self._end_version,
                    entity=entity['id'],
                    metric=metric,
                )[
                    'values'
                ]
        for metric in self._available_metrics.keys():
            try:
                if metric.endswith('.min'):
                    min_value = self._project.fetch_metric_value_range(
                        start=self._end_version,
                        end=self._end_version,
                        entity=entity['id'],
                        metric=metric,
                    )['values'][0]
                    if min_value:
                        self._metric_min_values[metric[:-4]] = int(min_value)
                if metric.endswith('.max'):
                    max_value = self._project.fetch_metric_value_range(
                        start=self._end_version,
                        end=self._end_version,
                        entity=entity['id'],
                        metric=metric,
                    )['values'][0]
                    if max_value:
                        self._metric_max_values[metric[:-4]] = int(max_value)
            except DashboardServerException:
                continue

        self._logger.info('Fetching issues from dashboard.')
        self._all_issues = {}
        for kind in self._issuekinds:
            self._logger.debug('Kind: %s' % kind)
            try:
                self._all_issues[kind] = self._project.fetch_issues(
                    kind=kind,
                    start='%s' % self._start_version,
                    end='%s' % self._end_version,
                    named_filter=self._filters[kind],
                    state=(
                        'changed'  # we want all issues in non-delta report
                        if self._readable_start_version == 'EMPTY'
                        else 'added'  # but only added issues in delta report
                    ),
                )
            except StopIteration as si:
                self._logger.error(f'{si}')

        # the project's fetch files gives a list of all files under version control
        # files outside the project directory are not in this list (but still can have issues)
        # e.g., header files from other components that are not excluded
        self._logger.info('Fetching files list.')
        self._files_to_revisions = {}
        for f in self._project.fetch_files(version=self._end_version)['rows']:
            path = f['path']
            self._files_to_revisions[path] = f['shortRevision']

        for kind in self._issuekinds:
            self._rules_to_issues[kind] = {}
            self._path_to_issues[kind] = {}

        self._logger.debug('Processing all issues.')
        # loop through all issues and prepare dicts for later stages
        files_under_analysis = set()
        for kind in self._issuekinds:
            for issue in self._all_issues[kind]['rows']:
                issue['kind'] = kind
                # for overall violation counts
                # count violations/suppressions per rule
                if kind == 'SV':
                    title = issue['errorNumber']
                    description = issue['message']
                elif kind == 'MV':
                    title = issue['errorNumber']
                    description = issue['metric']
                else:
                    raise NotImplementedError('Not yet implemented for %s' % kind)

                path = issue['path']
                if title not in self._rules_to_issues[kind]:
                    # (violation, suppression, deviation, description)
                    self._rules_to_issues[kind][title] = (
                        0,
                        0,
                        0,
                        description,
                    )
                (
                    violation,
                    suppression,
                    deviation,
                    description,
                ) = self._rules_to_issues[kind][title]

                # According to MISRA:
                # * an issue of severity "mandatory" cannot be suppressed or justified
                # * a violation can be suppressed and turned into a deviation
                # * a suppression needs a justification in order to be acceptable as a deviation
                # The sum of violations + suppressions + justifications = total number of issues.
                if issue['severity'] == Severity.mandatory.value or not issue.get(
                    'suppressed', False
                ):
                    violation += 1
                elif issue['justification']:
                    deviation += 1
                else:
                    suppression += 1
                self._rules_to_issues[kind][title] = (
                    violation,
                    suppression,
                    deviation,
                    description,
                )
                # collect issues per file
                if not path in self._path_to_issues[kind]:
                    self._path_to_issues[kind][path] = []
                self._path_to_issues[kind][path].append(issue)

                def path_to_module(path):
                    # sample implementation, this could also parse JSON to map files to modules
                    if path:
                        dirname, _ = os.path.split(path)
                        if not dirname:
                            dirname = "ROOT"
                    else:
                        dirname = "ROOT"
                    return dirname

                # collect issues per module
                module = path_to_module(path)
                if not module in self._module_to_issues:
                    self._module_to_issues[module] = []
                self._module_to_issues[module].append(issue)

                # we skip all issues of type 'Generic-Filemarker'
                if title == 'Generic-Filemarker':
                    files_under_analysis.add(path)
                    continue
                if path:
                    files_under_analysis.add(path)
                    self._files_with_issues.add(path)

                # create separate lists as per severity for style violations
                if kind == 'SV':
                    severity = issue['severity']
                    if severity == Severity.advisory.value:
                        self._advisory_issues.append(issue)
                    elif severity == Severity.required.value:
                        self._required_issues.append(issue)
                    elif severity == Severity.mandatory.value:
                        self._mandatory_issues.append(issue)
                    else:
                        self._other_issues.append(issue)
        self._sorted_files_under_analysis = os_sorted(files_under_analysis)
        self._logger.info('Initialising report end.')

    def preamble_report(self):
        # prepare report with some meta data
        self._logger.info('Creating preamble')
        cols = [
            ('Info', 'Value'),
            ('TableLeft', 'TableLeft'),
            ('Reporter name', self._user),
        ]
        if self._readable_start_version == 'EMPTY':
            cols += [
                ('Full report for project version', self._readable_end_version),
                ('Axivion version', self._tools_end_version),
            ]
        else:
            cols += [
                ('Project baseline version', self._readable_start_version),
                ('Axivion baseline version', self._tools_start_version),
                ('Project end version', self._readable_end_version),
                ('Axivion end version', self._tools_end_version),
            ]

        now = time.localtime()
        (ltz, tz) = time.tzname  # pylint: disable=unbalanced-tuple-unpacking

        cols += [
            (
                'Date of report generation',
                f'{now[0]}/{now[1]:02d}/{now[2]:02d}, {now[3]:02d}:{now[4]:02d} ({ltz}/{tz})',
            ),
        ]
        kind_to_nice_name = {
            'MV': 'metric violations',
            'SV': 'style violations',
        }
        for f in sorted(self._filters.keys()):
            if f in self._issuekinds:
                cols.append(
                    (f'Filter for {kind_to_nice_name[f]}', f'{self._filters[f]}')
                )

        self._story.add_heading1('Report Information')
        self._story.add_table(what=cols, colWidths=(5 * cm, None))

    def files_information(self, show_details=False):
        '''List the files under analysis.'''
        self._logger.info('Computing files information')
        if show_details:
            self._story.switch_1up()
        self._story.add_heading3('Files Information')

        # how many files with revision information have been analyzed
        # we assume here that Generic-Filemarker has been active and all files are marked
        files_with_issues_under_revision = 0
        for revfile in self._files_to_revisions.keys():
            if revfile in self._files_with_issues:
                files_with_issues_under_revision += 1
        self._story.add_normal_text(
            'Number of analyzed files in project index with revision: %s'
            % files_with_issues_under_revision
        )
        self._story.add_normal_text(
            'Overall number of analyzed files: %s'
            % len(self._sorted_files_under_analysis)
        )

        if not show_details:
            return
        data = [
            ('No.', 'Analyzed File', 'Revision'),
            (
                'TableRight',
                'TableLeft',
                'TableRight',
            ),
        ]
        idx = 0
        for filename in self._sorted_files_under_analysis:
            idx += 1
            if filename in self._files_to_revisions:
                rev = self._files_to_revisions[filename]
            else:
                rev = 'UNKNOWN'
            data.append((idx, f'<font name="Courier">{filename}</font>', rev))
        self._story.add_table(what=data, colWidths=(2 * cm, None, 4 * cm))

    def report_summary(self, show_others: bool = False):
        # overall violations
        self._logger.info('Computing overall violations')
        mandatory = len(self._mandatory_issues)
        advisory = len(self._advisory_issues)
        other = len(self._other_issues)
        required = 0
        suppressed = 0
        deviation = 0
        for issue in self._required_issues:
            if issue.get('suppressed', False):
                if issue['justification']:
                    deviation += 1
                else:
                    suppressed += 1
            else:
                required += 1
        categories = [
            'advisory',
            'req. just.',
            'req. supp.',
            'req. viol.',
            'mandatory',
        ]
        values = [
            advisory,
            deviation,
            suppressed,
            required,
            mandatory,
        ]
        if show_others and other > 0:
            categories = ['other'] + categories
            values = [other] + values

        if mandatory > 0 or required > 0 or suppressed > 0:
            compliance = f'<font color="{AXIVION_RED_HEX}">FAILED</font>'
        else:
            compliance = f'<font color="{AXIVION_DARK_GREEN_HEX}">PASSED</font>'
        self._story.add_heading3(f'Overall Compliance Check: {compliance}')
        self._story.add_normal_text(
            'According to MISRA deviation procedure, '
            'an issue of severity <i>mandatory</i> cannot be suppressed or justified. '
            'A violation of other severity can be suppressed and turned into a deviation. '
            'A deviation needs a justification in order to be acceptable as a deviation. '
            'The sum of violations + suppressions + justifications equals the total number of issues.'
        )

        self._story.add_heading3('Stylecheck Issues by Severity')
        self._story.add_barchart([categories, [values]])
        if self._readable_start_version == 'EMPTY':
            leading = 'The analysis identified'
            violations = lambda x: '1 violation' if x == 1 else f'{x} violations'
        else:
            leading = 'With regard to the baseline, the analysis identified'
            violations = lambda x: (
                '1 added violation' if x == 1 else f'{x} added violations'
            )
        self._story.add_normal_text(
            f'''{leading}
            {violations(mandatory)} of severity <i>mandatory</i>,
            {violations(required)} of severity <i>required</i>
            ({suppressed} suppressed, {deviation} suppressed with justification),
            and {violations(advisory)} of severity <i>advisory</i>.'''
        )
        if show_others and other > 0:
            self._story.add_normal_text(
                f'There were {other} violations of user defined categories <i>other</i>.'
            )

        self._story.add_heading3('Overall Issues')
        data = [
            ('Issue Kind', 'Issues'),
            ('TableLeft', 'TableRight'),
        ]

        sv_count = advisory + deviation + suppressed + required + mandatory
        if show_others:
            sv_count += other
        data.append(("Metric Violations", len(self._all_issues['MV']['rows'])))
        data.append(("Style Violations", sv_count))
        self._story.add_table(what=data, colWidths=(4 * cm, 5 * cm))

    def project_trend_charts(self):
        self._story.switch_2up()
        self._story.add_heading2('Trend Charts')
        is_second = False
        for m in self._requested_metrics:
            if m not in self._available_metrics:
                self._story.add_normal_text(f'No metric trend available for {m}.')
                continue
            displayName = self._available_metrics[m]['displayName']
            self._story.add_heading3(f'Trend for {displayName}')
            self._story.add_trendchart(
                categories=(displayName),
                data=self._metric_to_value_range[m],
            )
            if is_second:
                self._story.add_frame_break()
                is_second = False
            else:
                is_second = True

    def checks_performed(self, show_details=False):
        '''List the rules that were checked.'''
        self._logger.info('Building overall checks report')
        self._story.add_heading2('Report configuration details')
        performed_checks = set(
            performed_check['name']
            for performed_check in self._project.fetch_rules(self._end_version)['rules']
            if is_metric_in_scope(performed_check['name'])
            or is_stylecheck_in_scope(performed_check['name'])
        )

        status = None
        header_stylecheck = [
            (
                'No.',
                'Rule name',
                '#Viol.',
                '#Supp.',
                '#Just.',
            ),
            (
                'TableRight',
                'TableLeft',
                'TableRight',
                'TableRight',
                'TableRight',
            ),
        ]
        header_metric = [
            (
                'No.',
                'Metric name',
                '#Viol.',
                '#Supp.',
                '#Just.',
            ),
            (
                'TableRight',
                'TableLeft',
                'TableRight',
                'TableRight',
                'TableRight',
            ),
        ]
        data_metric = []
        data_stylecheck = []
        check_has_violations = 0
        metric_has_violations = 0

        metric_idx = 0
        style_idx = 0
        for check in natsorted(performed_checks):
            if is_metric_in_scope(check):
                metric_idx += 1
                v, s, j, _ = self._rules_to_issues['MV'].get(check, (0, 0, 0, ''))
                if sum((v, s, j)) > 0:
                    metric_has_violations += 1
                    status_v = f'<font color="{AXIVION_RED_HEX}">{v}</font>'
                    status_s = f'<font color="{AXIVION_RED_HEX}">{s}</font>'
                    status_j = f'<font color="{AXIVION_RED_HEX}">{j}</font>'
                else:
                    status = f'<font color="{AXIVION_DARK_GREEN_HEX}">0</font>'
                    status_v, status_s, status_j = (status, status, status)
                data_metric.append(
                    (metric_idx, check, status_v, status_s, status_j),
                )
            elif is_stylecheck_in_scope(check):
                style_idx += 1
                v, s, j, _ = self._rules_to_issues['SV'].get(
                    check,
                    (
                        0,
                        0,
                        0,
                        '',
                    ),
                )
                if sum((v, s, j)) > 0:
                    check_has_violations += 1
                    status_v = f'<font color="{AXIVION_RED_HEX}">{v}</font>'
                    status_s = f'<font color="{AXIVION_RED_HEX}">{s}</font>'
                    status_j = f'<font color="{AXIVION_RED_HEX}">{j}</font>'
                else:
                    status = f'<font color="{AXIVION_DARK_GREEN_HEX}">0</font>'
                    status_v, status_s, status_j = (status, status, status)
                data_stylecheck.append(
                    (
                        style_idx,
                        check,
                        status_v,
                        status_s,
                        status_j,
                    )
                )
            else:
                pass
        if show_details:
            self._story.add_heading3('Filters')
            filter_info = [
                ('Filter Kind', 'Filter Details'),
                (
                    'TableLeft',
                    'TableLeft',
                ),
            ]
            kind_to_nice_name = {
                'MV': 'Metric violations',
                'SV': 'Style violations',
            }
            for f in sorted(self._filters.keys()):
                if f in self._issuekinds:
                    filter_info.append(
                        (
                            f'{kind_to_nice_name[f]}',
                            f'<font name="Courier">{self._filters[f]}</font>',
                        )
                    )
            self._story.add_table(filter_info, colWidths=(4 * cm, None))

        self._story.add_heading3('Stylechecks executed')
        self._story.add_normal_text(
            f'The analysis executed {len(data_stylecheck)} stylechecks. '
            f'There were {check_has_violations} stylechecks with violations '
            f'(wrt. applied filter <i>{self._filters["SV"]}</i>).'
        )
        if show_details:
            self._story.add_table(
                header_stylecheck + data_stylecheck,
                colWidths=(2 * cm, 5 * cm, 2 * cm, 2 * cm, 2 * cm),
            )

        self._story.add_heading3('Metrics executed')
        self._story.add_normal_text(
            f'The analysis executed {len(data_metric)} metric checks. '
            f'There were {metric_has_violations} metrics with violations '
            f'(wrt. applied filter <i>{self._filters["MV"]}</i>).'
        )
        if show_details:
            self._story.add_table(
                header_metric + data_metric,
                colWidths=(2 * cm, 5 * cm, 2 * cm, 2 * cm, 2 * cm),
            )

    def _get_metric_name(self, metric):
        '''Since we do not necessarily report all metric values,
        we might have xxx.MIN and xxx.MAX in the _available_metrics but not the base
        metric. This tries to get a human readable name for the metric anyhow.'''
        if metric in self._available_metrics:
            return self._available_metrics[metric]['displayName']
        max_name = f"{metric}.max"
        if f"{metric}.max" in self._available_metrics:
            return self._available_metrics[max_name]['displayName'].replace(
                'Maximum of ', ''
            )
        min_name = f"{metric}.max"
        if f"{metric}.min" in self._available_metrics:
            return self._available_metrics[min_name]['displayName'].replace(
                'Minimum of ', ''
            )
        return "n/a"

    def metrics_summary(self):
        '''Summary table of all metrics and their value ranges.'''
        self._story.switch_2up()

        if not self._metric_to_value_range:
            return

        header = [
            (
                'Metric name',
                'Min. Value',
                'Max. Value',
            ),
            (
                'TableLeft',
                'TableRight',
                'TableRight',
            ),
        ]

        self._story.add_heading3('Code Metrics Summary')
        data = [
            (
                self._get_metric_name(metric),
                (
                    self._metric_min_values[metric]
                    if metric in self._metric_min_values
                    else "n/a"
                ),
                (
                    self._metric_max_values[metric]
                    if metric in self._metric_max_values
                    else "n/a"
                ),
            )
            for metric in self._metric_min_values.keys()
        ]
        self._story.add_table(
            what=header + data,
            colWidths=(6 * cm, 3 * cm, 3 * cm),
        )

    def rules_with_issues(self, top=0):
        '''List all rules of performed checks that have violations.'''
        header_stylecheck = [
            (
                'Rule name',
                '#Viol.',
                '#Supp.',
                '#Just.',
                '#Tot.',
            ),
            (
                'TableLeft',
                'TableRight',
                'TableRight',
                'TableRight',
                'TableRight',
            ),
        ]
        header_metric = [
            (
                'Metric name',
                '#Viol.',
                '#Supp.',
                '#Just.',
                '#Tot.',
            ),
            (
                'TableLeft',
                'TableRight',
                'TableRight',
                'TableRight',
                'TableRight',
            ),
        ]
        data_metric = []
        data_stylecheck = []
        for kind in self._issuekinds:
            for errorNumber, (
                violations,
                suppressions,
                deviations,
                description,
            ) in self._rules_to_issues[kind].items():
                if kind == 'MV' and is_metric_in_scope(errorNumber):
                    data_metric.append(
                        (
                            description,
                            violations,
                            suppressions,
                            deviations,
                            violations + suppressions + deviations,
                        )
                    )
                elif kind == 'SV' and is_stylecheck_in_scope(errorNumber):
                    data_stylecheck.append(
                        (
                            errorNumber,
                            violations,
                            suppressions,
                            deviations,
                            violations + suppressions + deviations,
                        )
                    )
        if top > 0:
            # soccertable sort: wins (1), draws (2), losses (3), team (0)
            for key, rev in reversed([(1, True), (2, True), (3, True), (0, False)]):
                data_metric = natsorted(data_metric, key=itemgetter(key), reverse=rev)
                data_stylecheck = natsorted(
                    data_stylecheck, key=itemgetter(key), reverse=rev
                )
            data_stylecheck = data_stylecheck[0:top]
            data_metric = data_metric[0:top]
            is_top_x = f'(TOP {top})'
        else:
            data_metric = natsorted(data_metric, key=itemgetter(0))
            data_stylecheck = natsorted(data_stylecheck, key=itemgetter(0))
            is_top_x = ''

        self._story.add_heading3(f'Stylechecks with Issues {is_top_x}')
        if len(data_stylecheck) > 0:
            self._story.add_table(
                what=header_stylecheck + data_stylecheck,
                colWidths=(5 * cm, 2 * cm, 2 * cm, 2 * cm, 2 * cm),
            )
        else:
            self._story.add_normal_text('There are no stylechecks with issues.')
        self._story.add_heading3(f'Metrics with Issues {is_top_x}')
        if len(data_metric) > 0:
            self._story.add_table(
                what=header_metric + data_metric,
                colWidths=(5 * cm, 2 * cm, 2 * cm, 2 * cm, 2 * cm),
            )
        else:
            self._story.add_normal_text('There are no metrics with issues.')

    def stats_per_module(
        self,
    ):
        self._logger.info('Building statistics for modules report')
        self._story.add_heading2('Module Violation Statistics')
        header = [
            (
                'Rule name',
                'Severity',
                '#Viol.',
            ),
            (
                'TableLeft',
                'TableCenter',
                'TableRight',
            ),
        ]

        for module in natsorted(self._module_to_issues.keys()):
            summary_module = {ik: {} for ik in self._issuekinds}
            data_metric = []
            data_stylecheck = []
            check_has_violations = 0
            metric_has_violations = 0
            for issue in self._module_to_issues[module]:
                check = issue['errorNumber']
                if is_metric_in_scope(check):
                    v, _ = summary_module['MV'].get(check, (0, ''))
                    v += 1
                    summary_module['MV'][check] = (v, issue['severity'])
                elif is_stylecheck_in_scope(check):
                    v, _ = summary_module['SV'].get(check, (0, ''))
                    v += 1
                    summary_module['SV'][check] = (v, issue['severity'])
            for check in summary_module['MV'].keys():
                v, s = summary_module['MV'][check]
                if v > 0:
                    metric_has_violations += 1
                    data_metric.append(
                        (
                            check,
                            s,
                            v,
                        ),
                    )
            for check in summary_module['SV'].keys():
                v, s = summary_module['SV'][check]
                if v > 0:
                    check_has_violations += 1
                    data_stylecheck.append(
                        (
                            check,
                            s,
                            v,
                        )
                    )

            def sorter(x):
                _, severity, __ = x
                if severity == Severity.mandatory.value:
                    return 1
                elif severity == Severity.required.value:
                    return 3
                else:
                    return 5

            if check_has_violations > 0:
                self._story.add_heading3(
                    f'Stylechecks executed for module <i>{module}</i>'
                )
                self._story.add_normal_text(
                    f'The analysis found violations in {len(data_stylecheck)} stylechecks. '
                    f'There was a total of {check_has_violations} violations.'
                )
                data_stylecheck = natsorted(data_stylecheck, key=itemgetter(0))
                data_stylecheck = sorted(data_stylecheck, key=sorter)
                self._story.add_table(
                    header + data_stylecheck,
                    colWidths=(None, 3 * cm, 2 * cm),
                )
            if metric_has_violations > 0:
                self._story.add_heading3(f'Metrics executed for module <i>{module}</i>')
                self._story.add_normal_text(
                    f'The analysis found violations in {len(data_metric)} metric checks. '
                    f'There was a total of {metric_has_violations} violations.'
                )
                data_metric = natsorted(data_metric, key=itemgetter(0))
                data_metric = sorted(data_metric, key=sorter)
                self._story.add_table(
                    header + data_metric,
                    colWidths=(None, 3 * cm, 2 * cm),
                )

    def severity_per_file(
        self,
        severity: Severity = Severity.dontcare,
        show_suppressed: bool = True,
        show_stats: bool = True,
        show_details: bool = True,
    ):
        if severity == Severity.dontcare:
            sev = None
        else:
            sev = severity.value
        if show_details:
            self._story.switch_1up()
        else:
            self._story.switch_2up()
        self._story.add_heading1(
            f'Stylecheck Violations of Severity <i>{sev}</i> per File'
        )
        # we cannot suppress mandatory rules
        if sev == Severity.mandatory:
            show_suppressed = True
        if show_suppressed:
            header_stylecheck = [
                (
                    'Rule name',
                    'Message',
                    'Supp.',
                    'Line',
                    'Entity',
                ),
                (
                    'TableLeft',
                    'TableLeft',
                    'TableLeft',
                    'TableRight',
                    'TableLeft',
                ),
            ]
            colsWidths_stylecheck = (5 * cm, None, 2 * cm, 2 * cm, 5 * cm)
            sorter_stylecheck = itemgetter(3)
            header_stats = [
                (
                    'Rule name',
                    'Issues',
                    'Supp.',
                ),
                (
                    'TableLeft',
                    'TableRight',
                    'TableRight',
                ),
            ]
            colsWidths_stats = (5 * cm, 2 * cm, 2 * cm)
            sorter_stats = itemgetter(1)
        else:
            header_stylecheck = [
                (
                    'Rule name',
                    'Message',
                    'Line',
                    'Entity',
                ),
                (
                    'TableLeft',
                    'TableLeft',
                    'TableRight',
                    'TableLeft',
                ),
            ]
            colsWidths_stylecheck = (5 * cm, None, 2 * cm, 5 * cm)
            sorter_stylecheck = itemgetter(2)
            header_stats = [
                (
                    'Rule name',
                    'Issues',
                ),
                (
                    'TableLeft',
                    'TableRight',
                ),
            ]
            colsWidths_stats = (5 * cm, 2 * cm)
            sorter_stats = itemgetter(1)

        all_dirty = False
        for path in self._sorted_files_under_analysis:
            if path in self._path_to_issues['SV']:
                stats_per_file = {}
                data = []
                file_dirty = 0
                for issue in self._path_to_issues['SV'][path]:
                    errorNumber = issue['errorNumber']
                    if sev and issue['severity'] != sev:
                        continue
                    suppressed = issue.get('suppressed', False)
                    if not show_suppressed and suppressed:
                        continue
                    if is_stylecheck_in_scope(errorNumber):
                        file_dirty += 1
                        all_dirty = True
                        v, s = stats_per_file.get(errorNumber, (0, 0))
                        if (
                            # special case: we cannot suppress mandatory rules
                            issue['severity'] != Severity.mandatory.value
                            and suppressed
                        ):
                            s += 1
                        else:
                            v += 1
                        stats_per_file[errorNumber] = (v, s)
                        if not show_details:
                            continue
                        if show_suppressed:
                            line = (
                                errorNumber,
                                issue['message'],
                                'yes' if suppressed else 'no',
                                issue['line'],
                                escape(issue.get('entity', 'n/a')),
                            )
                        else:
                            line = (
                                errorNumber,
                                issue['message'],
                                issue['line'],
                                escape(issue.get('entity', 'n/a')),
                            )
                        data.append(line)
                if file_dirty > 0:
                    self._story.add_normal_text(
                        f'File has issues: <font name="Courier">{path}</font>'
                    )
                    if show_stats or show_details:
                        self._story.add_normal_text(
                            f'Style violations of category <i>{sev}</i> ({file_dirty}):'
                        )
                    if show_stats:
                        stats = []
                        for errorNumber, (v, s) in stats_per_file.items():
                            if show_suppressed:
                                stats.append((errorNumber, v, s))
                            else:
                                stats.append((errorNumber, v))
                        self._story.add_table(
                            what=header_stats
                            + sorted(stats, key=sorter_stats, reverse=True),
                            colWidths=colsWidths_stats,
                        )
                    if show_details:
                        self._story.add_table(
                            what=header_stylecheck
                            + sorted(data, key=sorter_stylecheck),
                            colWidths=colsWidths_stylecheck,
                        )
        if not all_dirty:
            self._story.add_normal_text(f'There are no files with {sev} issues.')

    def pending_per_file(
        self,
        show_advisory: bool = True,
    ):
        '''Lists the pending justifications, i.e., suppressed issues without a justification.'''
        self._story.switch_1up()
        if show_advisory:
            adv = '/advisory'
        else:
            adv = ''
        self._story.add_heading1(
            f'Pending Justifications for Issues of Severity <i>required{adv}</i> per File'
        )
        if show_advisory:
            header_stylecheck = [
                (
                    'Rule name',
                    'Message',
                    'Sev.',
                    'Line',
                    'Entity',
                ),
                (
                    'TableLeft',
                    'TableLeft',
                    'TableLeft',
                    'TableRight',
                    'TableLeft',
                ),
            ]
            colsWidths = (5 * cm, None, 2 * cm, 2 * cm, None)
            sorter = itemgetter(3)
            sev = [Severity.advisory.value, Severity.required.value]
        else:
            header_stylecheck = [
                (
                    'Rule name',
                    'Message',
                    'Line',
                    'Entity',
                ),
                (
                    'TableLeft',
                    'TableLeft',
                    'TableRight',
                    'TableLeft',
                ),
            ]
            colsWidths = (5 * cm, None, 2 * cm, None)
            sorter = itemgetter(2)
            sev = [Severity.required.value]

        all_dirty = False
        for path in self._sorted_files_under_analysis:
            data = []
            file_dirty = 0
            if path in self._path_to_issues['SV']:
                for issue in self._path_to_issues['SV'][path]:
                    errorNumber = issue['errorNumber']
                    if not issue['severity'] in sev:
                        continue
                    if not issue.get('suppressed', False) or issue['justification']:
                        continue
                    if is_stylecheck_in_scope(errorNumber):
                        file_dirty += 1
                        all_dirty = True
                        if show_advisory:
                            line = (
                                errorNumber,
                                issue['message'],
                                issue['severity'],
                                issue['line'],
                                escape(issue.get('entity', 'n/a')),
                            )
                        else:
                            line = (
                                errorNumber,
                                issue['message'],
                                issue['line'],
                                escape(issue.get('entity', 'n/a')),
                            )
                        data.append(line)
                if len(data) > 0:
                    self._story.add_normal_text(
                        f'File: <font name="Courier">{path}</font>'
                    )
                    self._story.add_normal_text(
                        f'Style violations with pending justifications of category <i>required{adv}</i> ({len(data)}):'
                    )
                    self._story.add_table(
                        what=header_stylecheck + sorted(data, key=sorter),
                        colWidths=colsWidths,
                    )
        if not all_dirty:
            self._story.add_normal_text(
                f'There are no files with pending justifications for issues of category <i>required{adv}</i>.'
            )

    def approved_per_file(
        self,
        show_advisory: bool = True,
    ):
        '''Lists the approved justifications, i.e., suppressed issues without a justification.'''
        self._story.switch_1up()
        if show_advisory:
            adv = '/advisory'
        else:
            adv = ''
        self._story.add_heading1(
            f'Approved Justifications for Issues of Severity <i>required{adv}</i> per File'
        )
        if show_advisory:
            header_stylecheck = [
                (
                    'Rule name',
                    'Message',
                    'Sev.',
                    'Line',
                    'Entity',
                    'Justification',
                ),
                (
                    'TableLeft',
                    'TableLeft',
                    'TableLeft',
                    'TableRight',
                    'TableLeft',
                    'TableLeft',
                ),
            ]
            colsWidths = (5 * cm, None, 2 * cm, 2 * cm, None, None)
            sorter = itemgetter(3)
            sev = [Severity.advisory.value, Severity.required.value]
        else:
            header_stylecheck = [
                (
                    'Rule name',
                    'Message',
                    'Line',
                    'Entity',
                    'Justification',
                ),
                (
                    'TableLeft',
                    'TableLeft',
                    'TableRight',
                    'TableLeft',
                    'TableLeft',
                ),
            ]
            colsWidths = (5 * cm, None, 2 * cm, None, None)
            sorter = itemgetter(2)
            sev = [Severity.required.value]

        all_dirty = False
        for path in self._sorted_files_under_analysis:
            data = []
            file_dirty = 0
            if path in self._path_to_issues['SV']:
                for issue in self._path_to_issues['SV'][path]:
                    errorNumber = issue['errorNumber']
                    if not issue['severity'] in sev:
                        continue
                    if not issue.get('suppressed', False) or not issue['justification']:
                        continue
                    if is_stylecheck_in_scope(errorNumber):
                        file_dirty += 1
                        all_dirty = True
                        if show_advisory:
                            line = (
                                errorNumber,
                                issue['message'],
                                issue['severity'],
                                issue['line'],
                                escape(issue.get('entity', 'n/a')),
                                issue['justification'],
                            )
                        else:
                            line = (
                                errorNumber,
                                issue['message'],
                                issue['line'],
                                escape(issue.get('entity', 'n/a')),
                                issue['justification'],
                            )
                        data.append(line)
                if len(data) > 0:
                    self._story.add_normal_text(
                        f'File: <font name="Courier">{path}</font>'
                    )
                    self._story.add_normal_text(
                        f'Style violations with pending justifications of category <i>required{adv}</i> ({len(data)}):'
                    )
                    self._story.add_table(
                        what=header_stylecheck + sorted(data, key=sorter),
                        colWidths=colsWidths,
                    )
        if not all_dirty:
            self._story.add_normal_text(
                f'There are no files with pending justifications for issues of category <i>required{adv}</i>.'
            )

    def metric_violations_per_file(
        self,
        show_suppressed: bool = True,
    ):
        self._story.switch_1up()
        self._story.add_heading1('Metric Violations per File')
        if show_suppressed:
            header_metric = [
                (
                    'Description',
                    'Supp.',
                    'Line',
                    'Entity',
                    'Value',
                    'Max.',
                    'Min.',
                ),
                (
                    'TableLeft',
                    'TableCenter',
                    'TableRight',
                    'TableLeft',
                    'TableRight',
                    'TableRight',
                    'TableRight',
                ),
            ]
            colWidths = (
                4 * cm,
                2 * cm,
                2 * cm,
                None,
                5 * cm,
                2 * cm,
                2 * cm,
            )
            sorter = itemgetter(2)
        else:
            header_metric = [
                (
                    'Description',
                    'Line',
                    'Entity',
                    'Value',
                    'Max.',
                    'Min.',
                ),
                (
                    'TableLeft',
                    'TableRight',
                    'TableLeft',
                    'TableRight',
                    'TableRight',
                    'TableRight',
                ),
            ]
            colWidths = (
                4 * cm,
                2 * cm,
                None,
                5 * cm,
                2 * cm,
                2 * cm,
            )
            sorter = itemgetter(1)

        dirty = False
        for path in self._sorted_files_under_analysis:
            data = []
            if path in self._path_to_issues['MV']:
                for issue in self._path_to_issues['MV'][path]:
                    suppressed = issue.get('suppressed', False)
                    if not show_suppressed and suppressed:
                        continue
                    if show_suppressed:
                        line = (
                            issue['description'],
                            'yes' if suppressed else 'no',
                            issue.get('line', ''),
                            escape(issue.get('entity', 'n/a')),
                            issue['value'],
                            issue['max'],
                            issue['min'],
                        )
                    else:
                        line = (
                            issue['description'],
                            issue.get('line', ''),
                            escape(issue.get('entity', 'n/a')),
                            issue['value'],
                            issue['max'],
                            issue['min'],
                        )
                    data.append(line)
                if len(data) > 0:
                    dirty = True
                    self._story.add_normal_text(
                        f'File: <font name="Courier">{path}</font>'
                    )
                    self._story.add_normal_text(f'Metric violations ({len(data)}):')
                    self._story.add_table(
                        what=header_metric + sorted(data, key=sorter),
                        colWidths=colWidths,
                    )
        if not dirty:
            self._story.add_normal_text('There are no files with metric issues.')

    def run(self):
        self._logger.info('Current version: %s' % self._readable_end_version)
        self._logger.info('Baseline version: %s' % self._readable_start_version)
        self._logger.debug('Active filters: %s' % self._filters)
        self._story.initialize_template(
            self._output_file, self._project.name(), self._readable_end_version
        )
        self._logger.info('Starting data generation.')
        self._story.add_title('Axivion Code Check Report')
        self._story.add_subtitle(f'{self._project.name()}')
        self.preamble_report()
        self._story.add_heading1('Report Summary')
        self.report_summary()
        self.checks_performed()
        self.files_information()
        self.metrics_summary()
        self.rules_with_issues()
        self.project_trend_charts()
        if self._show_details:
            self.files_information(show_details=True)
            self.stats_per_module()
            self._story.switch_2up()
            self._story.add_heading1('Report Details')
            self.checks_performed(show_details=True)
            self.rules_with_issues()
            for severity in [Severity.mandatory, Severity.required, Severity.advisory]:
                self.severity_per_file(
                    show_suppressed=True,
                    show_details=False,
                    show_stats=True,
                    severity=severity,
                )
            for severity in [Severity.mandatory, Severity.required, Severity.advisory]:
                self.severity_per_file(
                    show_suppressed=True,
                    show_details=True,
                    show_stats=False,
                    severity=severity,
                )
            self.pending_per_file(show_advisory=True)
            self.approved_per_file(show_advisory=True)
            self.metric_violations_per_file()

        self._logger.info('Finished data generation.')
        self._logger.info('Starting PDF generation.')
        self._story.to_file()
        self._logger.info('Finished PDF generation.')
