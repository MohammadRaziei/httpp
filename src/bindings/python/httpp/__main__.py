"""Command-line interface for httpp."""

import click

from httpp import Server


@click.group()
def main():
    """httpp: a lightweight HTTP client + server toolkit."""


@main.command()
@click.argument("directory", default=".", type=click.Path(exists=True, file_okay=False))
@click.option("--host", default="0.0.0.0", show_default=True)
@click.option("--port", "-p", default=8000, show_default=True, type=int)
def server(directory, host, port):
    """Serve DIRECTORY over HTTP, like `python -m http.server`."""
    srv = Server()
    srv.serve_directory("/", directory)
    click.echo(f"Serving {directory!r} at http://{host}:{port}/")
    try:
        srv.listen(host, port)
    except KeyboardInterrupt:
        srv.stop()


@main.command()
@click.argument("url")
def download(url):
    """Download URL to stdout (a small libcurl-CLI-style replacement).

    URL parsing happens entirely in the C++ layer (httpp::client::fetch,
    backed by the vendored liburlparser) — nothing is parsed manually here.
    """
    from httpp import Client

    res = Client.fetch(url)
    if not res.ok:
        raise click.ClickException(f"request failed with status {res.status}")
    click.echo(res.body)


if __name__ == "__main__":
    main()
