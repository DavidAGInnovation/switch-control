#!/usr/bin/env python3
"""Generate the native MCP tool descriptions from the canonical Python schemas."""
import argparse
import json
from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from switch_control.control import TOOLS

def generated():
    payload = json.dumps({'tools': TOOLS}, separators=(',', ':'))
    return '#pragma once\nstatic const char api_tools[]='+json.dumps(payload)+';\n'

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    target = ROOT/'firmware/usb-resident/source/api-tools.h'
    if args.check:
        if target.read_text() != generated():
            raise SystemExit('Native schemas differ; run scripts/generate_schemas.py')
        print(f'Native schemas match all {len(TOOLS)} tools.')
    else:
        target.write_text(generated())
