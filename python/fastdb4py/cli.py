"""fdb — fastdb4py CLI entry point."""

import argparse
import sys


def _run_codegen(args):
    if args.ts:
        from fastdb4py.codegen import run_codegen_ts
        run_codegen_ts(args.input_path, args.output_path)
        return
    if args.c_two_ts:
        from fastdb4py.codegen import run_codegen_c_two_ts
        run_codegen_c_two_ts(args.input_path, args.output_path, args.schema)
        return
    raise ValueError('unknown codegen target')


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
    lang_group.add_argument(
        '--c-two-ts',
        action='store_true',
        help='Generate C-Two TypeScript fastdb codec helpers',
    )
    codegen_parser.add_argument(
        '--schema',
        action='append',
        default=[],
        help='fastdb.schema.v1 descriptor JSON file for --c-two-ts; repeatable',
    )
    codegen_parser.add_argument('input_path', help='Input path: feature directory for --ts, C-Two contract JSON for --c-two-ts')
    codegen_parser.add_argument('output_path', help='Output path: directory for --ts, TypeScript file for --c-two-ts')
    codegen_parser.set_defaults(func=_run_codegen)

    args = parser.parse_args()
    try:
        args.func(args)
    except Exception as e:
        print(f'fdb: error: {e}', file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
