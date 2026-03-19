"""fdb — fastdb4py CLI entry point."""

import argparse
import sys


def _run_codegen(args):
    from fastdb4py.codegen import run_codegen_ts
    run_codegen_ts(args.input_dir, args.output_dir)


def main():
    parser = argparse.ArgumentParser(
        prog='fdb',
        description='fastdb4py command-line tools',
    )
    subparsers = parser.add_subparsers(dest='command', metavar='<command>')
    subparsers.required = True

    # codegen subcommand
    codegen_parser = subparsers.add_parser(
        'codegen',
        help='Generate feature class bindings for other languages',
    )
    lang_group = codegen_parser.add_mutually_exclusive_group(required=True)
    lang_group.add_argument(
        '--ts',
        action='store_true',
        help='Generate TypeScript output',
    )
    codegen_parser.add_argument('input_dir', help='Directory of Python feature definition files')
    codegen_parser.add_argument('output_dir', help='Directory to write generated files')
    codegen_parser.set_defaults(func=_run_codegen)

    args = parser.parse_args()
    try:
        args.func(args)
    except Exception as e:
        print(f'fdb: error: {e}', file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
