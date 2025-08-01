#!/usr/bin/env python3
import argparse
import dataclasses
import os
import json
import sys
from github import Github, Auth as GithubAuth
from github.PullRequest import PullRequest
from enum import Enum
from operator import attrgetter
from typing import List, Dict
from jinja2 import Environment, FileSystemLoader, StrictUndefined
from junit_utils import get_property_value, iter_xml_files


class TestStatus(Enum):
    PASS = 0
    FAIL = 1
    ERROR = 2
    SKIP = 3
    MUTE = 4
    FAIL_BUILD = 5

    def __lt__(self, other):
        return self.value < other.value


@dataclasses.dataclass
class TestResult:
    classname: str
    name: str
    status: TestStatus
    log_urls: Dict[str, str]
    elapsed: float
    is_timed_out: bool

    @property
    def status_display(self):
        return {
            TestStatus.PASS: "PASS",
            TestStatus.FAIL: "FAIL",
            TestStatus.FAIL_BUILD: "FAIL BUILD",
            TestStatus.ERROR: "ERROR",
            TestStatus.SKIP: "SKIP",
            TestStatus.MUTE: "MUTE",
        }[self.status]

    @property
    def elapsed_display(self):
        m, s = divmod(self.elapsed, 60)
        parts = []
        if m > 0:
            parts.append(f"{int(m)}m")
        parts.append(f"{s:.3f}s")
        return " ".join(parts)

    def __str__(self):
        return f"{self.full_name:<138} {self.status_display}"

    @property
    def full_name(self):
        return f"{self.classname}/{self.name}"

    @classmethod
    def from_junit(cls, testcase):
        classname, name = testcase.get("classname"), testcase.get("name")

        is_timed_out = False
        if testcase.find("failure") is not None:
            status = TestStatus.FAIL
            text = testcase.find("failure").text
            if text is not None and "Killed by timeout" in text:
                print(f"{classname}, {name} is_timed_out  = True")
                is_timed_out = True
            if text is not None and "skipped due to a failed build" in text:
                status = TestStatus.FAIL_BUILD
        elif testcase.find("error") is not None:
            status = TestStatus.ERROR
        elif get_property_value(testcase, "mute") is not None:
            status = TestStatus.MUTE
        elif testcase.find("skipped") is not None:
            status = TestStatus.SKIP
        else:
            status = TestStatus.PASS

        logs_directory = get_property_value(testcase, "url:logs_directory")
        if logs_directory is not None:
            logs_directory = f"{logs_directory}/index.html"

        log_urls = {
            "DIR": logs_directory,
            "Log": get_property_value(testcase, "url:Log"),
            "log": get_property_value(testcase, "url:log"),
            "stdout": get_property_value(testcase, "url:stdout"),
            "stderr": get_property_value(testcase, "url:stderr"),
            "backtrace": get_property_value(testcase, "url:backtrace"),
            "recipe_stderr": get_property_value(testcase, "url:recipe stderr"),
            "recipe_stdout": get_property_value(testcase, "url:recipe stdout"),
        }
        log_urls = {k: v for k, v in log_urls.items() if v}

        elapsed = testcase.get("time")

        try:
            elapsed = float(elapsed)
        except (TypeError, ValueError):
            elapsed = 0
            print(
                f"Unable to cast elapsed time for {classname}::{name}  value={elapsed!r}"
            )

        return cls(classname, name, status, log_urls, elapsed, is_timed_out)


class TestSummaryLine:
    def __init__(self, title):
        self.title = title
        self.tests = []
        self.is_failed = False
        self.report_fn = self.report_url = None
        self.counter = {s: 0 for s in TestStatus}

    def add(self, test: TestResult):
        self.is_failed |= test.status in (
            TestStatus.ERROR,
            TestStatus.FAIL,
            TestStatus.FAIL_BUILD,
        )
        self.counter[test.status] += 1
        self.tests.append(test)

    def add_report(self, fn, url):
        self.report_fn = fn
        self.report_url = url

    @property
    def test_count(self):
        return len(self.tests)

    @property
    def passed(self):
        return self.counter[TestStatus.PASS]

    @property
    def errors(self):
        return self.counter[TestStatus.ERROR]

    @property
    def failed(self):
        return self.counter[TestStatus.FAIL]

    @property
    def failed_build(self):
        return self.counter[TestStatus.FAIL_BUILD]

    @property
    def skipped(self):
        return self.counter[TestStatus.SKIP]

    @property
    def muted(self):
        return self.counter[TestStatus.MUTE]


class TestSummary:
    def __init__(self):
        self.lines: List[TestSummaryLine] = []
        self.is_failed = False

    def add_line(self, line: TestSummaryLine):
        self.is_failed |= line.is_failed
        self.lines.append(line)

    @property
    def is_empty(self):
        return len(self.lines) == 0

    def render_line(self, items):
        return f"| {' | '.join(items)} |"

    def render(self, add_footnote=False):
        github_srv = os.environ.get("GITHUB_SERVER_URL", "https://github.com")
        repo = os.environ.get("GITHUB_REPOSITORY", "ydb-platform/nbs")

        footnote_url = f"{github_srv}/{repo}/tree/main/.github/config"

        footnote = (
            "[^1]"
            if add_footnote
            else f'<sup>[?]({footnote_url} "All mute rules are defined here")</sup>'
        )

        columns = [
            "TESTS",
            "PASSED",
            "ERRORS",
            "FAILED",
            "FAILED BUILD",
            "SKIPPED",
            f"MUTED{footnote}",
        ]

        need_first_column = len(self.lines) > 1

        if need_first_column:
            columns.insert(0, "")

        result = [
            self.render_line(columns),
        ]

        if need_first_column:
            result.append(self.render_line([":---"] + ["---:"] * (len(columns) - 1)))
        else:
            result.append(self.render_line(["---:"] * len(columns)))

        for line in self.lines:
            report_url = line.report_url
            row = []
            if need_first_column:
                row.append(line.title)
            row.extend(
                [
                    render_pm(line.test_count, f"{report_url}", 0),
                    render_pm(line.passed, f"{report_url}#PASS", 0),
                    render_pm(line.errors, f"{report_url}#ERROR", 0),
                    render_pm(line.failed, f"{report_url}#FAIL", 0),
                    render_pm(line.failed_build, f"{report_url}#FAIL_BUILD", 0),
                    render_pm(line.skipped, f"{report_url}#SKIP", 0),
                    render_pm(line.muted, f"{report_url}#MUTE", 0),
                ]
            )
            result.append(self.render_line(row))

        if add_footnote:
            result.append("")
            result.append(f"[^1]: All mute rules are defined [here]({footnote_url}).")
        return result


def render_pm(value, url, diff=None):
    if value:
        text = f"[{value}]({url})"
    else:
        text = str(value)

    if diff is not None and diff != 0:
        if diff == 0:
            sign = "±"
        elif diff < 0:
            sign = "-"
        else:
            sign = "+"

        text = f"{text} {sign}{abs(diff)}"

    return text


def render_testlist_html(rows, fn, summary_url):
    TEMPLATES_PATH = os.path.join(os.path.dirname(__file__), "templates")

    env = Environment(
        loader=FileSystemLoader(TEMPLATES_PATH), undefined=StrictUndefined
    )

    status_test = {}
    has_any_log = set()

    for t in rows:
        status_test.setdefault(t.status, []).append(t)
        if any(t.log_urls.values()):
            has_any_log.add(t.status)

    for status in status_test.keys():
        status_test[status].sort(key=attrgetter("full_name"))

    status_order = [
        TestStatus.ERROR,
        TestStatus.FAIL,
        TestStatus.FAIL_BUILD,
        TestStatus.SKIP,
        TestStatus.MUTE,
        TestStatus.PASS,
    ]

    # remove status group without tests
    status_order = [s for s in status_order if s in status_test]

    content = env.get_template("summary.html").render(
        status_order=status_order,
        tests=status_test,
        has_any_log=has_any_log,
        summary_url=summary_url,
    )

    with open(fn, "w") as fp:
        fp.write(content)


def write_summary(summary: TestSummary, summary_out_env_path=""):
    if summary_out_env_path == "":
        summary_fn = os.environ.get("GITHUB_STEP_SUMMARY")
    else:
        summary_fn = summary_out_env_path

    if summary_fn:
        fp = open(summary_fn, "at")
    else:
        fp = sys.stdout

    if summary.is_empty:
        fp.write(
            ":red_circle: Test run completed, no test results found. Please check build logs."
        )
    else:
        for line in summary.render(add_footnote=True):
            fp.write(f"{line}\n")

    fp.write("\n")

    if summary_fn:
        fp.close()


def gen_summary(summary_url_prefix, summary_out_folder, paths):
    summary = TestSummary()

    for title, html_fn, path in paths:
        summary_line = TestSummaryLine(title)

        for fn, suite, case in iter_xml_files(path):
            test_result = TestResult.from_junit(case)
            summary_line.add(test_result)

        if not summary_line.tests:
            continue

        report_url = f"{summary_url_prefix}{html_fn}"

        render_testlist_html(
            sorted(summary_line.tests, key=lambda x: x.full_name),
            os.path.join(summary_out_folder, html_fn),
            summary_url=summary_url_prefix,
        )
        summary_line.add_report(html_fn, report_url)
        summary.add_line(summary_line)

    return summary


def get_comment_text(
    pr: PullRequest, summary: TestSummary, build_preset: str, test_history_url: str
):
    if summary.is_empty:
        return [
            f":red_circle: **{build_preset}**: Test run completed, no test results found for commit {pr.head.sha}. "
            f"Please check build logs."
        ]
    elif summary.is_failed:
        result = f":red_circle: **{build_preset}**: some tests FAILED"
    else:
        result = f":green_circle: **{build_preset}**: all tests PASSED"

    body = [f"{result} for commit {pr.head.sha}."]

    if test_history_url:
        body.append("")
        body.append(f"[Test history]({test_history_url})")

    body.extend(summary.render())

    return body


def update_pr_comment(
    run_number: int,
    pr: PullRequest,
    summary: TestSummary,
    build_preset: str,
    test_history_url: str,
    is_dry_run: bool,
):
    header = f"<!-- status pr={pr.number}, run={run_number}, build_preset={build_preset}, dry_run={is_dry_run} -->"

    body = None
    comment = None

    for c in pr.get_issue_comments():
        if c.body.startswith(header):
            print(f"Found comment with id={c.id}")
            comment = c
            body = [c.body]
            break

    if body is None:
        body = [header]

        if is_dry_run:
            body.extend(
                [
                    "> [!NOTE]",
                    "> This is a simulation, not a real result. If you see this, everything is as it should be.",
                    "",
                ]
            )
        body.extend(
            [
                "> [!NOTE]",
                "> This is an automated comment that will be appended during run.",
                "",
            ]
        )
    else:
        body.extend(["", ""])

    body.extend(get_comment_text(pr, summary, build_preset, test_history_url))

    body = "\n".join(body)

    if comment is None:
        print("Creating new comment")
        pr.create_issue_comment(body)
    else:
        print("Updating existing comment")
        comment.edit(body)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--summary-out-path", required=True)
    parser.add_argument(
        "--summary-out-env-path",
        required=False,
        help="File to write out summary instead of GITHUB_STEP_SUMMARY",
        default="",
    )
    parser.add_argument("--summary-url-prefix", required=True)
    parser.add_argument("--test-history-url", required=False)
    parser.add_argument(
        "--build-preset", default="default-linux-x86-64-relwithdebinfo", required=False
    )
    parser.add_argument(
        "--is-dry-run",
        default=False,
        action="store_true",
        help="Add mark in comments that this is simulation, not real result",
    )
    parser.add_argument("args", nargs="+", metavar="TITLE html_out path")
    args = parser.parse_args()

    if len(args.args) % 3 != 0:
        print("Invalid argument count")
        raise SystemExit(-1)

    paths = iter(args.args)
    title_path = list(zip(paths, paths, paths))

    summary = gen_summary(args.summary_url_prefix, args.summary_out_path, title_path)
    write_summary(summary, args.summary_out_env_path)

    if os.environ.get("GITHUB_EVENT_NAME") in ("pull_request", "pull_request_target"):
        gh = Github(auth=GithubAuth.Token(os.environ["GITHUB_TOKEN"]))

        with open(os.environ["GITHUB_EVENT_PATH"]) as fp:
            event = json.load(fp)

        run_number = int(os.environ.get("GITHUB_RUN_NUMBER"))
        pr = gh.create_from_raw_data(PullRequest, event["pull_request"])
        update_pr_comment(
            run_number,
            pr,
            summary,
            args.build_preset,
            args.test_history_url,
            args.is_dry_run,
        )


if __name__ == "__main__":
    main()
