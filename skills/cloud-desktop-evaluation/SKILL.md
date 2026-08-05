---
name: cloud-desktop-evaluation
description: Run and calibrate this Bisheng compiler-contest project in the authorized CourseGrading AArch64 cloud desktop. Use when asked to prepare or upload a cloud-desktop test bundle, bootstrap the private SSH path, sync source, execute verifier or performance tests through noVNC or SSH, retrieve structured results, or compare cloud measurements with formal judge results. Do not use for final contest submission.
---

# Cloud Desktop Evaluation

Use this skill only for the user-owned CourseGrading cloud desktop. Treat an
explicit request to run a test plus a freshly supplied authenticated desktop
URL as authorization to upload the generated test bundle and execute its
guarded command. A URL alone permits inspection only.

This project received committee permission to establish its own private remote
access path. That is a project-specific prerequisite, not a general platform
policy. Bootstrap it only when the user asks, and follow
`docs/cloud_desktop_remote_access.md`; never persist a Tailnet login URL, auth
key, node state, private key, personal public key, or Tailnet address in Git.

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

## Continue through authorized SSH

After the noVNC bootstrap has established the documented private SSH path,
prefer ordinary SSH for builds, long benchmarks, logs, and result collection.
The verified topology is userspace `tailscaled`, private Tailnet TCP Serve on
port 22, and a public-key-only `sshd` bound to `127.0.0.1:2222`. The container
has no `/dev/net/tun`; do not switch it to kernel TUN mode. Built-in Tailscale
SSH stalled before returning an SSH server banner in this environment, so keep
it disabled and use the TCP Serve topology.

Use `scripts/cloud_desktop_ssh.sh` for connectivity and commands, and
`scripts/sync_to_cloud_desktop.sh` for a source-only sync. The scripts default
to MagicDNS rather than a personal Tailnet IP. The base image lacks `rsync`, so
the sync wrapper automatically uses tar-over-SSH and refuses delete mode unless
remote rsync is available. Connection success is not
performance evidence: record the remote source revision, command, CPU affinity,
verifier result, and CSV. Avoid concurrent file transfers during timed runs. A
DERP-only path affects terminal/SCP latency, not the remote core-computation
timer.

Do not fall back to the retired 4-vCPU AArch64 VM. Use the CourseGrading cloud
desktop as the only active AArch64 build/verifier/correlation environment. Use
the separate Ubuntu x86_64 Xeon host only for same-host scheduler scaling, never
as an AArch64 or formal-environment substitute.

## Calibrate performance through SSH

Run calibration only after CMake, Ninja, and the complete AArch64 BiSheng LLVM
toolchain are available and `/opt/bisheng` resolves to that toolchain. Use this
artifact contract for every run:

- Record the local commit and dirty status; a commit ID alone does not identify
  a dirty synced tree.
- Record remote architecture, CPU topology, memory, affinity/governor, tool
  versions, exact command, environment overrides, case-spec hash, and wall-clock
  start/end times in a metadata file next to the result CSV.
- Run all 150 cases through the verifier before accepting any performance data.
- Use at least three repeats for a reported performance comparison. Stop source
  sync, downloads, and other remote work while timing.
- Retrieve metadata, verifier output, and raw CSV together; archive them under a
  unique label in `docs/benchmark_results/`.
- Compare against formal `150/150`, equal-weight per-case geomean `2.393844`,
  and `m_ideal=32.0` only when the cloud runner uses the same individual cases
  and aggregation semantics. The current full-range `smoke_test.sh` aggregate
  and four-suite `benchmark.sh` CSV are diagnostics, not proof of judge
  equivalence.

Treat any mismatch in source, inputs, correctness, timing boundary, aggregation,
affinity, or toolchain as a failed equivalence precondition. Report the two
measurements separately instead of inventing a correction factor.

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
`status=setup_error` and exit code 127. The unprovisioned cloud image tested on
2026-08-05 lacked those prerequisites, but it has sufficient persistent space
to provision them under `/mnt/cgshare`. Do not misreport upload, RFB, or SSH
success as build/test success.

The remote image has neither `xclip` nor `xsel`. To retrieve a short structured
result without OCR, run `clear; cat <result>; printf <end-marker>`, then operate
the maximized XFCE Terminal menu relative to the Canvas: Edit, Select All, Edit,
Copy. Read the resulting page `#clipboardText` (browser path) or ServerCutText
(raw RFB path). This result-copy path was verified with a marker probe.
