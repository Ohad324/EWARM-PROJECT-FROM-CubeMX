#  Axivion Suite
#  Copyright (C) 2021-2025 Axivion GmbH
#  Copyright (C) 2025 The Qt Company GmbH, a subsidiary of The Qt Group
#  https://www.qt.io


import csv
from typing import Tuple

from axivion.dashboard.report import (
    Option,
    Report,
    ReportApiVersion,
    ReportRunner,
    ReportWriter,
)


def create_report_writer(_report_runner: ReportRunner) -> ReportWriter:
    '''Factory method called by the reporting framework module loader'''
    return MetricsCSVWriter()


class MetricsCSVWriter(ReportWriter):
    def report_api_version(self) -> ReportApiVersion:
        return ReportApiVersion(3, 0)

    def get_description(self) -> str:
        return 'A simple report generator for csv output.'

    def get_options(self) -> Tuple[Option, ...]:
        return (
            Option.version(
                name='start_version',
                description='the start version for the report, defaults to initial version if omitted',
                default='0',
            ),
            Option.version(
                name='end_version',
                description='the end version for the report, defaults to newest version if omitted',
                default='latest',
            ),
        )

    def write_report(self, context: Report) -> None:

        start = context.get_option_value('start_version')
        end = context.get_option_value('end_version')
        the_project = context.get_project()

        start_version = context.query_version(start)
        end_version = context.query_version(end)

        metrics = [
            'Metric.McCabe_Complexity',
            'Metric.Lines.Comment',
            'Metric.Lines.LOC',
            'Metric.Number_Of_Statements',
        ]

        suffixes = ['avg', 'cnt', 'max', 'min', 'ssum', 'std', 'sum']
        metrics_with_suffix = ['%s.%s' % (m, s) for m in metrics for s in suffixes]

        system_entity_id = the_project.get_system_entity()['entities'][0]['id']
        start_index = None
        end_index = None
        rows = []
        for metric in metrics_with_suffix:
            plot = the_project.fetch_metric_value_range(
                start=start_version['date'],
                end=end_version['date'],
                entity=system_entity_id,
                metric=metric,
            )
            assert start_index is None or start_index == plot['startVersion']['index']
            assert end_index is None or end_index == plot['endVersion']['index']
            start_index = plot['startVersion']['index']
            end_index = plot['endVersion']['index']
            rows.append([metric] + plot['values'])

        outfile = f'{the_project.name()}_metrics.csv'
        output_file = context.get_report_output_path(outfile)
        with open(output_file, 'w', newline='', encoding='utf-8') as out:
            writer = csv.writer(out, delimiter=str(';'))
            header_row = ['Metric']
            header_row.extend(range(start_index, end_index + 1))
            writer.writerow(header_row)
            for row in rows:
                writer.writerow(row)
