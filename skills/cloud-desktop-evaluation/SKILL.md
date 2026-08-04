---
name: cloud-desktop-evaluation
description: Run this Bisheng compiler-contest project in a CourseGrading cloud desktop from a fresh authenticated `doexpDeskDocker.jsp` URL. Use when asked to prepare a cloud-desktop test bundle, upload it through the logged-in browser, execute the guarded smoke test in noVNC, or retrieve its structured result. Do not use for final contest submission.
---

# Cloud Desktop Evaluation

Use this skill only for the user-owned CourseGrading cloud desktop. Treat an
explicit request to run a test plus a freshly supplied authenticated desktop
URL as authorization to upload the generated test bundle and execute its
guarded command. A URL alone permits inspection only.

Never use this workflow for the contest's final submission form. Stop if a
CAPTCHA, a new permission prompt, an unexpected account/contest page, or an
unverifiable upload result appears.

## Prepare the bundle

From the repository root, run:

```bash
./scripts/prepare_cloud_desktop_bundle.sh <label>
```

The command creates a source-only zip below `build/cloud_desktop/`, validates
the 64 MiB upload limit, prints its SHA-256, and writes a matching
`remote-command.txt`. The bundle contains the current `submission/` and
`contestant_sdk/` sources plus `run.sh`; it does not include credentials or
host-built binaries.

Use the printed bundle path as the only upload candidate. Read
`docs/judge_automation.md` for the page contract when the browser UI changes.

## Upload through the session capability

Prefer the curl wrapper when both the fresh desktop URL and a matching,
explicitly user-supplied login cookie file are available; it avoids the
browser extension's local-file access restriction:

```bash
./scripts/upload_cloud_desktop_bundle.sh '<desktop-url>' '<bundle.zip>'
```

The wrapper extracts `cicdParam` in memory and sends the same
`multipart/form-data` field (`file`) as the page JavaScript. The endpoint also
requires the matching logged-in session. If the user has explicitly supplied a
Netscape cookie file, pass it only through `CLOUD_DESKTOP_COOKIE_FILE`; never
print, copy into the repository, or persist its contents. Treat redirects,
empty responses, and non-2xx responses as failure. Always verify the uploaded
filename inside `/mnt/cgshare` before execution.

Use the Chrome browser-control skill only to open the fresh desktop URL and
operate the noVNC terminal. Do not use this workflow for final contest upload.

## Execute and collect

The noVNC desktop is a Canvas. Use the page clipboard (`#clipboardText` then
`#clipboardBtn`) to transfer the exact contents of `remote-command.txt`. Focus
the maximized terminal once, send Ctrl+U to clear any partial command, use the
terminal's right-click Paste action, and send Enter. The command only deletes
its generated `cloud_eval_<label>` directory inside `/mnt/cgshare`, unpacks the
bundle with `python3 -m zipfile`, and invokes `run.sh`.

If the matching logged-in Netscape cookie file is available, the optional
`scripts/cloud_desktop_rfb.py` client can replace Canvas interaction with raw
RFB pointer, clipboard, and key events over websockify. Read
`docs/judge_automation.md` before using it. Do not claim that URL-only direct
RFB works: the endpoint timed out without the matching browser session.
With the matching cookie, the WebSocket upgrade, RFB authentication, command
injection, and uniquely marked ServerCutText return path have been verified.

`run.sh` always writes:

```text
/mnt/cgshare/cloud_eval_<label>/result.env
/mnt/cgshare/cloud_eval_<label>/test.log
```

Read `result.env` first through the remote clipboard, workspace file browser,
or download dialog. Use `test.log` only for diagnosis. Report the bundle SHA,
exit code, result status, and a bounded log tail. Never claim a successful test
unless `result.env` says `status=passed` and `exit_code=0`.

Before running the smoke command, `run.sh` verifies cmake, ninja, and the
required `/opt/bisheng` LLVM tools. Missing tools must produce
`status=setup_error` and exit code 127. The cloud image tested on 2026-08-05
lacked those prerequisites; do not misreport protocol success as test success.

The remote image has neither `xclip` nor `xsel`. To retrieve a short structured
result without OCR, run `clear; cat <result>; printf <end-marker>`, then operate
the maximized XFCE Terminal menu relative to the Canvas: Edit, Select All, Edit,
Copy. Read the resulting page `#clipboardText` (browser path) or ServerCutText
(raw RFB path). This result-copy path was verified with a marker probe.
