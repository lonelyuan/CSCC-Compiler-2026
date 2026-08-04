#!/usr/bin/env python3
"""Minimal RFB-over-WebSocket client for the CourseGrading cloud desktop.

The client intentionally supports only the protocol features needed by the
cloud evaluation workflow: VNC authentication, one pointer click, clipboard
transfer, key events, and ServerCutText result collection.  It does not render
the framebuffer and never prints the desktop token or VNC password.
"""

from __future__ import annotations

import argparse
import base64
import http.cookiejar
import re
import shlex
import struct
import sys
import time
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path

import websocket
from Crypto.Cipher import DES


RFB_VERSION = b"RFB 003.008\n"
RESULT_MARKER = "CLOUD_EVAL_RESULT"


class ProtocolError(RuntimeError):
    pass


@dataclass(frozen=True)
class DesktopCapability:
    websocket_url: str
    origin: str
    password: str
    cookie_header: str | None


def _b64decode_text(value: str) -> str:
    padded = value + "=" * (-len(value) % 4)
    return base64.urlsafe_b64decode(padded.encode("ascii")).decode("utf-8")


def _load_cookie_jar(path: Path | None) -> http.cookiejar.MozillaCookieJar:
    jar = http.cookiejar.MozillaCookieJar()
    if path is not None:
        if not path.is_file():
            raise ProtocolError(f"cookie file does not exist: {path}")
        jar.load(str(path), ignore_discard=True, ignore_expires=True)
    return jar


def _cookie_header(jar: http.cookiejar.CookieJar) -> str | None:
    values = [f"{cookie.name}={cookie.value}" for cookie in jar]
    return "; ".join(values) if values else None


def _find_desktop_param(url: str, html: str) -> str | None:
    query = urllib.parse.parse_qs(urllib.parse.urlsplit(url).query)
    value = (query.get("desktopParam") or [None])[0]
    if value:
        return value
    match = re.search(
        r"cgvncutil_get(?:token|pwd|host|port)\(['\"]([^'\"]+)['\"]\)",
        html,
    )
    return match.group(1) if match else None


def resolve_capability(url: str, cookie_file: Path | None) -> DesktopCapability:
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme != "https" or parsed.hostname != "course.educg.net":
        raise ProtocolError("desktop URL must use https://course.educg.net")
    if parsed.path != "/authincludes/expEnv/doexpDeskDocker.jsp":
        raise ProtocolError("desktop URL must target doexpDeskDocker.jsp")

    jar = _load_cookie_jar(cookie_file)
    desktop_param = (urllib.parse.parse_qs(parsed.query).get("desktopParam") or [None])[0]
    final_url = url
    if desktop_param is None:
        opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
        request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
        with opener.open(request, timeout=20) as response:
            html = response.read().decode("utf-8", "replace")
            final_url = response.geturl()
        desktop_param = _find_desktop_param(final_url, html)
    if desktop_param is None:
        raise ProtocolError(
            "desktopParam was not issued; provide a logged-in Netscape cookie file "
            "or a fresh URL that already contains desktopParam"
        )

    try:
        parts = _b64decode_text(desktop_param).split(":")
        token = _b64decode_text(parts[1])
        password = _b64decode_text(parts[2])
    except (IndexError, ValueError, UnicodeError) as exc:
        raise ProtocolError("desktopParam has an unsupported encoding") from exc

    page = urllib.parse.urlsplit(final_url)
    origin = f"{page.scheme}://{page.hostname}"
    ws_query = urllib.parse.urlencode({"token": token})
    websocket_url = f"wss://{page.hostname}/websockify?{ws_query}"
    return DesktopCapability(websocket_url, origin, password, _cookie_header(jar))


def _reverse_bits(byte: int) -> int:
    result = 0
    for _ in range(8):
        result = (result << 1) | (byte & 1)
        byte >>= 1
    return result


class RFBClient:
    def __init__(self, capability: DesktopCapability, timeout: float) -> None:
        self.capability = capability
        self.timeout = timeout
        self.ws: websocket.WebSocket | None = None
        self.buffer = bytearray()
        self.width = 0
        self.height = 0
        self.desktop_name = ""

    def __enter__(self) -> "RFBClient":
        self.ws = websocket.create_connection(
            self.capability.websocket_url,
            timeout=self.timeout,
            origin=self.capability.origin,
            subprotocols=["binary"],
            cookie=self.capability.cookie_header,
        )
        self._handshake()
        return self

    def __exit__(self, *_: object) -> None:
        if self.ws is not None:
            self.ws.close()

    def _recv_exact(self, length: int) -> bytes:
        if self.ws is None:
            raise ProtocolError("RFB client is not connected")
        while len(self.buffer) < length:
            message = self.ws.recv()
            if not isinstance(message, bytes):
                raise ProtocolError("websockify returned a non-binary message")
            self.buffer.extend(message)
        result = bytes(self.buffer[:length])
        del self.buffer[:length]
        return result

    def _send(self, payload: bytes) -> None:
        if self.ws is None:
            raise ProtocolError("RFB client is not connected")
        self.ws.send_binary(payload)

    def _handshake(self) -> None:
        server_version = self._recv_exact(12)
        if not server_version.startswith(b"RFB 003."):
            raise ProtocolError("server did not return an RFB banner")
        self._send(RFB_VERSION)

        count = self._recv_exact(1)[0]
        if count == 0:
            reason_length = struct.unpack(">I", self._recv_exact(4))[0]
            reason = self._recv_exact(reason_length).decode("utf-8", "replace")
            raise ProtocolError(f"RFB security negotiation failed: {reason}")
        security_types = self._recv_exact(count)
        if 2 in security_types:
            self._send(b"\x02")
            challenge = self._recv_exact(16)
            password = self.capability.password.encode("latin-1", "replace")[:8]
            key = bytes(_reverse_bits(byte) for byte in password.ljust(8, b"\0"))
            self._send(DES.new(key, DES.MODE_ECB).encrypt(challenge))
        elif 1 in security_types:
            self._send(b"\x01")
        else:
            offered = ",".join(str(value) for value in security_types)
            raise ProtocolError(f"server offered unsupported RFB security types: {offered}")

        security_result = struct.unpack(">I", self._recv_exact(4))[0]
        if security_result != 0:
            reason_length = struct.unpack(">I", self._recv_exact(4))[0]
            reason = self._recv_exact(reason_length).decode("utf-8", "replace")
            raise ProtocolError(f"VNC authentication failed: {reason}")

        self._send(b"\x01")  # Shared ClientInit.
        server_init = self._recv_exact(24)
        self.width, self.height = struct.unpack(">HH", server_init[:4])
        name_length = struct.unpack(">I", server_init[20:24])[0]
        self.desktop_name = self._recv_exact(name_length).decode("utf-8", "replace")

    def pointer_click(self, x: int, y: int) -> None:
        x = max(0, min(self.width - 1, x))
        y = max(0, min(self.height - 1, y))
        self._send(struct.pack(">BBHH", 5, 1, x, y))
        self._send(struct.pack(">BBHH", 5, 0, x, y))

    def key(self, keysym: int, down: bool) -> None:
        self._send(struct.pack(">BB2xI", 4, int(down), keysym))

    def chord(self, *keysyms: int) -> None:
        for keysym in keysyms:
            self.key(keysym, True)
        for keysym in reversed(keysyms):
            self.key(keysym, False)

    def clipboard(self, text: str) -> None:
        payload = text.encode("utf-8")
        self._send(struct.pack(">B3xI", 6, len(payload)) + payload)

    def run_terminal_command(self, command: str) -> None:
        if len(command.encode("utf-8")) > 4096:
            raise ProtocolError("terminal command exceeds the platform clipboard limit")
        self.pointer_click(self.width // 2, max(0, self.height - 48))
        self.chord(0xFFE3, ord("u"))  # Control_L + U clears a partial command.
        self.clipboard(command)
        time.sleep(0.25)
        self.chord(0xFFE1, 0xFF63)  # Shift_L + Insert pastes X11 clipboard.
        self.chord(0xFF0D)  # Return.

    def wait_for_clipboard(self, marker: str, timeout: float) -> str:
        if self.ws is None:
            raise ProtocolError("RFB client is not connected")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.ws.settimeout(max(0.1, min(5.0, deadline - time.monotonic())))
            try:
                message_type = self._recv_exact(1)[0]
            except websocket.WebSocketTimeoutException:
                continue
            if message_type == 2:  # Bell.
                continue
            if message_type == 3:  # ServerCutText.
                header = self._recv_exact(7)
                length = struct.unpack(">I", header[3:7])[0]
                text = self._recv_exact(length).decode("utf-8", "replace")
                if marker in text:
                    return text
                continue
            raise ProtocolError(
                f"unexpected RFB server message {message_type}; framebuffer rendering is disabled"
            )
        raise ProtocolError("timed out waiting for the structured result clipboard")

    def copy_terminal_all(self) -> None:
        """Use the maximized XFCE Terminal menu to select and copy its output."""
        self.pointer_click(57, 60)   # Edit menu.
        time.sleep(0.10)
        self.pointer_click(97, 184)  # Select All.
        time.sleep(0.10)
        self.pointer_click(57, 60)   # Edit menu again.
        time.sleep(0.10)
        self.pointer_click(87, 86)   # Copy.

    def collect_terminal_result(self, marker: str, timeout: float) -> str:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.copy_terminal_all()
            try:
                return self.wait_for_clipboard(
                    marker,
                    min(2.0, max(0.1, deadline - time.monotonic())),
                )
            except ProtocolError as exc:
                if "timed out waiting" not in str(exc):
                    raise
        raise ProtocolError("timed out waiting for the structured terminal result")


def _read_command(path: Path) -> str:
    lines = [line.strip() for line in path.read_text().splitlines() if line.strip()]
    if lines and lines[0] == "set -e":
        lines.pop(0)
    if not lines:
        raise ProtocolError(f"command file is empty: {path}")
    return " && ".join(lines)


def _with_result_output(command: str, result_file: str) -> str:
    result = shlex.quote(result_file)
    marker = shlex.quote(RESULT_MARKER)
    end_marker = shlex.quote(f"{RESULT_MARKER}_END")
    return (
        f"{{ {command}; }}; __cg_rc=$?; "
        "clear; "
        f"printf '%s\\n' {marker}; "
        f"cat {result} 2>&1; "
        f"printf '%s\\n' {end_marker}; "
        "unset __cg_rc"
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Probe or control the CourseGrading noVNC endpoint over RFB/WebSocket."
    )
    parser.add_argument("desktop_url", help="fresh doexpDeskDocker.jsp URL")
    parser.add_argument(
        "--cookie-file",
        type=Path,
        help="optional user-provided Netscape cookie file for the logged-in session",
    )
    parser.add_argument("--connect-timeout", type=float, default=20.0)
    subparsers = parser.add_subparsers(dest="action", required=True)
    subparsers.add_parser("probe", help="perform WebSocket and RFB authentication only")
    send = subparsers.add_parser("send", help="send one terminal command through raw RFB")
    send.add_argument("--command-file", type=Path, required=True)
    send.add_argument("--result-file", help="remote result file to return through X11 clipboard")
    send.add_argument("--result-timeout", type=float, default=900.0)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        capability = resolve_capability(args.desktop_url, args.cookie_file)
        with RFBClient(capability, args.connect_timeout) as client:
            print("websocket_upgrade=ok")
            print("rfb_authentication=ok")
            print(f"framebuffer_size={client.width}x{client.height}")
            if args.action == "probe":
                return 0
            command = _read_command(args.command_file)
            if args.result_file:
                command = _with_result_output(command, args.result_file)
            client.run_terminal_command(command)
            print("command_sent=yes")
            if args.result_file:
                result = client.collect_terminal_result(
                    f"{RESULT_MARKER}_END", args.result_timeout
                )
                print(result)
        return 0
    except (OSError, ProtocolError, websocket.WebSocketException) as exc:
        print(f"cloud_desktop_rfb_error={exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
