"""Command-line interface for httpp."""

import click
from httpp import hello_world, Calculator


@click.command()
@click.option('--calculate', '-c', default=None, type=str,
              help='Perform a calculation (e.g., "5 + 3")')
def main(calculate):
    """Simple command-line tool for httpp."""
    if calculate:
        # Simple calculation example
        calc = Calculator()
        # This is just a basic example - in a real app you'd want proper parsing
        print(f"Calculation result: {calculate} = {eval(calculate)}")  # NOQA
    else:
        print(hello_world())


if __name__ == "__main__":
    main()