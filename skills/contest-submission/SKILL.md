---
name: contest-submission
description: Validate, upload, and monitor this Bisheng project's final zip submission through the authenticated CourseGrading contest interface. Use when asked to inspect the submission API, read the current judge result, or perform an explicitly authorized contest submission. Keep this separate from cloud-desktop test uploads.
---

# Contest Submission

Treat a formal judge submission as a separate external write. Discovering the
interface and reading status are read-only. Run the `submit` action only when
the user explicitly authorizes a new submission in the current conversation.
One authorization permits one upload; never retry an ambiguous upload.

## Persistent local configuration

Keep reusable submission inputs outside the repository under
`~/.config/bisheng-contest/` by default:

- `contest_url.txt`: exact CourseGrading task URL, mode `0600`.
- `course.cookies.txt`: user-exported Netscape cookie file, mode `0600`.

Keep the directory at mode `0700`. Never copy either file into the repository,
build bundle, logs, VM, or remote benchmark host. Override the directory only
with the absolute `BISHENG_CONTEST_CONFIG_DIR` path or `--config-dir`; the client
rejects configuration paths inside the repository.

When the user supplies a task URL, validate, normalize, and save it with:

```bash
python3 scripts/contest_submit.py configure '<contest-task-url>'
```

When the user supplies a fresh cookie export, place it at
`~/.config/bisheng-contest/course.cookies.txt` with mode `0600` without printing
its contents. The client automatically discovers both default files when the
corresponding command-line arguments are omitted. Explicit `--contest-url` and
`--cookie-file` arguments remain available as one-run overrides.

Treat the configured cookie as persistent user configuration: do not delete it
after a successful run. If authentication fails, stop before upload and ask the
user to replace that file; do not extract cookies from browser storage or reuse
a cloud-desktop credential file.

## Prepare and validate

Use the `compiler-contest-assistant` workflow first: build, run the verifier,
benchmark, package, and recreate the zip in a clean directory. Stop on any
failure. The archive root must contain `CMakeLists.txt`, `pass/dag_pass.cpp`,
and `runtime/dag_runtime.cpp`.

Require a user-supplied Netscape cookie file with mode `0600`. Never print its
contents, add it to Git, or place it inside a bundle. Require the exact contest
task URL; do not reuse a cloud-desktop URL. Prefer the persistent paths above so
later agents can discover them without asking the user to repeat valid inputs.

Inspect the dynamic endpoint without changing state:

```bash
python3 scripts/contest_submit.py discover
```

Read the current result with the `status` action. Save detailed output only
below ignored `build/judge_results/` unless the user requests a durable report.

## Submit once

Compute and show the zip SHA-256, size, source revision, smoke result, and
benchmark evidence before invoking the write action. Then run exactly once:

```bash
python3 scripts/contest_submit.py submit \
  --bundle /absolute/path/to/submission.zip \
  --expect-sha256 '<reviewed-sha256>' \
  --result-file build/judge_results/<label>.json
```

The client discovers `assignID`, `problemID`, the size limit, and the per-user
result URL from authenticated pages. It uploads multipart field `file`, records
the pre-submit result hash, and accepts only a changed terminal result. Do not
hard-code or log the per-user result URL.

If the request fails before a 2xx response, report failure. If a 2xx upload is
followed by a timeout, report the submission as ambiguous and do not retry.
Only report success when the new result is `AC`; preserve non-AC output for
diagnosis. A protocol-successful upload is not a correctness pass.

## Result handoff

Report the submitted revision and SHA-256, verdict, audit/performance pass
counts, geometric mean speedup, and total score. Do not commit cookies,
credentialized URLs, user IDs, temporary download links, or raw authenticated
HTML. Delete only explicitly temporary cookie copies after the run; retain the
standard persistent cookie file until the user replaces or revokes it.
