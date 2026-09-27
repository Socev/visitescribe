#!/usr/bin/env python3
"""Disposable VisiteScribe LAN throughput sink.

Receives benchmark POSTs from the M5, discards the payload, and reports both
server-side transfer speed and request metadata. No patient audio is stored.

Run from the repository root:
    py tools\\lan-benchmark\\server.py

Then trigger MENU > LAN TEST on the M5.
"""

from __future__ import annotations

import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class BenchmarkHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "VisiteScribeLANBench/1"

    def log_message(self, fmt: str, *args) -> None:
        print(
            f"{time.strftime('%H:%M:%S')} "
            f"{self.client_address[0]} "
            f"{fmt % args}",
            flush=True,
        )

    def _json(self, status: int, payload: dict) -> None:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()

    def do_GET(self) -> None:
        if self.path == "/health":
            self._json(
                200,
                {
                    "ok": True,
                    "service": "visitescribe-lan-benchmark",
                    "bind": (
                        f"{self.server.server_address[0]}:"
                        f"{self.server.server_address[1]}"
                    ),
                },
            )
            return
        self._json(404, {"ok": False, "error": "not_found"})

    def do_POST(self) -> None:
        if self.path != "/upload":
            self._json(404, {"ok": False, "error": "not_found"})
            return

        raw_length = self.headers.get("Content-Length")
        if raw_length is None:
            self._json(411, {"ok": False, "error": "content_length_required"})
            return

        try:
            expected = int(raw_length)
        except ValueError:
            self._json(400, {"ok": False, "error": "bad_content_length"})
            return

        label = self.headers.get("X-VisiteScribe-Label", "unknown")
        audio_seconds = self.headers.get("X-Equivalent-Audio-Seconds", "?")

        print(
            f"\n--- {label} ---\n"
            f"client        : {self.client_address[0]}:{self.client_address[1]}\n"
            f"bytes expected: {expected:,}\n"
            f"audio equiv   : {audio_seconds} s",
            flush=True,
        )

        received = 0
        started_ns = time.perf_counter_ns()
        last_report_ns = started_ns
        report_step = max(expected // 10, 1)
        next_report = report_step

        while received < expected:
            want = min(256 * 1024, expected - received)
            block = self.rfile.read(want)
            if not block:
                break
            received += len(block)

            if received >= next_report:
                now_ns = time.perf_counter_ns()
                elapsed = max((now_ns - started_ns) / 1e9, 1e-9)
                mib_s = received / 1024 / 1024 / elapsed
                mbit_s = received * 8 / 1_000_000 / elapsed
                print(
                    f"  {received * 100 / expected:5.1f}%  "
                    f"{mib_s:6.2f} MiB/s  {mbit_s:6.2f} Mbit/s",
                    flush=True,
                )
                while next_report <= received:
                    next_report += report_step
                last_report_ns = now_ns

        finished_ns = time.perf_counter_ns()
        elapsed_s = max((finished_ns - started_ns) / 1e9, 1e-9)
        mib_s = received / 1024 / 1024 / elapsed_s
        mbit_s = received * 8 / 1_000_000 / elapsed_s

        ok = received == expected
        print(
            f"received      : {received:,} / {expected:,} bytes\n"
            f"elapsed       : {elapsed_s:.3f} s\n"
            f"server rate   : {mib_s:.3f} MiB/s = {mbit_s:.3f} Mbit/s\n"
            f"result        : {'OK' if ok else 'SHORT READ'}\n",
            flush=True,
        )

        self._json(
            200 if ok else 400,
            {
                "ok": ok,
                "label": label,
                "bytes": received,
                "expected_bytes": expected,
                "elapsed_ms": round(elapsed_s * 1000),
                "mib_per_s": round(mib_s, 3),
                "mbit_per_s": round(mbit_s, 3),
            },
        )


class BenchmarkServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main() -> None:
    parser = argparse.ArgumentParser(
        description="VisiteScribe dummy LAN throughput receiver"
    )
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()

    server = BenchmarkServer((args.bind, args.port), BenchmarkHandler)
    print(
        "VisiteScribe LAN benchmark receiver\n"
        f"Listening on {args.bind}:{args.port}\n"
        "No payload is written to disk. Ctrl+C stops the server.\n",
        flush=True,
    )

    try:
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        print("\nStopping...", flush=True)
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
