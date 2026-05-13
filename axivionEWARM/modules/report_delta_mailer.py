#  Axivion Suite
#  Copyright (C) 2021-2025 Axivion GmbH
#  Copyright (C) 2025 The Qt Company GmbH, a subsidiary of The Qt Group
#  https://www.qt.io


import datetime
import email.message
import email.utils
import os
import smtplib
import socket
from typing import Tuple
from urllib.parse import urljoin

from axivion.dashboard.report import (
    Option,
    Report,
    ReportApiVersion,
    ReportRunner,
    ReportWriter,
)

SUBJECT_TEMPLATE = "Erosion Notification for Project {projectname}"
BODY_TEMPLATE = """Dear Axivion Dashboard User,

the following changes in the erosion indicators for project

  {projectname}

have been found during the comparison between {startname} and {endname}:

{details}
--
This mail has been generated on {time} using """ + os.path.basename(
    __file__
)
ISSUE_KIND_TEMPLATE = """  {kind:24}: {added:3} added, {removed:3} removed
  {diffurl}

"""
ISSUE_KINDS = {
    'AV': 'Architecture Violations',
    'CL': 'Clones',
    'CY': 'Cycles',
    'DE': 'Dead Entities',
    'MV': 'Metric Violations',
    'SV': 'Style Violations',
}


def create_report_writer(_report_runner: ReportRunner) -> ReportWriter:
    '''Factory method called by the reporting framework module loader'''
    return DeltaMailer()


class DeltaMailer(ReportWriter):
    def report_api_version(self) -> ReportApiVersion:
        return ReportApiVersion(3, 0)

    def get_description(self) -> str:
        return 'Sends E-Mails containing the issue counts changes of a specified delta to a configurable list of recipients.'

    def get_options(self) -> Tuple[Option, ...]:
        return (
            Option.version(
                name='start_version',
                description='''The baseline version of the delta.

The issues, that are included in this version and not in `end_version` will be considered as `removed`.''',
                default='-1',
            ),
            Option.version(
                name='end_version',
                description='The issues that are included in this version and not in `start_version` are considered as `added`.',
                default='latest',
            ),
            Option.text(
                name='from',
                description="""The sender e-mail address.

Most e-mail servers will send a notification to this e-mail address in case one
of the E-Mails could not be delivered.""",
                default=f'Issue Delta Mailer <do-not-reply@{socket.getfqdn()}>',
            ),
            Option.list(
                name='recipients',
                description='Who should receive the issue counts e-mail?',
                element_type=Option.text(
                    name='recipient',
                    description='E-Mail address that should be notified',
                ),
                min_length=1,
            ),
            Option.text(
                name='smtp_server',
                default='localhost',
                description='an smtp server hostname[:port] that accepts mail without authentication',
            ),
        )

    def write_report(self, context: Report) -> None:
        log = context.get_logger()
        start = context.get_option_value('start_version')
        end = context.get_option_value('end_version')
        start_version = context.query_version(start)
        end_version = context.query_version(end)

        start_name = None
        end_name = None
        delta_text = ''
        for kind in sorted(ISSUE_KINDS.keys()):
            counts = context.get_project().count_issues(
                kind=kind, start=start_version['date'], end=end_version['date']
            )
            if end_name is None:
                start_name = counts.get('startVersion', {}).get('name')
                end_name = counts['endVersion']['name']

            if counts['totalAddedCount'] != 0 or counts['totalRemovedCount'] != 0:
                replacements = {
                    'kind': ISSUE_KINDS[kind],
                    'added': counts['totalAddedCount'],
                    'removed': counts['totalRemovedCount'],
                    'diffurl': urljoin(
                        context.get_dashboard().get_main_url(), counts['tableViewUrl']
                    ),
                }
                delta_text += ISSUE_KIND_TEMPLATE.format(**replacements)

        send_report = self.send(context, delta_text, start_name, end_name)
        log.info(send_report)
        context.get_sink('sendreport.txt').write(send_report.encode('utf-8'))

    def send(
        self, report: Report, delta_text: str, start_name: str, end_name: str
    ) -> str:
        if not delta_text:
            return 'No relevant changes in delta so no mail will be sent.'

        log = report.get_logger()
        smtp_server = split_host_port(report.get_option_value('smtp_server'))
        from_address = report.get_option_value('from')
        recipients = report.get_option_value('recipients')

        replacements = {
            'projectname': report.get_project().name(),
            'details': delta_text,
            'dashboardurl': report.get_dashboard().get_main_url(),
            'time': datetime.datetime.now().isoformat(),
            'startname': start_name,
            'endname': end_name,
        }
        subject = SUBJECT_TEMPLATE.format(**replacements)
        body = BODY_TEMPLATE.format(**replacements)

        log.info(
            'Sending mail with subject "%s" from "%s"',
            subject,
            from_address,
        )
        log.info(
            'Will be using SMTP server at "%s" with %s',
            smtp_server[0],
            'default port' if smtp_server[1] == 0 else f'port {smtp_server[1]}',
        )

        with smtplib.SMTP(host=smtp_server[0], port=smtp_server[1]) as s:
            msg = email.message.EmailMessage()
            # Some mail servers might not like it when no `To` is specified
            msg['From'] = from_address
            msg['Bcc'] = ', '.join(recipients)
            msg['Subject'] = subject
            msg['Date'] = email.utils.formatdate(localtime=True)
            msg.set_content(body)
            s.send_message(msg)

        return f'''E-Mail sent to {len(recipients)} recipients.

{subject}:

{delta_text}
Recipients: {', '.join(recipients)}
'''


def split_host_port(host_with_optional_port: str) -> Tuple[str, int]:
    split_attempt = host_with_optional_port.rsplit(':', 1)
    if not split_attempt[-1].isdigit():
        return (host_with_optional_port, 0)
    return (split_attempt[0], int(split_attempt[1]))
