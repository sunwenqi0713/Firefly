#!/usr/bin/env python3
"""Run two interop helper processes concurrently and check success markers.

Usage:
  run_interop_pair.py <helper> <mode_a> <mode_b> <marker_a> <marker_b>
                      [<mode_b_ready_marker>]

With a ready marker, process B starts first and process A starts only after B
prints it. This is the cross-process barrier used when B must install its
discovery Watch before A registers a writer.

Discovery REP/PUB addresses come from FIREFLY_DISCOVERY_*_URL supplied by CTest;
each CTest case has its own port pair and can run in parallel without a lock.
"""

import subprocess
import sys
import threading


def main() -> int:
    if len(sys.argv) not in (6, 7):
        print("run_interop_pair: bad arguments", file=sys.stderr)
        return 2
    helper, mode_a, mode_b, marker_a, marker_b = sys.argv[1:6]
    ready_marker = sys.argv[6] if len(sys.argv) == 7 else None
    modes = [mode_a, mode_b]
    markers = [marker_a, marker_b]
    outputs = [bytearray(), bytearray()]
    ready_event = threading.Event()

    def start(index: int):
        proc = subprocess.Popen(
            [helper, modes[index]],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

        def drain() -> None:
            assert proc.stdout is not None
            for line in iter(proc.stdout.readline, b""):
                outputs[index].extend(line)
                if ready_marker and index == 1 and ready_marker.encode() in line:
                    ready_event.set()
            proc.wait()

        thread = threading.Thread(target=drain)
        thread.start()
        return proc, thread

    threads = []
    started_b, thread_b = start(1)
    procs = [None, started_b]
    threads.append(thread_b)
    if ready_marker:
        if not ready_event.wait(60):
            print("run_interop_pair: {} never became ready".format(mode_b),
                  file=sys.stderr)
            for proc in procs:
                if proc is not None and proc.poll() is None:
                    proc.kill()
            for thread in threads:
                thread.join()
            return 1
    started_a, thread_a = start(0)
    procs[0] = started_a
    threads.append(thread_a)

    for thread in threads:
        thread.join()

    failed = False
    for index, proc in enumerate(procs):
        assert proc is not None
        text = outputs[index].decode("utf-8", errors="replace")
        sys.stdout.write(text)
        if proc.returncode != 0:
            print("run_interop_pair: {} exited with {}".format(
                modes[index], proc.returncode), file=sys.stderr)
            failed = True
        if markers[index] not in text:
            print("run_interop_pair: {} missing marker {}".format(
                modes[index], markers[index]), file=sys.stderr)
            failed = True
    if failed:
        return 1
    print("run_interop_pair: {} + {} OK".format(mode_a, mode_b))
    return 0


if __name__ == "__main__":
    sys.exit(main())
