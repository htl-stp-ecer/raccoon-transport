"""Subprocess manager for C++ interop helper binaries."""

import json
import os
import queue
import signal
import subprocess
import threading


BUILD_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "build", "cpp", "tools")


class CppHelper:
    """Wraps a C++ interop binary subprocess with JSON-line event reading."""

    def __init__(self, script: str, args: list[str]):
        binary = os.path.join(BUILD_DIR, script)
        if not os.path.isfile(binary):
            raise FileNotFoundError(
                f"C++ interop binary not found: {binary}\n"
                "Build with: cd build && cmake .. && cmake --build . -j$(nproc)"
            )
        cmd = [binary, *args]
        self._proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            stdin=subprocess.PIPE,
        )
        self._events: queue.Queue[dict] = queue.Queue()
        self._reader_thread = threading.Thread(
            target=self._read_stdout, daemon=True
        )
        self._reader_thread.start()

    def _read_stdout(self):
        assert self._proc.stdout is not None
        for raw_line in self._proc.stdout:
            line = raw_line.decode("utf-8").strip()
            if not line:
                continue
            try:
                event = json.loads(line)
                self._events.put(event)
            except json.JSONDecodeError:
                pass

    def wait_for_event(self, name: str, timeout: float = 5.0) -> dict:
        """Block until a JSON event with the given name appears."""
        deadline = timeout
        while True:
            try:
                event = self._events.get(timeout=deadline)
            except queue.Empty:
                raise TimeoutError(
                    f"Timed out waiting for event '{name}' "
                    f"(timeout={timeout}s)"
                )
            if event.get("event") == name:
                return event

    def read_events(self, count: int, timeout: float = 5.0) -> list[dict]:
        """Collect count events within timeout."""
        events = []
        for _ in range(count):
            try:
                event = self._events.get(timeout=timeout)
                events.append(event)
            except queue.Empty:
                raise TimeoutError(
                    f"Timed out reading events "
                    f"(got {len(events)}/{count}, timeout={timeout}s)"
                )
        return events

    @property
    def returncode(self) -> int | None:
        return self._proc.poll()

    def wait(self, timeout: float = 10.0) -> int:
        """Wait for the process to exit and return the exit code."""
        return self._proc.wait(timeout=timeout)

    def stop(self):
        """Gracefully stop: close stdin, SIGTERM, then SIGKILL."""
        if self._proc.poll() is not None:
            return
        try:
            if self._proc.stdin:
                self._proc.stdin.close()
            self._proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass

        if self._proc.poll() is None:
            self._proc.send_signal(signal.SIGTERM)
            try:
                self._proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass

        if self._proc.poll() is None:
            self._proc.kill()
            self._proc.wait(timeout=2)

    @property
    def stderr_text(self) -> str:
        """Read any stderr output (non-blocking best-effort)."""
        if self._proc.stderr:
            try:
                return self._proc.stderr.read().decode("utf-8", errors="replace")
            except Exception:
                return ""
        return ""
