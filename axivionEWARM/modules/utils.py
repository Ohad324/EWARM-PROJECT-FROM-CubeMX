#  Axivion Suite
#  Copyright (C) 2022-2025 Axivion GmbH
#  Copyright (C) 2025 The Qt Company GmbH, a subsidiary of The Qt Group
#  https://www.qt.io


"""
A set of small utility functions which can be used by both,
visualization and report scripts.
"""

from urllib.parse import quote, urljoin

from axivion.dashboard.report import Context


def create_file_url(ctx: Context, filename: str, version='latest') -> str:
    """Creates a quoted URL for linking files in the dashboard.

    Example:

        <MAIN_URL>/projects/bash/files?version=latest&filename=lib/readline/readline.h

    where <MAIN_URL> is the base URL of the dashboard,
    `lib/readline/readline.h` is the argument value of parameter
    `filename`, and `latest` is the argument value of parameter
    `version` (the default value is `latest`).

    Parameters:
    ctx      -- module context of the visualization/reporting script
    filename -- name of the file to be linked
    version  -- version of the file to be linked
    """
    name = ctx.get_project().name()
    url = f"projects/{quote(name, safe='')}/files?version={quote(version, safe='')}&filename="
    main_url = ctx.get_dashboard().get_main_url()
    if not main_url.endswith('/'):
        main_url += '/'
    return urljoin(main_url, url) + quote(filename)


def create_error_number_url(
    ctx: Context, error_number: str, severity='', start='', end=''
) -> str:
    """Creates a quoted URL for linking style violations in the issue
       table.

    The primary use case for this function is to filter (or let's say
    select) style violations by 'Error Number'. Although error numbers
    have typically exactly one severity, nothing prevents from storing
    the same error number with different severities in the
    database---and hence in the dashboard. Thus, the severity of the
    style violation(s) to be linked can be filtered, too. If parameter
    `severity` is empty, the created URL has no severity filter.

    In addition to the error number and severity filters, a version
    range filter, starting at `start` (inclusive) and ending at `end`
    (inclusive), can be specified. Possible values for `start` and
    `end` are ISO 8601 datetime strings (e.g., "2017-03-03"), the
    spacial keyword "latest", which determines the latest version of a
    software, and version numbers (ordinal values with origin `0`;
    negative values are supported and indicate: "latest" minus the
    supplied value). Note that `end` must be after `start`, that is,
    if `start` refers to a version at time `t0`, `end` must refer to a
    version at time `t1` where `t0 < t1`---`t0` is older than
    `t1`. Both `start` and `end` are optional parameters. If, for
    instance, `start` is empty (the default value), the created URL
    has no start filter---the same is true for `end`.

    Example:

        <MAIN_URL>/projects/cppcheck/issues?filter_errorNumber=%22MisraC%2B%2B-7.1.1%22&filter_severity=%22required%22&kind=SV&namedFilter=p_show_all&start=0&end=7

    where <MAIN_URL> is the base URL of the dashboard. Note that this
    function does not make any assumptions about the kind of match
    (exact, contains, negate etc.) for `error_number` and
    `severity`. It is in the callers responsibility to configure the
    kind of match as needed. For example, if `x` is an error_number to
    be matched exactly, `"x"` must be supplied. The same is true for
    combinations, patterns, and so on.

    Parameters:
    ctx          -- module context of the visualization/reporting script
    error_number -- filter for the error number
    severity     -- filter for the severity
    start        -- filter for the start version
    end          -- filter for the end version

    """
    name = ctx.get_project().name()
    url = f"projects/{quote(name, safe='')}/issues?filter_errorNumber={quote(error_number, safe='')}&kind=SV&namedFilter=p_show_all"
    if severity:
        url += f"&filter_severity={quote(severity, safe='')}"
    if start:
        url += f"&start={quote(start, safe='')}"
    if end:
        url += f"&end={quote(end, safe='')}"
    main_url = ctx.get_dashboard().get_main_url()
    if not main_url.endswith('/'):
        main_url += '/'
    return urljoin(main_url, url)
