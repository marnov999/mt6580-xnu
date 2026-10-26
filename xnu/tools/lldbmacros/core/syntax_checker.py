#!/usr/bin/env python3

helpdoc = """
A simple utility that verifies the syntax for python scripts.
The checks it does are :
  * Check for 'tab' characters in .py files
  * Compile errors in py sources
Usage:
  python syntax_checker.py <python_source_file> [<python_source_file> ..] 
"""
import sys
import os
import re

tabs_search_rex = re.compile(r"^\s*\t+", re.MULTILINE | re.DOTALL)


def eprint(*args, **kwargs):
    print(*args, file=sys.stderr, **kwargs)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        eprint("Error: Unknown arguments")
        print(helpdoc)
        sys.exit(1)
    for fname in sys.argv[1:]:
        if not os.path.exists(fname):
            eprint("Error: Cannot recognize %s as a file" % fname)
            sys.exit(1)
        if fname.split('.')[-1] != 'py':
            print("Note: %s is not a valid python file. Skipping." % fname)
            continue
        with open(fname, 'r') as fh:
            strdata = fh.readlines()
        lineno = 0
        tab_check_status = True
        for linedata in strdata:
            lineno += 1
            if len(tabs_search_rex.findall(linedata)) > 0:
                eprint("Error: Found a TAB character at %s:%d" % (fname, lineno))
                tab_check_status = False
        if not tab_check_status:
            eprint("Error: Syntax check failed. Please fix the errors and try again.")
            sys.exit(1)
        # Now check for errors in compilation.
        try:
            with open(fname, 'r') as fh:
                compile(fh.read(), fname, 'exec')
        except SyntaxError as exc:
            eprint(str(exc))
            eprint("Error: Compilation failed. Please fix the errors and try again.")
            sys.exit(1)
        print("Success: Checked %s. No syntax errors found." % fname)
    sys.exit(0)
