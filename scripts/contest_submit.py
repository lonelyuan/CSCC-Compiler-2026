#!/usr/bin/env python3
"""Discover and use the authenticated CourseGrading contest submit interface.

The client requires an explicit ``submit`` subcommand for the state-changing
operation. It discovers platform identifiers from the logged-in contest page
and never prints cookies or the per-user polling URL.
"""

from __future__ import annotations

import argparse
import hashlib
import html
import http.cookiejar
import json
import os
import re
import stat
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import zipfile
from dataclasses import asdict, dataclass
from html.parser import HTMLParser
from pathlib import Path, PurePosixPath


ORIGIN = "https://course.educg.net"
MAX_RESULT_TEXT = 200_000
TERMINAL_VERDICTS = {"AC", "WA", "CE", "RE", "TLE", "MLE", "OLE", "PE", "SE"}
SUMMARY_KEYS = (
    "audit_pass",
    "audit_gate_passed",
    "perf_pass",
    "pass_rate",
    "geometric_mean_speedup",
    "functional_score",
    "performance_score",
    "total_score",
)


class SubmissionError(RuntimeError):
    """A safe-to-display submission workflow error."""


@dataclass(frozen=True)
class SubmitContract:
    contest_id: str
    task_id: str
    assign_id: str
    problem_id: str
    max_file_size: int
    upload_url: str
    refresh_url: str
    status_url: str

    def public_dict(self) -> dict[str, str | int]:
        return {
            "contest_id": self.contest_id,
            "task_id": self.task_id,
            "assign_id": self.assign_id,
            "problem_id": self.problem_id,
            "max_file_size": self.max_file_size,
        }


@dataclass(frozen=True)
class JudgeStatus:
    ret: str
    content_hash: str
    submission_time: str | None
    display_score: str | None
    verdict: str | None
    metrics: dict[str, str]
    text: str

    def summary_dict(self) -> dict[str, object]:
        return {
            "ret": self.ret,
            "submission_time": self.submission_time,
            "display_score": self.display_score,
            "verdict": self.verdict,
            "metrics": self.metrics,
        }


class _TextExtractor(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.parts: list[str] = []

    def handle_starttag(
        self, tag: str, attrs: list[tuple[str, str | None]]
    ) -> None:
        del attrs
        if tag.lower() in {"br", "p", "div", "tr", "li", "hr"}:
            self.parts.append("\n")

    def handle_endtag(self, tag: str) -> None:
        if tag.lower() in {"p", "div", "tr", "li"}:
            self.parts.append("\n")

    def handle_data(self, data: str) -> None:
        self.parts.append(data)

    def text(self) -> str:
        raw = "".join(self.parts).replace("\xa0", " ")
        lines = [re.sub(r"[ \t]+", " ", line).strip() for line in raw.splitlines()]
        return "\n".join(line for line in lines if line)


def _load_cookie_jar(path: Path) -> http.cookiejar.MozillaCookieJar:
    if not path.is_file():
        raise SubmissionError(f"cookie file does not exist: {path}")
    mode = stat.S_IMODE(path.stat().st_mode)
    if mode & 0o077:
        raise SubmissionError("cookie file permissions must be 0600 or stricter")
    jar = http.cookiejar.MozillaCookieJar()
    try:
        jar.load(str(path), ignore_discard=True, ignore_expires=True)
    except (OSError, http.cookiejar.LoadError) as exc:
        raise SubmissionError("cookie file is not valid Netscape cookie format") from exc
    if not any(cookie.domain.endswith("course.educg.net") for cookie in jar):
        raise SubmissionError("cookie file has no course.educg.net session cookies")
    return jar


def _request(
    opener: urllib.request.OpenerDirector,
    url: str,
    *,
    label: str,
    data: bytes | None = None,
    headers: dict[str, str] | None = None,
    timeout: float = 30,
) -> bytes:
    request_headers = {
        "User-Agent": "Mozilla/5.0 (Bisheng contest submission automation)",
        "Accept-Language": "zh-CN,zh;q=0.9,en;q=0.5",
    }
    if headers:
        request_headers.update(headers)
    request = urllib.request.Request(url, data=data, headers=request_headers)
    try:
        with opener.open(request, timeout=timeout) as response:
            final = urllib.parse.urlsplit(response.geturl())
            if final.scheme != "https" or final.hostname != "course.educg.net":
                raise SubmissionError(f"{label} redirected outside course.educg.net")
            if "login" in final.path.lower():
                raise SubmissionError(f"{label} redirected to login; refresh the session")
            status_code = getattr(response, "status", 200)
            if not 200 <= status_code < 300:
                raise SubmissionError(f"{label} returned HTTP {status_code}")
            return response.read()
    except urllib.error.HTTPError as exc:
        raise SubmissionError(f"{label} returned HTTP {exc.code}") from exc
    except urllib.error.URLError as exc:
        reason = getattr(exc, "reason", "network error")
        raise SubmissionError(f"{label} failed: {type(reason).__name__}") from exc


def _decode_page(payload: bytes) -> str:
    return payload.decode("utf-8", "replace")


def _require_match(pattern: str, text: str, label: str) -> re.Match[str]:
    match = re.search(pattern, text, re.DOTALL)
    if match is None:
        raise SubmissionError(f"could not discover {label}; the platform page changed")
    return match


def _validate_contest_url(url: str) -> tuple[str, str]:
    parsed = urllib.parse.urlsplit(url)
    if (
        parsed.scheme != "https"
        or parsed.hostname != "course.educg.net"
        or parsed.path != "/pages/contest/contest_submit.jsp"
    ):
        raise SubmissionError(
            "contest URL must target https://course.educg.net/pages/contest/contest_submit.jsp"
        )
    query = urllib.parse.parse_qs(parsed.query)
    contest_id = (query.get("contestID") or [""])[0]
    task_id = (query.get("taskID") or [""])[0]
    if not contest_id or not task_id:
        raise SubmissionError("contest URL must include contestID and taskID")
    return contest_id, task_id


def discover_contract(
    opener: urllib.request.OpenerDirector, contest_url: str
) -> SubmitContract:
    expected_contest_id, expected_task_id = _validate_contest_url(contest_url)
    page = _decode_page(_request(opener, contest_url, label="contest page"))
    if "/login/loginproc.jsp?logout=true" not in page:
        raise SubmissionError("contest page is not an authenticated session")

    load_match = _require_match(
        r"\.load\(\s*['\"](?P<path>/assignment/contestindex\.jsp)['\"]\s*,"
        r"\s*\{\s*assignID\s*:\s*['\"](?P<ajax_assign>[^'\"]+)['\"]\s*,"
        r"\s*contestID\s*:\s*['\"](?P<contest>[^'\"]+)['\"]\s*,"
        r"\s*taskID\s*:\s*['\"](?P<task>[^'\"]+)['\"]",
        page,
        "contest assignment loader",
    )
    if (
        load_match.group("contest") != expected_contest_id
        or load_match.group("task") != expected_task_id
    ):
        raise SubmissionError("contest page identifiers do not match the requested task")

    index_data = urllib.parse.urlencode(
        {
            "assignID": load_match.group("ajax_assign"),
            "contestID": expected_contest_id,
            "taskID": expected_task_id,
        }
    ).encode("ascii")
    index_page = _decode_page(
        _request(
            opener,
            urllib.parse.urljoin(ORIGIN, load_match.group("path")),
            label="contest assignment index",
            data=index_data,
            headers={
                "Content-Type": "application/x-www-form-urlencoded",
                "Origin": ORIGIN,
                "Referer": contest_url,
                "X-Requested-With": "XMLHttpRequest",
            },
        )
    )
    index_page = html.unescape(index_page)
    program_match = _require_match(
        r"(?P<path>/assignment/programOJPList\.jsp\?proNum=\d+&assignID=(?P<assign>\d+))",
        index_page,
        "judge program page",
    )
    assign_id = program_match.group("assign")
    program_url = urllib.parse.urljoin(ORIGIN, program_match.group("path"))
    program_page = html.unescape(
        _decode_page(_request(opener, program_url, label="judge program page"))
    )

    upload_match = _require_match(
        r"showOJPProcessMsg\.jsp\?problemID=(?P<problem>\d+)"
        r"&assignID=(?P<assign>\d+)&doSubmit=true",
        program_page,
        "judge upload endpoint",
    )
    if upload_match.group("assign") != assign_id:
        raise SubmissionError("judge page returned inconsistent assignment identifiers")
    problem_id = upload_match.group("problem")
    size_match = _require_match(
        r"var\s+maxFileSize\s*=\s*(?P<size>\d+)",
        program_page,
        "submission size limit",
    )
    max_file_size = int(size_match.group("size"))

    base_status_url = urllib.parse.urljoin(
        program_url,
        f"showOJPProcessMsg.jsp?problemID={problem_id}&assignID={assign_id}",
    )
    status_page = html.unescape(
        _decode_page(_request(opener, base_status_url, label="judge status page"))
    )
    status_match = _require_match(
        r"(?P<path>/assignment/showOJPProcessJSON\.jsp\?"
        r"assignID=\d+&problemID=\d+&userID=[^'\"\s<]+)",
        status_page,
        "judge result endpoint",
    )

    common_query = f"problemID={problem_id}&assignID={assign_id}"
    return SubmitContract(
        contest_id=expected_contest_id,
        task_id=expected_task_id,
        assign_id=assign_id,
        problem_id=problem_id,
        max_file_size=max_file_size,
        upload_url=urllib.parse.urljoin(
            program_url,
            f"showOJPProcessMsg.jsp?{common_query}&doSubmit=true&wtime=10",
        ),
        refresh_url=urllib.parse.urljoin(
            program_url,
            f"showOJPProcessMsg.jsp?{common_query}&doNewSubmit=true",
        ),
        status_url=urllib.parse.urljoin(ORIGIN, status_match.group("path")),
    )


def _parse_status(payload: bytes) -> JudgeStatus:
    try:
        records = json.loads(_decode_page(payload).strip())
        ret = str(records[0]["ret"])
        content = str(records[1].get("content", ""))
    except (IndexError, KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
        raise SubmissionError("judge result endpoint returned an unsupported payload") from exc

    extractor = _TextExtractor()
    extractor.feed(content)
    text = extractor.text()[:MAX_RESULT_TEXT]
    submission_match = re.search(r"最后一次提交时间[：:]\s*([^\n]+)", text)
    score_match = re.search(r"得分[：:]\s*([^\n]+)", text)
    verdict_match = re.search(
        r"<p(?:\s+[^>]*)?>\s*(AC|WA|CE|RE|TLE|MLE|OLE|PE|SE)\s*</p>",
        content,
        re.IGNORECASE,
    )
    metrics: dict[str, str] = {}
    for key in SUMMARY_KEYS:
        value_match = re.search(rf"(?:^|\n){re.escape(key)}=([^\n]+)", text)
        if value_match:
            metrics[key] = value_match.group(1).strip()
    return JudgeStatus(
        ret=ret,
        content_hash=hashlib.sha256(content.encode("utf-8")).hexdigest(),
        submission_time=submission_match.group(1).strip() if submission_match else None,
        display_score=score_match.group(1).strip() if score_match else None,
        verdict=verdict_match.group(1).upper() if verdict_match else None,
        metrics=metrics,
        text=text,
    )


def fetch_status(
    opener: urllib.request.OpenerDirector, contract: SubmitContract
) -> JudgeStatus:
    return _parse_status(_request(opener, contract.status_url, label="judge result"))


def _validate_bundle(path: Path, max_file_size: int) -> dict[str, object]:
    if not path.is_file():
        raise SubmissionError(f"submission bundle does not exist: {path}")
    if path.suffix.lower() != ".zip":
        raise SubmissionError("submission automation accepts only a .zip bundle")
    if not re.fullmatch(r"[A-Za-z0-9._-]+", path.name):
        raise SubmissionError(
            "submission filename must contain only ASCII letters, digits, ._-"
        )
    size = path.stat().st_size
    if size <= 0 or size > max_file_size:
        raise SubmissionError(
            f"submission size {size} is outside the platform limit {max_file_size}"
        )
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    try:
        with zipfile.ZipFile(path) as archive:
            names = archive.namelist()
            bad_member = archive.testzip()
            if bad_member is not None:
                raise SubmissionError(f"submission zip has a corrupt member: {bad_member}")
            normalized: set[str] = set()
            for name in names:
                member = PurePosixPath(name)
                if member.is_absolute() or ".." in member.parts:
                    raise SubmissionError("submission zip contains an unsafe path")
                normalized.add(member.as_posix().rstrip("/"))
    except zipfile.BadZipFile as exc:
        raise SubmissionError("submission bundle is not a valid zip file") from exc
    required = {"CMakeLists.txt", "pass/dag_pass.cpp", "runtime/dag_runtime.cpp"}
    missing = sorted(required - normalized)
    if missing:
        raise SubmissionError(
            "submission zip is missing required root files: " + ", ".join(missing)
        )
    forbidden = [
        name
        for name in normalized
        if name.startswith(".git/")
        or name.startswith("build/")
        or "cookie" in PurePosixPath(name).name.lower()
    ]
    if forbidden:
        raise SubmissionError("submission zip contains repository or credential artifacts")
    return {"filename": path.name, "size_bytes": size, "sha256": digest}


def _multipart_body(path: Path) -> tuple[bytes, str]:
    boundary = f"----BishengContest{uuid.uuid4().hex}"
    disposition = (
        f'Content-Disposition: form-data; name="file"; filename="{path.name}"\r\n'
    )
    prefix = (
        f"--{boundary}\r\n"
        + disposition
        + "Content-Type: application/octet-stream\r\n\r\n"
    ).encode("ascii")
    suffix = f"\r\n--{boundary}--\r\n".encode("ascii")
    return prefix + path.read_bytes() + suffix, boundary


def _save_result(
    path: Path | None,
    *,
    contract: SubmitContract,
    status: JudgeStatus,
    bundle: dict[str, object] | None = None,
    state: str = "complete",
) -> None:
    if path is None:
        return
    payload: dict[str, object] = {
        "state": state,
        "contract": contract.public_dict(),
        "judge": asdict(status),
    }
    if bundle is not None:
        payload["bundle"] = bundle
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    os.chmod(path, 0o600)


def submit_and_wait(
    opener: urllib.request.OpenerDirector,
    contract: SubmitContract,
    bundle_path: Path,
    *,
    expected_sha256: str | None,
    timeout: float,
    poll_interval: float,
    result_file: Path | None,
) -> int:
    bundle = _validate_bundle(bundle_path, contract.max_file_size)
    if expected_sha256 and bundle["sha256"] != expected_sha256.lower():
        raise SubmissionError("submission SHA-256 does not match --expect-sha256")
    baseline = fetch_status(opener, contract)
    print(
        json.dumps(
            {
                "submission_preview": {
                    **contract.public_dict(),
                    **bundle,
                    "baseline_submission_time": baseline.submission_time,
                }
            },
            ensure_ascii=False,
        ),
        flush=True,
    )

    body, boundary = _multipart_body(bundle_path)
    response = _request(
        opener,
        contract.upload_url,
        label="submission upload",
        data=body,
        headers={
            "Accept": "*/*",
            "Content-Type": f"multipart/form-data; boundary={boundary}",
            "Origin": ORIGIN,
            "Referer": ORIGIN + "/pages/contest/contest_submit.jsp",
            "X-Requested-With": "XMLHttpRequest",
        },
        timeout=120,
    )
    response_text = _decode_page(response)
    if re.search(r"请.*登录|loginproc\.jsp|上传失败|文件格式不对", response_text):
        raise SubmissionError("submission upload response indicates rejection")
    _request(opener, contract.refresh_url, label="new submission status")
    print("submission_upload=accepted", flush=True)

    deadline = time.monotonic() + timeout
    next_progress = 0.0
    latest = baseline
    while time.monotonic() < deadline:
        time.sleep(poll_interval)
        latest = fetch_status(opener, contract)
        changed = latest.content_hash != baseline.content_hash
        if changed and latest.ret == "1" and latest.verdict in TERMINAL_VERDICTS:
            _save_result(
                result_file,
                contract=contract,
                status=latest,
                bundle=bundle,
            )
            print(json.dumps({"judge_result": latest.summary_dict()}, ensure_ascii=False))
            return 0 if latest.verdict == "AC" else 1
        elapsed = timeout - max(0.0, deadline - time.monotonic())
        if elapsed >= next_progress:
            print(
                f"judge_state=waiting elapsed_seconds={int(elapsed)} "
                f"changed={'yes' if changed else 'no'}",
                flush=True,
            )
            next_progress = elapsed + 30

    _save_result(
        result_file,
        contract=contract,
        status=latest,
        bundle=bundle,
        state="timeout",
    )
    raise SubmissionError("judge polling timed out before a new terminal result")


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Discover, inspect, or explicitly submit to CourseGrading."
    )
    parser.add_argument("--contest-url", required=True, help="authenticated contest task URL")
    parser.add_argument(
        "--cookie-file", required=True, type=Path, help="0600 Netscape cookie file"
    )
    subparsers = parser.add_subparsers(dest="action", required=True)
    subparsers.add_parser("discover", help="discover safe public interface metadata")

    status_parser = subparsers.add_parser("status", help="read the latest judge result")
    status_parser.add_argument("--result-file", type=Path)

    submit_parser = subparsers.add_parser(
        "submit", help="perform one authorized upload and wait for its result"
    )
    submit_parser.add_argument("--bundle", required=True, type=Path)
    submit_parser.add_argument("--expect-sha256")
    submit_parser.add_argument("--timeout", type=float, default=7200)
    submit_parser.add_argument("--poll-interval", type=float, default=10)
    submit_parser.add_argument("--result-file", type=Path)
    return parser


def main() -> int:
    args = _build_parser().parse_args()
    if getattr(args, "timeout", 1) <= 0:
        raise SubmissionError("--timeout must be positive")
    if getattr(args, "poll_interval", 2) < 2:
        raise SubmissionError("--poll-interval must be at least 2 seconds")
    jar = _load_cookie_jar(args.cookie_file)
    opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
    contract = discover_contract(opener, args.contest_url)
    if args.action == "discover":
        print(json.dumps({"submit_contract": contract.public_dict()}, ensure_ascii=False))
        return 0
    if args.action == "status":
        status = fetch_status(opener, contract)
        _save_result(args.result_file, contract=contract, status=status)
        print(json.dumps({"judge_result": status.summary_dict()}, ensure_ascii=False))
        return 0
    return submit_and_wait(
        opener,
        contract,
        args.bundle,
        expected_sha256=args.expect_sha256,
        timeout=args.timeout,
        poll_interval=args.poll_interval,
        result_file=args.result_file,
    )


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except SubmissionError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
