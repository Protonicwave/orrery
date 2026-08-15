"""Logs that a machine can read, and counters that say what happened.

None of this needs a database. What is asserted is the shape: that a record is
one JSON object with the fields something searching them will filter on, that a
header a stranger sent cannot put a newline into one, and that the metrics come
out in the format anything that scrapes will read.
"""

from __future__ import annotations

import json
import logging

from orrery_service.observability import (
    Records,
    clean_request_id,
    context,
    count,
    render,
    reset,
)


def a_record(message: str = "took a job", **extra: object) -> logging.LogRecord:
    record = logging.LogRecord(
        name="orrery_service.worker",
        level=logging.INFO,
        pathname="worker.py",
        lineno=1,
        msg=message,
        args=(),
        exc_info=None,
    )
    record.__dict__.update(extra)
    return record


def test_a_record_is_one_json_object() -> None:
    written = json.loads(Records().format(a_record()))
    assert written["level"] == "info"
    assert written["logger"] == "orrery_service.worker"
    assert written["message"] == "took a job"
    # An ISO 8601 instant with an offset, which is what a log store sorts on.
    assert written["time"].endswith("+00:00")


def test_what_the_call_site_attached_is_carried_through() -> None:
    written = json.loads(Records().format(a_record(particles=64, seconds=12.5)))
    assert written["particles"] == 64
    assert written["seconds"] == 12.5


def test_a_record_says_which_request_and_job_it_belongs_to() -> None:
    formatter = Records()
    assert "request" not in json.loads(formatter.format(a_record()))

    with context(request="abc123", job="a-job"):
        written = json.loads(formatter.format(a_record()))
    assert written["request"] == "abc123"
    assert written["job"] == "a-job"

    # And the block puts it back, so a worker between jobs does not attribute
    # its idle lines to the last one it ran.
    assert "job" not in json.loads(formatter.format(a_record()))


def test_a_traceback_is_a_field_rather_than_several_lines() -> None:
    try:
        raise ValueError("the run exited 1")
    except ValueError:
        record = a_record("the job failed")
        record.exc_info = __import__("sys").exc_info()

    written = json.loads(Records().format(record))
    assert "ValueError: the run exited 1" in written["traceback"]
    # One line out, whatever the traceback contained.
    assert "\n" not in Records().format(record)


def test_a_request_identifier_from_a_stranger_is_reduced_to_something_safe() -> None:
    assert clean_request_id("7f3a-0b1c.2") == "7f3a-0b1c.2"
    # Anything that could end a line, start a field or be read as markup goes.
    assert clean_request_id('a"b\nc d') == "abcd"
    assert len(clean_request_id("x" * 500)) == 64
    assert clean_request_id("   ") == ""


def test_counters_are_rendered_in_the_format_a_scraper_reads() -> None:
    reset()
    count("orrery_requests_total", route="/jobs", status="202")
    count("orrery_requests_total", route="/jobs", status="202")
    count("orrery_requests_total", route="/jobs", status="429")

    written = render({})
    assert "# TYPE orrery_requests_total counter" in written
    assert 'orrery_requests_total{route="/jobs",status="202"} 2' in written
    assert 'orrery_requests_total{route="/jobs",status="429"} 1' in written


def test_a_metric_nothing_has_recorded_is_left_out() -> None:
    reset()
    assert render({}) == "\n"


def test_gauges_are_taken_from_the_caller_rather_than_kept() -> None:
    reset()
    written = render(
        {
            "orrery_jobs": {"queued": 3, "running": 1},
            "orrery_workers": {"": 2},
        }
    )
    assert 'orrery_jobs{state="queued"} 3' in written
    assert 'orrery_jobs{state="running"} 1' in written
    # A single-valued gauge carries no label at all rather than an empty one.
    assert "orrery_workers 2" in written


def test_a_label_cannot_break_out_of_its_quotes() -> None:
    reset()
    count("orrery_requests_total", route='/jobs"} injected{x="', status="200")
    written = render({})
    assert '\\"' in written
    assert written.count("\n") == 3
