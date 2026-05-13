#  Axivion Suite
#  Copyright (C) 2021-2025 Axivion GmbH
#  Copyright (C) 2025 The Qt Company GmbH, a subsidiary of The Qt Group
#  https://www.qt.io


import os
from getpass import getuser
from typing import Tuple

from axivion.dashboard.report import (
    Option,
    Report,
    ReportApiVersion,
    ReportRunner,
    ReportWriter,
)

from .misra_pdf.report import TheReport


def create_report_writer(_report_runner: ReportRunner) -> ReportWriter:
    '''Factory method called by the reporting framework module loader'''
    return MisraReportWriter()


class MisraReportWriter(ReportWriter):
    def report_api_version(self) -> ReportApiVersion:
        return ReportApiVersion(3, 0)

    def get_description(self) -> str:
        return 'A more complex report generator for MISRA status as PDF output.'

    def get_options(self) -> Tuple[Option, ...]:
        return (
            Option.named_filter_mv(
                name='mvfilter',
                description='the named filter for metric violations',
                default='p_show_all',
            ),
            Option.named_filter_sv(
                name='svfilter',
                description='the named filter for style violations',
                default='p_show_all',
            ),
            Option.version(
                name='start_version',
                description='the project start version for the delta report, defaults to first if omitted',
                default='0',
            ),
            Option.version(
                name='end_version',
                description='the project end version for the delta report, defaults to latest if omitted',
                default='latest',
            ),
            Option.boolean(
                name='details', description='show details in report', default=False
            ),
            Option.select(
                name='loglevel',
                description='One of ERROR,WARNING,INFO,DEBUG',
                choices=('ERROR', 'WARNING', 'INFO', 'DEBUG'),
                default='INFO',
            ),
        )

    def write_report(self, context: Report) -> None:

        project = context.get_project()
        outfile = f'{project.name()}_misra.pdf'
        output = context.get_report_output_path(outfile)

        try:
            the_user = context.get_dashboard_user()
        except ValueError:
            the_user = getuser()

        logger = context.get_logger()
        logger.setLevel(context.get_option_value('loglevel'))

        end_version = context.query_version(context.get_option_value('end_version'))
        start_version = context.query_version(context.get_option_value('start_version'))

        TheReport(
            the_project=project,
            the_user=the_user,
            end_version=end_version['date'],
            start_version=start_version['date'],
            mvfilter=context.get_option_value('mvfilter'),
            svfilter=context.get_option_value('svfilter'),
            logger=logger,
            output_file=os.fspath(output),
            show_details=context.get_option_value('details'),
        ).run()
