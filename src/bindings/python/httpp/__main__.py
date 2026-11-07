"""`python -m httpp` (ADR-0041): the same program as the `httpp` command, whatever the arguments.

`python -m httpp server|browse|download|curl|--version ...` starts the native executable, `python -m httpp py run|path ...` is the Python half
(`py_main` below, argparse, stdlib only). The routing is httpp_cli.main, shared with the console script, so there is one grammar."""

import argparse
import sys


def _workers_arg(text):
    """argparse type for --workers: a positive integer or 'auto'."""
    if text == "auto":
        return "auto"
    try:
        n = int(text)
    except ValueError:
        n = 0
    if n < 1:
        raise argparse.ArgumentTypeError(f"workers must be an integer >= 1 or 'auto', not {text!r}")
    return n


def _cmd_run(args):
    from httpp import run

    try:
        return run(args.app, args.host, args.port, workers=args.workers, reuse_port=args.reuse_port, log_level=args.log_level,
                   io_threads=args.io_threads, shutdown_timeout=args.shutdown_timeout,
                   reload=args.reload, reload_dirs=args.reload_dirs, reload_patterns=args.reload_patterns,
                   ssl_certfile=args.ssl_certfile, ssl_keyfile=args.ssl_keyfile, ssl_keyfile_password=args.ssl_keyfile_password)
    except (ValueError, TypeError, OSError) as e:
        print(f"httpp: error: {e}", file=sys.stderr)
        return 1


def _cmd_path(args):
    import httpp as _httpp

    printed = False
    if args.include_dir:
        print(_httpp.get_include_dir())
        printed = True
    if args.lib_dir:
        print(_httpp.get_lib_dir())
        printed = True
    if args.cmake_dir:
        print(_httpp.get_cmake_dir())
        printed = True
    if not printed:
        # no flag given: print all three, labeled
        print(f"include: {_httpp.get_include_dir()}")
        print(f"lib:     {_httpp.get_lib_dir()}")
        print(f"cmake:   {_httpp.get_cmake_dir()}")
    return 0


def build_parser():
    parser = argparse.ArgumentParser(
        prog="httpp py", description="httpp Python commands: run an app, print the SDK directories. "
        "The native commands (server, browse, download, curl) are `httpp server` etc., without `py`."
    )
    # Top-level path flags (nanobind/pybind11 style: `httpp py --cmake-dir`,
    # same info as `python -c "import httpp; print(httpp.get_cmake_dir())"`)
    # work standalone, without a subcommand.
    parser.add_argument("--cmake-dir", action="store_true", help="Print the directory containing httppConfig.cmake and exit.")
    parser.add_argument("--include-dir", action="store_true", help="Print the directory containing httpp.h and exit.")
    parser.add_argument("--lib-dir", action="store_true", help="Print the directory containing the compiled httpp library and exit.")

    sub = parser.add_subparsers(dest="command")

    p_run = sub.add_parser("run", help="Run an httpp app: `httpp py run main:app`.")
    p_run.add_argument("app", help="import string 'module:attribute' of an httpp.Server")
    p_run.add_argument("--host", default="127.0.0.1", help="default 127.0.0.1; use 0.0.0.0 to listen on every interface")
    p_run.add_argument("--port", "-p", type=int, default=8000)
    p_run.add_argument("--workers", "-w", type=_workers_arg, default=1, metavar="N|auto",
                       help="processes sharing the port (Linux only); gets past the GIL. "
                            "'auto' = one per usable CPU (default 1)")
    p_run.add_argument("--io-threads", type=int, default=None, metavar="N",
                       help="I/O threads sharing the port inside each process (default 1); on a free-threaded Python "
                            "each also gets its own asyncio loop")
    p_run.add_argument("--shutdown-timeout", type=float, default=None, metavar="SECONDS",
                       help="on Ctrl+C or SIGTERM, how long requests in progress may take to finish before they are cut "
                            "(default 10)")
    p_run.add_argument("--reload", action="store_true",
                       help="development: restart the app when a source file changes (one worker; polls the files)")
    p_run.add_argument("--reload-dir", dest="reload_dirs", action="append", default=None, metavar="DIR",
                       help="with --reload, a directory to watch (repeatable; default: the current directory)")
    p_run.add_argument("--reload-pattern", dest="reload_patterns", action="append", default=None, metavar="GLOB",
                       help="with --reload, a file name pattern to watch (repeatable; default: *.py)")
    p_run.add_argument("--ssl-certfile", default=None, metavar="FILE", help="serve https and wss: the certificate chain (PEM, leaf first)")
    p_run.add_argument("--ssl-keyfile", default=None, metavar="FILE", help="the private key (PEM) of --ssl-certfile")
    p_run.add_argument("--ssl-keyfile-password", default=None, metavar="PASSWORD", help="the password of an encrypted --ssl-keyfile")
    p_run.add_argument("--reuse-port", action="store_true", help="let other httpp processes share the port (Linux only)")
    p_run.add_argument("--log-level", choices=("trace", "debug", "info", "warning", "error", "off"), default=None)
    p_run.set_defaults(func=_cmd_run)

    # Same three flags as a subcommand too: `httpp py path --cmake-dir`.
    p_path = sub.add_parser("path", help="Print include/lib/cmake directories (see also the top-level --cmake-dir etc.).")
    p_path.add_argument("--cmake-dir", action="store_true")
    p_path.add_argument("--include-dir", action="store_true")
    p_path.add_argument("--lib-dir", action="store_true")
    p_path.set_defaults(func=_cmd_path)

    return parser


def py_main(argv=None):
    """The Python commands, the arguments after `py` (httpp_cli.main calls this)."""
    argv = sys.argv[1:] if argv is None else list(argv)
    parser = build_parser()
    args = parser.parse_args(argv)

    # Top-level path flags work with no subcommand: `httpp py --cmake-dir`.
    if getattr(args, "command", None) is None:
        if args.cmake_dir or args.include_dir or args.lib_dir:
            return _cmd_path(args)
        parser.print_help()
        return 1

    return args.func(args) or 0


def main(argv=None):
    """Same as the `httpp` command."""
    import httpp_cli

    return httpp_cli.main(argv)


if __name__ == "__main__":
    sys.exit(main())
