"""Command-line interface for httpp. Stdlib only (argparse) — no
third-party dependencies, matching the rest of the project."""

import argparse
import shutil
import sys
from pathlib import Path


def _cmd_server(args):
    from httpp import Server

    srv = Server()
    srv.serve_directory("/", args.directory)
    print(f"Serving {args.directory!r} at http://{args.host}:{args.port}/")
    try:
        srv.listen(args.host, args.port)
    except KeyboardInterrupt:
        srv.stop()


def _cmd_download(args):
    if args.output:
        # Real download to a file, with a terminal progress bar (see
        # httpp::download_file / httpp::progress::bar) — nothing here
        # reimplements the request or the progress logic, it's all in C++.
        from httpp import DownloadFile

        result = DownloadFile(args.url).output(args.output).enable_progress(not args.quiet).run()
        if not result.ok:
            print(f"error: {result.error}", file=sys.stderr)
            return 1
        return 0

    # No destination given: behave like `curl URL` with no -o — print the
    # body to stdout. URL parsing happens entirely in the C++ layer
    # (httpp::client::fetch, backed by the vendored liburlparser).
    from httpp import Client

    res = Client.fetch(args.url)
    if not res.ok:
        print(f"error: request failed with status {res.status}", file=sys.stderr)
        return 1
    print(res.body)
    return 0


def _sdk_base(args):
    if args.prefix:
        return Path(args.prefix)
    if args.user:
        return Path.home() / ".local"
    return Path("/usr/local")


def _cmd_install(args):
    import httpp as _httpp

    base = _sdk_base(args)
    target = base / "httpp"
    sources = {
        "include": _httpp.get_include_dir(),
        "lib": _httpp.get_lib_dir(),
        "cmake": _httpp.get_cmake_dir(),
    }
    try:
        for name, src in sources.items():
            shutil.copytree(src, target / name, dirs_exist_ok=True)
    except PermissionError:
        print(
            f"error: no permission to write to {target}\n"
            f"Try `httpp install --user`, or re-run with sudo.",
            file=sys.stderr,
        )
        return 1

    print(f"Installed httpp headers/lib/cmake config to {target}")
    print(f"To use: cmake -DCMAKE_PREFIX_PATH={base} ...  then find_package(httpp CONFIG)")
    return 0


def _cmd_uninstall(args):
    base = _sdk_base(args)
    target = base / "httpp"
    if not target.exists():
        print(f"nothing installed at {target}")
        return 0
    try:
        shutil.rmtree(target)
    except PermissionError:
        print(
            f"error: no permission to remove {target}\n"
            f"Try `httpp uninstall --user`, or re-run with sudo.",
            file=sys.stderr,
        )
        return 1
    print(f"Removed {target}")
    return 0


def build_parser():
    parser = argparse.ArgumentParser(
        prog="httpp", description="httpp: a lightweight HTTP client + server toolkit."
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_server = sub.add_parser("server", help="Serve a directory over HTTP, like `python -m http.server`.")
    p_server.add_argument("directory", nargs="?", default=".")
    p_server.add_argument("--host", default="0.0.0.0")
    p_server.add_argument("--port", "-p", type=int, default=8000)
    p_server.set_defaults(func=_cmd_server)

    p_download = sub.add_parser("download", help="Download a URL (to stdout, or a file with -o).")
    p_download.add_argument("url")
    p_download.add_argument("-o", "--output", default=None, help="Save to this file (shows a progress bar) instead of printing to stdout.")
    p_download.add_argument("-q", "--quiet", action="store_true", help="With -o, suppress the progress bar.")
    p_download.set_defaults(func=_cmd_download)

    sdk_parent = argparse.ArgumentParser(add_help=False)
    sdk_parent.add_argument("--prefix", default=None, help="Install location (parent of httpp/). Default: /usr/local.")
    sdk_parent.add_argument("--user", action="store_true", help="Use ~/.local instead of /usr/local.")

    p_install = sub.add_parser(
        "install", parents=[sdk_parent],
        help="Install the httpp C++ SDK (headers, lib, CMake config) onto the system.",
    )
    p_install.set_defaults(func=_cmd_install)

    p_uninstall = sub.add_parser(
        "uninstall", parents=[sdk_parent],
        help="Remove what `httpp install` copied.",
    )
    p_uninstall.set_defaults(func=_cmd_uninstall)

    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args) or 0


if __name__ == "__main__":
    sys.exit(main())
