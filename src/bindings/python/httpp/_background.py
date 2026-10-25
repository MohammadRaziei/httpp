"""BackgroundTasks: work that runs after the answer has been sent (FastAPI's name and shape).

It is one job in the end: the C++ `response::after_send`, which the server runs on a worker thread once the answer has
been written. A handler can set that job itself (`response.after_send = fn` on the injected `response`); BackgroundTasks
is the convenience of queueing several calls, run in order. A coroutine function task is run on the shared event loop and
waited for, so it never blocks the loop itself.
"""

import traceback


class BackgroundTasks:
    """Ask for it by annotation: `def handler(tasks: BackgroundTasks): tasks.add_task(send_mail, to)`.

    The tasks run after the response was written, in the order they were added, on a worker thread. A task that raises is
    reported on stderr and the next one still runs. If the handler itself raises, nothing runs."""

    __slots__ = ("_tasks",)

    def __init__(self):
        self._tasks = []

    def add_task(self, fn, *args, **kwargs):
        if not callable(fn):
            raise TypeError("add_task needs a callable")
        self._tasks.append((fn, args, kwargs))

    def __len__(self):
        return len(self._tasks)

    def __call__(self):  # what the server runs
        run_all(self._tasks)


def run_all(tasks):
    from .server import _call  # server imports this module, so only here
    for fn, args, kwargs in tasks:
        try:
            _call(fn, *args, **kwargs)  # waits for a coroutine on the shared loop
        except BaseException:
            traceback.print_exc()


class AfterSend:
    """The one callable handed to C++: the handler's own after_send first, then the queued tasks."""

    __slots__ = ("_first", "_tasks")

    def __init__(self, first, tasks):
        self._first, self._tasks = first, tasks

    def __call__(self):
        if self._first is not None:
            run_all([(self._first, (), {})])
        if self._tasks is not None:
            self._tasks()
