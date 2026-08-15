"""What the service says about itself while it is running.

Three things, and they are here together because they answer one question:
somebody is looking at a deployment they cannot attach a debugger to, and has to
be able to find out what it did.

**Logs are records rather than sentences.** One JSON object a line, with the
level, the logger, the message and whatever the call site attached. A container's
output is read by a machine before it is read by a person, and a line of prose
with numbers in it has to be parsed by a regular expression that somebody wrote
after the incident rather than before it.

**Every log line carries the request it belongs to.** The identifier is set once,
by the middleware, into a context variable, and every record made while handling
that request picks it up without being passed it. It reaches the worker as well:
the submission stores it on the job, and the worker binds it while it runs, so
the API's line about a submission and the worker's lines about the run it became
share a value that can be searched for.

**Metrics are counted here and rendered on demand.** Counters for what the
process has done, and gauges read from the database when somebody scrapes, since
what is worth knowing about a queue is its state now rather than a number this
process has been adding to. The format is Prometheus's text exposition, which is
a dozen lines to write and is what anything that scrapes will read; a client
library would be a dependency for the part of this that is a `for` loop over a
dictionary.
"""

from __future__ import annotations

import contextlib
import json
import logging
import sys
from collections.abc import Iterator
from contextvars import ContextVar
from datetime import UTC, datetime

#: The request being handled, or empty outside one.
REQUEST_ID: ContextVar[str] = ContextVar("request_id", default="")

#: The job being run, for the worker, which handles no requests.
JOB_ID: ContextVar[str] = ContextVar("job_id", default="")

#: How much of a client's `X-Request-Id` is kept.
#:
#: Sixty-four characters. Long enough for any identifier a proxy generates, and
#: short enough that a header cannot put a kilobyte of anything into a log line
#: or a database column.
REQUEST_ID_LENGTH = 64

#: What a LogRecord carries before anything is attached to it.
#:
#: Everything else in the record's dictionary was put there by a `logging` call
#: with `extra=`, which is what makes a structured record structured, so the
#: formatter writes those out and this is how it tells them apart.
_STANDARD = frozenset(logging.LogRecord("", 0, "", 0, "", None, None).__dict__) | {
    "message",
    "asctime",
    "taskName",
}


class Records(logging.Formatter):
    """One JSON object a line."""

    def format(self, record: logging.LogRecord) -> str:
        written = {
            "time": datetime.fromtimestamp(record.created, UTC).isoformat(),
            "level": record.levelname.lower(),
            "logger": record.name,
            "message": record.getMessage(),
        }

        request = REQUEST_ID.get()
        if request:
            written["request"] = request
        job = JOB_ID.get()
        if job:
            written["job"] = job

        for key, value in record.__dict__.items():
            if key not in _STANDARD:
                written[key] = value

        if record.exc_info:
            # The traceback as one string in a field, rather than as the several
            # lines Python prints, so that a record stays a record.
            written["traceback"] = self.formatException(record.exc_info)

        return json.dumps(written, default=str)


def configure_logging(level: int = logging.INFO) -> None:
    """Send structured records to standard output, and nothing anywhere else.

    Standard output because the process runs in a container and its output is
    the log. Handlers are replaced rather than added to, so that calling this
    twice does not double every line.
    """
    handler = logging.StreamHandler(sys.stdout)
    handler.setFormatter(Records())

    root = logging.getLogger()
    root.handlers = [handler]
    root.setLevel(level)


def clean_request_id(supplied: str) -> str:
    """A client's request identifier, reduced to something safe to write down.

    Kept because a proxy in front of this service has usually already given the
    request an identifier, and threading that one through means the service's
    logs and the proxy's can be read together. Bounded and filtered because it
    is a header: anything a stranger sends ends up in a log line and in a
    database column, and neither should be able to contain a newline.
    """
    allowed = "".join(
        character
        for character in supplied.strip()[:REQUEST_ID_LENGTH]
        if character.isalnum() or character in "-_."
    )
    return allowed


@contextlib.contextmanager
def context(*, request: str = "", job: str = "") -> Iterator[None]:
    """Attach a request or a job to every record made inside this block."""
    tokens = []
    if request:
        tokens.append((REQUEST_ID, REQUEST_ID.set(request)))
    if job:
        tokens.append((JOB_ID, JOB_ID.set(job)))
    try:
        yield
    finally:
        for variable, token in reversed(tokens):
            variable.reset(token)


#: Every counter this process has incremented, by name and labels.
#:
#: A plain dictionary rather than a registry object. What a registry provides is
#: a place to attach types and help text, and both are written once in `render`
#: below, where they are read.
_COUNTS: dict[tuple[str, tuple[tuple[str, str], ...]], int] = {}


def count(name: str, **labels: str) -> None:
    """Add one to a counter.

    Labels are the ones the caller passes and no others. The route label is a
    path template rather than a path, because `/jobs/{identifier}` is one thing
    to count and a job's address is one more series for every run ever
    submitted, which is how a metrics endpoint becomes the largest response a
    service produces.
    """
    key = (name, tuple(sorted(labels.items())))
    _COUNTS[key] = _COUNTS.get(key, 0) + 1


def reset() -> None:
    """Forget every counter. For tests, which should not see each other's."""
    _COUNTS.clear()


def _escape(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", " ")


def _series(name: str, labels: tuple[tuple[str, str], ...], value: float) -> str:
    if not labels:
        return f"{name} {value}"
    inside = ",".join(f'{key}="{_escape(text)}"' for key, text in labels)
    return f"{name}{{{inside}}} {value}"


#: What each metric is, in the order it is written out.
#:
#: The help text is the documentation a scraper shows beside the number, so it
#: says what the number is rather than restating its name.
_HELP = {
    "orrery_requests_total": ("counter", "Requests answered, by route and status."),
    "orrery_submissions_total": ("counter", "Submissions, by what became of them."),
    "orrery_jobs": ("gauge", "Jobs in the queue, by state."),
    "orrery_workers": ("gauge", "Workers whose heartbeat is inside the timeout."),
    "orrery_budget_used": ("gauge", "Work submitted inside the budget window."),
    "orrery_budget_total": ("gauge", "Work the service will take in that window."),
}


def render(gauges: dict[str, dict[str, float]]) -> str:
    """Everything this process knows, in Prometheus's text exposition format.

    `gauges` is read when the scrape arrives rather than kept here, because the
    queue's length is a property of the database and not of this process: two
    API containers would otherwise each report their own idea of it.
    """
    lines: list[str] = []
    for name, (kind, help_text) in _HELP.items():
        series = [
            _series(name, labels, value)
            for (counter, labels), value in sorted(_COUNTS.items())
            if counter == name
        ] + [
            _series(name, (("state", label),) if label else (), value)
            for label, value in gauges.get(name, {}).items()
        ]
        if not series:
            continue
        lines.append(f"# HELP {name} {help_text}")
        lines.append(f"# TYPE {name} {kind}")
        lines.extend(series)
    return "\n".join(lines) + "\n"
