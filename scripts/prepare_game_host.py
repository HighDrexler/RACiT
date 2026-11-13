#!/usr/bin/env python3
"""Prepare version-selected host sources from the shared game implementation."""
import argparse
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from game_version import UPDATE, rows, identify_profile


def rewrite_host(source, functions, addresses):
    # Preserve strings and comments while replacing actual C++ source tokens.
    token = re.compile(r'("(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/)|(\bfunc_([0-9A-F]{8})\b)|(0x[0-9A-Fa-f]+[uUlL]*)', re.S)
    def replace(match):
        if match.group(1):
            return match.group(0)
        if match.group(3):
            old = match.group(3)
            if old not in functions:
                raise ValueError('Missing host function mapping ' + old)
            return 'func_' + functions[old]
        number = re.sub('[uUlL]+$', '', match.group(4))
        address = int(number, 16)
        return 'racit_addr_%08X' % address if address in addresses else match.group(0)
    return token.sub(replace, source)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf')
    parser.add_argument('output')
    parser.add_argument('--profile', default='auto')
    options = parser.parse_args()
    profile = identify_profile(options.elf, options.profile, require_buildable=False)
    update = profile['id'] == UPDATE
    functions, addresses = {}, {}
    if update:
        functions = {row['disc_function']: row['update120_function']
                     for row in rows(UPDATE + '-host-functions.tsv')}
        for row in rows(UPDATE + '-host-bindings.tsv'):
            old, new = int(row['disc_address'], 16), int(row['update120_address'], 16)
            if old in addresses and addresses[old] != new:
                raise ValueError('Conflicting host address binding')
            addresses[old] = new
    elif profile['id'] != 'BCUS98124-1.00':
        raise ValueError('No host source implementation for ' + profile['id'])
    header = '#pragma once\n#include <stdint.h>\n'
    header += '#define RACACIT_GAME_PROFILE_ID "%s"\n' % profile['id']
    for old, new in sorted(addresses.items()):
        header += 'static constexpr uint32_t racit_addr_%08X = 0x%08Xu;\n' % (old, new)
    prepared = {'game_version.h': header}
    for name in ('main.cpp', 'stubs.cpp'):
        source = (ROOT / 'game' / name).read_text(encoding='utf-8')
        if update:
            source = rewrite_host(source, functions, addresses)
            if name == 'stubs.cpp':
                start = source.index('extern "C" void racacit_skin_armor_selected(')
                end = source.index("/* EquipSkin's value", start)
                source = source[:start] + (ROOT / 'versions' / (UPDATE + '-skin.inc')).read_text(encoding='utf-8') + source[end:]
        source = source.replace('"../tools/ps3recomp/runtime/ppu/ppu_context.h"', '"ppu_context.h"')
        prepared[name] = '#include "game_version.h"\n' + source
    output = Path(options.output)
    output.mkdir(parents=True, exist_ok=True)
    for name, source in prepared.items():
        target = output / name
        contents = source.encode('utf-8')
        try:
            if target.read_bytes() == contents:
                continue
        except FileNotFoundError:
            pass
        temporary = output / (name + '.tmp')
        temporary.write_bytes(contents)
        os.replace(temporary, target)
    print('Prepared %s host sources in %s' % (profile['id'], output))


if __name__ == '__main__':
    main()
