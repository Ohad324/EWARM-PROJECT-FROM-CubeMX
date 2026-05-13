#  Axivion Suite
#  Copyright (C) 2021-2025 Axivion GmbH
#  Copyright (C) 2025 The Qt Company GmbH, a subsidiary of The Qt Group
#  https://www.qt.io


from typing import Any, Mapping, Tuple, cast

from axivion.dashboard.report import (
    Option,
    Report,
    ReportApiVersion,
    ReportRunner,
    ReportWriter,
)


def create_report_writer(_report_runner: ReportRunner) -> ReportWriter:
    '''Factory method called by the reporting framework module loader'''
    return HISReportWriter()


class HISReportWriter(ReportWriter):
    def report_api_version(self) -> ReportApiVersion:
        return ReportApiVersion(3, 0)

    def get_description(self) -> str:
        return 'Generates a simple report for HIS metric violations.'

    def get_options(self) -> Tuple[Option, ...]:
        return (
            Option.version(
                name='start_version',
                description='the report version for the report, defaults to oldest version if omitted',
                default='0',
            ),
        )

    def write_report(self, context: Report) -> None:
        start = context.get_option_value('start_version')
        start_version = context.query_version(start)
        end_version = context.query_version('latest')
        the_project = context.get_project()
        projectname = the_project.name()
        # Get HIS metric violations
        the_MV = cast(
            Mapping[str, Any],
            the_project.fetch_issues(
                kind='MV', start=start_version['date'], end=end_version['date']
            ),
        )
        the_SV = cast(
            Mapping[str, Any],
            the_project.fetch_issues(
                kind='SV', start=start_version['date'], end=end_version['date']
            ),
        )
        the_CY = cast(
            Mapping[str, Any],
            the_project.fetch_issues(
                kind='CY', start=start_version['date'], end=end_version['date']
            ),
        )

        # MetricName --> HIS NAME
        metric_to_his_name = {
            'Metric.Comment.Density': 'COMF (Comment density)',
            'Metric.NPATH': 'PATH (Number of Paths)',
            'Metric.Number_Of_Gotos': 'GOTO (Number of Gotos)',
            'Metric.McCabe_Complexity': 'v(G) (Cyclomatic Complexity)',
            'Metric.Number_Of_Calling_Routines': 'CALLING (Number of calling Routines)',
            'Metric.Number_Of_Called_Routines': 'CALLS (Number of called Routines)',
            'Metric.Number_Of_Parameters': 'PARAM (Number of Parameters)',
            'Metric.Maximum_Nesting': 'LEVEL (Nesting Level)',
            'Metric.Number_Of_Returns': 'RETURN (Number of Return statements)',
            'Metric.Halstead.Vocabulary_Frequency': 'VOCF (Vocabulary Frequency)',
        }

        metric_to_range = {}

        # MetricName --> EntityType --> NrOfViolations
        his_violations = {}

        # RuleName --> EntityType --> NrOfViolations
        misra_violations = {}

        # Get all MISRA violations defined in the HIS Metric standard
        def is_misra_style_violation(issue):
            if 'errorNumber' in issue:
                error_number = issue['errorNumber']
                return error_number.startswith('Misra')
            return False

        # Check if the given metric violation is from a HIS metric
        # TODO: just use the metric's name?
        def is_his_metrics_issue(issue):
            if 'metric' in issue:
                name = issue['metric']
                if name in metric_to_his_name:
                    return True
            return False

        # Add a metric violation to his_violations.
        def add_metric_violation(issue):
            metric_name = issue['metric']
            entity_type = issue['entityType']
            if metric_name in his_violations:
                if entity_type in his_violations[metric_name]:
                    his_violations[metric_name][entity_type] += 1
                else:
                    his_violations[metric_name][entity_type] = 1
            else:
                his_violations[metric_name] = {entity_type: 1}
            metric_to_range[metric_name] = (issue['min'], issue['max'])

        # Add a MISRA finding to misra_violations
        def add_misra_violation(issue):
            rule_name = issue['errorNumber']
            if rule_name in misra_violations:
                misra_violations[rule_name] += 1
            else:
                misra_violations[rule_name] = 1

        # HTML output routines

        def get_html_output_nomv_metrics():
            '''Output info about NOMV / NOMVPR metrics.'''
            result = '<H3>NOMV/NOMVPR (Violations of MISRA rules)</H3>'
            result += 'Only rules are listed for which at least one violation occurred.'
            result += '\n<p>\n'
            result += '<table border="1" style="width:50%">\n'
            result += '<tr><th>Misra Rule</th><th>Number of violations</th></tr>\n'
            rule_names = list(misra_violations.keys())[:]
            rule_names.sort()
            all_violations = 0
            table_cells = ''
            for rule_name in rule_names:
                table_cells += '<tr><td>%s</td><td>%s</td></tr>\n' % (
                    rule_name,
                    misra_violations[rule_name],
                )
                all_violations += misra_violations[rule_name]
            result += (
                'Total number of metric violations (NOMVPR): %s\n' % all_violations
            )
            result += table_cells
            result += '</table>\n'
            return result

        def get_html_metric_violations():
            '''Output info about HIS metrics besides ap_cg_cycle and NOMV/NOMVPR.'''
            result = ''
            metric_names = list(his_violations.keys())[:]
            metric_names.sort()
            for metric_name in metric_names:
                all_violations = 0
                his_name = metric_to_his_name[metric_name]
                # create a section for each metric
                result += '<H3>%s </H3>\n' % his_name
                result += '\n<BR>\n'
                result += '<table border="1" style="width:50%">\n'
                result += '<tr><th>Entity type</th><th>Number of violations</th></tr>\n'
                table_cells = ''
                for entity_type in his_violations[metric_name]:
                    #
                    table_cells += '<tr><td>%s</td><td>%s</td></tr>\n' % (
                        entity_type,
                        his_violations[metric_name][entity_type],
                    )
                    all_violations += his_violations[metric_name][entity_type]
                result += 'Total number of metric violations: %s\n<p>' % all_violations
                result += 'Used Range: '
                if metric_to_range[metric_name][0] and metric_to_range[metric_name][1]:
                    result += '%s - %s' % metric_to_range[metric_name]
                elif metric_to_range[metric_name][0]:
                    result += '>= %s ' % metric_to_range[metric_name][0]
                elif metric_to_range[metric_name][1]:
                    result += '<= %s ' % metric_to_range[metric_name][1]
                else:
                    result += '-'  # should not happen...
                result += '<p>\n'
                result += table_cells
                result += '</table>\n<p>\n'
            return result

        def get_html_cycle_violations(cycles_found):
            """Output info about cycles (ap_cg_cycle), but right now only whether there
            are some or not."""
            result = '<H3>AP_CG_CYCLE (Number of recursions)</H3>'
            if cycles_found:
                result += (
                    'Call graph recursions have been detected, so AP_CG_CYCLE &gt; 0. '
                    'Please inspect the concrete cycles in the dashboard.'
                )
            else:
                result += (
                    'No Call graph recursions have been detected, so AP_CG_CYCLE = 0.'
                )

            result += '\n<p>\n'
            return result

        def get_html_output(projectname, cycles_found):
            '''Main driver for HTML output.'''

            result = '<!DOCTYPE html><html lang="en">\n'
            result += '<head><meta charset="utf-8">\n'
            result += '<title>HIS Metrics Report for Project %s</title>' % projectname
            result += '</head>\n<body>\n'
            result += '<H1>HIS Report for Project %s</H1>' % projectname
            result += '</body>\n</html>\n'
            result += get_html_cycle_violations(cycles_found)
            result += get_html_output_nomv_metrics()
            result += get_html_metric_violations()
            return result

        # END HTML output routines

        # obtain metric violations
        for issue in the_MV['rows']:
            if is_his_metrics_issue(issue):
                add_metric_violation(issue)

        # obtain MISRA findings
        for issue in the_SV['rows']:
            if is_misra_style_violation(issue):
                add_misra_violation(issue)

        # check for cycles
        cycles_found = False
        for issue in the_CY['rows']:
            cycles_found = True
            break

        outfile = f'{the_project.name()}_his.html'
        with context.get_sink(outfile) as fp:
            fp.write(get_html_output(projectname, cycles_found).encode('utf-8'))
