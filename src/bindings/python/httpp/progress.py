"""tqdm-like terminal progress: `bar` and `range`, mirroring httpp::progress in C++.

    for i in httpp.progress.range(100, "working"):
        ...

    bar = httpp.progress.bar(total, "downloading")
    bar.update(n)           # or bar.set_progress(absolute); bar.finish()

Drawn on stderr, so redirecting a program's output stays clean.
"""

import builtins

from .httpp_cy import Bar as bar


class range:  # shadows the builtin inside this module only; builtins.range is used below
    """Iterable 0 .. total-1 that owns a `bar` and advances it by one after every step
    (C++: httpp::progress::range). Single use, like the C++ one."""

    def __init__(self, total, description=""):
        self._total = int(total)
        self._bar = bar(self._total, description)

    def __len__(self):
        return self._total

    def __iter__(self):
        for i in builtins.range(self._total):
            yield i
            self._bar.update()


__all__ = ["bar", "range"]
