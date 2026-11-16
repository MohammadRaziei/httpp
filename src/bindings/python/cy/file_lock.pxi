# A lock between processes on a file: the Python face of httpp::file_lock (include/httpp/file_lock.hpp).

cdef class FileLock:
    """A lock on a file (or on anything named by a path) that holds between processes, not only threads:

        with httpp.FileLock("state.json") as lock:
            ...                      # read, change and write state.json (Path.write_text, or atomically in C++)

    Whoever creates the directory "<path>.lock" owns the lock; the owner keeps a heartbeat file in it. A waiter
    that sees the heartbeat unchanged for `stale_after` seconds takes the lock over (its owner died); a lock that keeps
    being updated is waited for as long as it lives. `owned` is False when the lock directory cannot be created (a
    read-only folder): the body then runs unlocked. `waited` is True when someone else held the lock meanwhile, so what
    it protects may have changed. Acquiring waits without the GIL. `path` is a str, bytes or os.PathLike.
    """
    cdef cpp_file_lock* _lock
    cdef bytes _target
    cdef long long _stale_ms

    def __cinit__(self, target, stale_after=15.0):
        self._lock = NULL
        self._target = _fs(target)
        self._stale_ms = <long long>(stale_after * 1000.0)

    def __dealloc__(self):
        self._release()

    cdef void _release(self):
        if self._lock is not NULL:
            del self._lock
            self._lock = NULL

    def acquire(self):
        """Wait until the lock is held (or cannot be made). A no-op when this object already holds it."""
        cdef string target = self._target
        cdef long long stale_ms = self._stale_ms
        if self._lock is NULL:
            with nogil:
                self._lock = new cpp_file_lock(target, cpp_milliseconds(stale_ms))
        return self

    def release(self):
        """Let go of the lock (the heartbeat stops and the lock directory is removed)."""
        self._release()

    @property
    def owned(self):
        return self._lock is not NULL and self._lock.owned()

    @property
    def waited(self):
        return self._lock is not NULL and self._lock.waited()

    def __enter__(self):
        return self.acquire()

    def __exit__(self, *exc):
        self._release()
        return False
