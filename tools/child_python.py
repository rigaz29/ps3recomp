"""The command that starts a tool's Python child: this Python, in UTF-8 mode when this one is.

-X utf8 is not passed down to a child. The Kit runs its tools under it, and the SPU lifter they started without it ran
in the locale's code page: on a PC whose code page is not UTF-8 it printed a player's folder with an accented letter
in it (a Windows user named with one) in that code page, the tool read what it printed as UTF-8, and the build
stopped there (issue #41). The Kit's gowa.py gives its own children the flag the same way (same_python, issue #32)."""
import sys


def python_cmd(*args):
    """[this Python, -X utf8 when this one runs in UTF-8 mode, then args], each as a string."""
    cmd = [sys.executable]
    if sys.flags.utf8_mode:
        cmd += ["-X", "utf8"]
    return cmd + [str(a) for a in args]
