#  Axivion Suite
#  Copyright (C) 2021-2025 Axivion GmbH
#  Copyright (C) 2025 The Qt Company GmbH, a subsidiary of The Qt Group
#  https://www.qt.io


import sys

try:
    import openpyxl
    import openpyxl.utils
except ImportError:
    sys.stderr.write('This module requires openpyxl to be installed.\n')
    sys.exit(1)


import collections
import logging
import os
from typing import Tuple

import bauhaus.shared
from axivion.dashboard.report import (
    Option,
    Report,
    ReportApiVersion,
    ReportRunner,
    ReportWriter,
)
from bauhaus import axitertools
from bauhaus.style import rule_loader


def create_report_writer(_report_runner: ReportRunner) -> ReportWriter:
    '''Factory method called by the reporting framework module loader'''
    return MisraExcelWriter()


class MisraExcelWriter(ReportWriter):
    def __init__(self):
        self.standards = collections.defaultdict(list)

    def report_api_version(self) -> ReportApiVersion:
        return ReportApiVersion(3, 0)

    def get_description(self) -> str:
        return '''
Create a compliance report from information stored in a dashboard database.
The baseline_version can be used to flag a certain version as baseline; only issues newer as the
baseline are reported. The report_version can be used to create a report for
a certain version. For each rule that has violations or deviations, a details
sheet is produced if detail_sheets is issued.
'''

    def get_rules(self):
        rules = rule_loader.load_builtin_rules()
        for k, r in rules.items():
            if (
                not r.is_rule_group
                and r.rulegroup is not None
                and any(
                    k.startswith(s)
                    for s in [
                        'MisraC',
                        'AutosarC++',
                        'CertC',
                        'CWE',
                        'Qt',
                        'SecureCoding',
                    ]
                )
            ):
                s = k.split('-')[0]
                self.standards[s].append(k)
        for l in self.standards.values():
            l.sort(key=axitertools.natural_sort_key)

    def get_options(self) -> Tuple[Option, ...]:
        self.get_rules()
        return (
            Option.version(
                name='baseline_version',
                description='''the start (aka baseline) version for the report, defaults to initial version if omitted '
(i.e., all issues present in the report version are reported). '
Versions can be specified either as ISO8601 Date String, e.g. "2017-03-02"
or using their version number integer. You can also
specify negative indices, e.g. "-1" stands for the version before the latest, "-2" for the
version before "-1" and so on.''',
                default='0',
            ),
            Option.version(
                name='report_version',
                description='''the end (aka report) version for the report, defaults to newest version if omitted
Versions can be specified either as ISO8601 Date String, e.g. "2017-03-03"
or using their version number integer. "latest" is a special keyword you can use to specify
the latest version, which is also the default. You can also
specify negative indices, e.g. "-1" stands for the version before the latest, "-2" for the
version before "-1" and so on.''',
                default='latest',
            ),
            Option.multi_select(
                name='applied_standards',
                description='''identification of standard to generate the compliance report for;
rules not applied are flagged as disapplied''',
                choices=list(self.standards.keys()),
                defaults=('MisraC2012',),
            ),
            Option.text(
                name='rule_file',
                description='''file contains a set of rules to be used for the report,
rules not applied are flagged as disapplied''',
            ),
            Option.boolean(
                name='detail_sheets',
                description='create a sheet per rule that has violations or deviations',
                default=False,
            ),
            Option.select(
                name='loglevel',
                description='One of ERROR,WARNING,INFO,DEBUG',
                choices=('ERROR', 'WARNING', 'INFO', 'DEBUG'),
                default='INFO',
            ),
        )

    def write_report(self, context: Report) -> None:
        def quote(c):
            if isinstance(c, str):
                if c.startswith('='):
                    return "'%s" % c
                else:
                    return c
            else:
                return c

        def write_row(worksheet, rno, contents):
            for ci, c in enumerate(contents):
                worksheet.cell(row=rno, column=ci + 1).value = quote(c)

        def adjust_column_widths(worksheet):
            '''Adjusts the column widths in a worksheet'''
            column_letters = tuple(
                openpyxl.utils.get_column_letter(col_number + 1)
                for col_number in range(worksheet.max_column)
            )
            for column_letter in column_letters:
                worksheet.column_dimensions[column_letter].bestFit = True

        logger = context.get_logger()
        logger.setLevel(context.get_option_value('loglevel'))

        the_project = context.get_project()
        outfile = f'{the_project.name()}_misra.xlsx'
        output_file = context.get_report_output_path(outfile)
        start = context.get_option_value('baseline_version')
        baseline_version = context.query_version(start)
        end = context.get_option_value('report_version')
        report_version = context.query_version(end)
        detail_sheets = context.get_option_value('detail_sheets')
        applied_standards = context.get_option_value('applied_standards')
        if isinstance(applied_standards, str):
            applied_standards = applied_standards.splitlines()
        rule_file = context.get_option_value('rule_file')
        # collect all relevant rules for the report
        #
        # A rules found here is to be found either as checked in the database
        # or it is flagged as "disapplied" in the report
        relevant_rule_names = []

        # first we collect the rules from the "named" standards...
        for standard in applied_standards:
            if standard in self.standards:
                logger.info('applying standard "%s"' % standard)
                relevant_rule_names.extend(self.standards[standard])
            else:
                logger.error('error: unknown standard: "%s", aborting' % standard)
                return

        # ...then we add the rules found in configuration files.
        if rule_file:
            if os.path.exists(rule_file):
                with open(rule_file, 'r', encoding='utf-8') as fp:
                    rules_in_file = list(map(str.strip, fp.readlines()))
                    relevant_rule_names.extend(rules_in_file)
                    logger.info('applying rule file "%s"' % rule_file)
            else:
                logger.error('error: unknown file: "%s", aborting' % rule_file)
                return

        if logger.level > logging.INFO:
            logger.info('all applied rules:')
            for rule_name in relevant_rule_names:
                logger.info('   %s' % rule_name)

        # speedup for lookups if a given rule name is a relevant rule name
        relevant_rule_set = set(relevant_rule_names)

        the_issues = the_project.fetch_issues(
            kind='SV', start=baseline_version['date'], end=report_version['date']
        )

        # RuleName --> EntityType --> NrOfViolations
        relevant_violations_count = {}
        relevant_deviations_count = {}
        relevant_issues = collections.defaultdict(list)
        severity = {}

        def is_active(issue):
            if 'state' in issue:
                return issue['state'] == 'added'
            else:
                return False

        def is_relevant_style_violation(issue):
            return (
                is_active(issue)
                and 'errorNumber' in issue
                and issue['errorNumber'] in relevant_rule_set
            )

        for rule_name in relevant_rule_names:
            relevant_deviations_count[rule_name] = 0
            relevant_violations_count[rule_name] = 0
            severity[rule_name] = 'uncertain'

        def add_relevant_violation(issue):
            rule_name = issue['errorNumber']
            if rule_name in relevant_rule_set:
                severity[rule_name] = issue[
                    'severity'
                ]  # assert: all entries have the same severity
                relevant_issues[rule_name].append(issue)
                if issue['justification'] == '':
                    relevant_violations_count[rule_name] += 1
                else:
                    relevant_deviations_count[rule_name] += 1

        # obtain stylecheck issues
        for issue in the_issues['rows']:
            if is_relevant_style_violation(issue):
                add_relevant_violation(issue)

        # obtain performed checks
        performed_checks = set()
        for performed_check in the_project.fetch_executed_stylechecks(
            report_version['date']
        )['rules']:
            rule_name = performed_check['name']
            if rule_name in relevant_rule_set:
                performed_checks.add(rule_name)
        project_name = the_project.name()
        workbook = openpyxl.Workbook()
        worksheet = workbook.active
        worksheet.title = 'Report Information'
        write_row(worksheet, 2, ('Project Name', project_name))
        baseline_version_name = (
            baseline_version['name']
            if not 'startVersion' in the_issues
            else the_issues['startVersion']['name']
        )
        write_row(worksheet, 3, ('Baseline Version', baseline_version_name))
        report_version_name = (
            report_version['name']
            if not 'endVersion' in the_issues
            else the_issues['endVersion']['name']
        )
        write_row(worksheet, 4, ('Report Version', report_version_name))
        write_row(
            worksheet, 5, ('Tooling Version', bauhaus.shared.get_bauhaus_version())
        )
        adjust_column_widths(worksheet)

        summary_worksheet = workbook.create_sheet()
        summary_worksheet.title = 'Summary'
        write_row(
            summary_worksheet,
            1,
            ('Guideline', 'Category', 'Violations', 'Deviations', 'Compliance'),
        )

        for rule_index, rule_name in enumerate(relevant_rule_names):
            logger.info('processing rule "%s"' % rule_name)

            if relevant_violations_count[rule_name] > 0:
                state = 'Violations'
            elif relevant_deviations_count[rule_name] > 0:
                state = 'Deviations'
            elif rule_name in performed_checks:
                state = 'Compliant'
            else:
                state = 'Disapplied'

            write_row(
                summary_worksheet,
                rule_index + 2,
                (
                    rule_name,
                    severity[rule_name],
                    relevant_violations_count[rule_name],
                    relevant_deviations_count[rule_name],
                    state,
                ),
            )

            if detail_sheets:
                if len(relevant_issues[rule_name]) > 0:
                    logger.info('creating detail sheet for rule "%s"' % rule_name)
                    rule_worksheet = workbook.create_sheet()
                    rule_worksheet.title = rule_name
                    write_row(
                        rule_worksheet,
                        1,
                        (
                            'ID',
                            'Guideline',
                            'Position',
                            'Category',
                            'Message',
                            'Entity',
                            'Tags',
                            'Justification',
                        ),
                    )
                    for rowno, violation in enumerate(relevant_issues[rule_name]):
                        write_row(
                            rule_worksheet,
                            rowno + 2,
                            (
                                'SV%s' % violation['id'],
                                '%s' % violation['errorNumber'],
                                '%s:%s' % (violation['path'], violation['line']),
                                '%s' % violation['severity'],
                                '%s' % violation['message'],
                                '%s' % violation.get('entity', ''),
                                '%s' % violation.get('tags', ''),
                                '%s' % violation['justification'],
                            ),
                        )

                    adjust_column_widths(rule_worksheet)
                else:
                    logger.info('no detailed sheet for rule "%s"' % rule_name)

        adjust_column_widths(summary_worksheet)

        workbook.save(output_file)
