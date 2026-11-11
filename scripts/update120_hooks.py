"""Fingerprint-bound PPU source hooks for BCUS98124 update 1.20."""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# This consumes source edits; executable bytes and generated game functions
# remain local to the person building their own copy.
def apply_update120_hooks(recomp, elf):
    import hashlib, json
    from pathlib import Path

    recipe_path = Path(ROOT) / 'versions' / 'BCUS98124-1.20-hooks.json'
    recipe = json.loads(recipe_path.read_text(encoding='utf-8'))
    if recipe.get('schema') != 1 or recipe.get('profile') != 'BCUS98124-1.20':
        raise ValueError('Unsupported update hook recipe')
    digest = hashlib.sha256()
    with open(elf, 'rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    if digest.hexdigest() != recipe['elf_sha256']:
        raise ValueError('Update hooks require the exact BCUS98124 1.20 executable')

    hooks = {row['function']: row for row in recipe['hooks']}
    if len(hooks) != len(recipe['hooks']):
        raise ValueError('Duplicate update hook address')
    seen = set()
    pending = {}
    preamble = recipe['preamble']
    for path in sorted(Path(recomp).glob('ppu_recomp_*.cpp')):
        source = path.read_text(encoding='utf-8')
        replacements = []
        for match in re.finditer(r'(?m)^void func_([0-9A-F]{8})\(ppu_context\* ctx\) \{', source):
            address = match.group(1)
            if address not in hooks:
                continue
            if address in seen:
                raise ValueError('Duplicate generated function ' + address)
            seen.add(address)
            end = source.find('\n}\n', match.end())
            if end < 0:
                raise ValueError('Unterminated generated function ' + address)
            end += 3
            before = source[match.start():end]
            hook = hooks[address]
            checksum = hashlib.sha256(before.encode('utf-8')).hexdigest()
            if checksum == hook['after_sha256']:
                continue
            if checksum != hook['before_sha256']:
                raise ValueError('Generated function does not match the supported compiler output: ' + address)
            lines = before.splitlines(keepends=True)
            previous = len(lines) + 1
            for start, count, added in reversed(hook['edits']):
                if not (0 <= start <= start + count <= len(lines)) or start + count > previous:
                    raise ValueError('Invalid/overlapping update edits for ' + address)
                lines[start:start + count] = added.splitlines(keepends=True)
                previous = start
            after = ''.join(lines)
            if hashlib.sha256(after.encode('utf-8')).hexdigest() != hook['after_sha256']:
                raise ValueError('Update hook result does not match: ' + address)
            replacements.append((match.start(), end, after))
        for start, end, after in reversed(replacements):
            source = source[:start] + after + source[end:]
        if replacements:
            if preamble not in source:
                include = source.index('\n', source.index('#include')) + 1
                source = source[:include] + preamble + source[include:]
            pending[path] = source
    missing = set(hooks) - seen
    if missing:
        raise ValueError('Missing generated update functions: ' + ', '.join(sorted(missing)))
    # Validate every function before modifying any output file. In particular,
    # a late compiler/layout mismatch must not leave a half-patched source tree.
    for path, source in pending.items():
        temporary = path.with_suffix(path.suffix + '.update120.tmp')
        with open(temporary, 'x', encoding='utf-8', newline='') as stream:
            stream.write(source)
        os.replace(temporary, path)
    print('BCUS98124 1.20 hooks: %d functions checked, %d chunks updated' % (len(seen), len(pending)))


if __name__ == '__main__' and len(sys.argv) > 1 and sys.argv[1] == '--apply-update-hooks':
    if len(sys.argv) != 4:
        sys.exit('usage: patch_lift.py --apply-update-hooks RECOMP EBOOT.ELF')
    apply_update120_hooks(sys.argv[2], sys.argv[3])
    sys.exit(0)
