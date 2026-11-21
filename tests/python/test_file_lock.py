"""httpp.FileLock: a lock between processes on a file (ADR-0054)."""
import subprocess
import sys
import threading
import time
from pathlib import Path

import httpp


def test_holds_the_lock_directory_while_in_the_block_and_removes_it_after(tmp_path):
    target = tmp_path / "data.txt"
    lock_dir = Path(str(target) + ".lock")
    with httpp.FileLock(target) as lock:
        assert lock.owned is True
        assert lock.waited is False
        assert lock_dir.is_dir() and (lock_dir / "alive").exists()
        assert not target.exists()  # the file itself is never touched
    assert not lock_dir.exists()
    assert lock.owned is False  # let go


def test_it_takes_str_bytes_and_pathlike(tmp_path):
    for target in (str(tmp_path / "a"), str(tmp_path / "b").encode(), tmp_path / "c"):
        with httpp.FileLock(target) as lock:
            assert lock.owned


def test_acquire_and_release_by_hand_and_acquire_is_idempotent(tmp_path):
    lock = httpp.FileLock(tmp_path / "x")
    assert lock.owned is False
    lock.acquire()
    lock.acquire()
    assert lock.owned is True
    lock.release()
    assert lock.owned is False
    lock.release()  # twice is fine


def test_a_second_taker_waits_until_the_first_lets_go(tmp_path):
    target = tmp_path / "data.txt"
    first = httpp.FileLock(target).acquire()
    got = {}

    def other():
        with httpp.FileLock(target) as second:
            got["owned"], got["waited"] = second.owned, second.waited

    t = threading.Thread(target=other)
    t.start()
    time.sleep(0.4)
    assert got == {}  # still held, and the waiter does not hold the GIL while it waits
    first.release()
    t.join(10)
    assert got == {"owned": True, "waited": True}


def test_read_modify_write_across_threads_loses_no_update(tmp_path):
    target = tmp_path / "count.txt"
    target.write_text("0")

    def work():
        for _ in range(10):
            with httpp.FileLock(target, stale_after=5):
                n = int(target.read_text())
                target.write_text(str(n + 1))

    threads = [threading.Thread(target=work) for _ in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(60)
    assert int(target.read_text()) == 40


def test_read_modify_write_across_processes_loses_no_update(tmp_path):
    target = tmp_path / "count.txt"
    target.write_text("0")
    code = (
        "import sys, httpp\n"
        "from pathlib import Path\n"
        "p = Path(sys.argv[1])\n"
        "for _ in range(10):\n"
        "    with httpp.FileLock(p, stale_after=5):\n"
        "        p.write_text(str(int(p.read_text()) + 1))\n"
    )
    procs = [subprocess.Popen([sys.executable, "-c", code, str(target)]) for _ in range(3)]
    assert [p.wait(120) for p in procs] == [0, 0, 0]
    assert int(target.read_text()) == 30


def test_a_lock_whose_owner_died_is_taken_over_after_stale_after(tmp_path):
    target = tmp_path / "data.txt"
    dead = Path(str(target) + ".lock")
    dead.mkdir()
    (dead / "alive").write_text("999999 1\n")  # never changes again
    t0 = time.monotonic()
    with httpp.FileLock(target, stale_after=1.0) as lock:
        took = time.monotonic() - t0
        assert lock.owned and lock.waited
        assert 0.9 <= took < 5
    assert not dead.exists()


def test_it_goes_on_unlocked_when_the_lock_cannot_be_made(tmp_path):
    with httpp.FileLock(tmp_path / "no_such_dir" / "x" / "data.txt") as lock:
        assert lock.owned is False
        assert lock.waited is False
