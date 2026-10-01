"""Process ownership shared by the manual CEF integration runners."""

import os
import signal
import subprocess


# CTRL_BREAK_EVENT targets a process group and reaches the native shutdown handler.
PROCESS_CREATION_FLAGS = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0


def stop_process(process, timeout=15):
    """Request ordinary shutdown, then reap the child even if signaling or waiting fails."""
    try:
        if process.poll() is None:
            process.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGINT)
        return process.wait(timeout=timeout)
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()
