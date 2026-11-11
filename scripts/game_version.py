"""Version-specific host bindings and SPU input addresses."""
import csv
import hashlib
import json
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UPDATE = 'BCUS98124-1.20'


def identify_profile(elf, selected='auto', require_buildable=True):
    with open(os.path.join(ROOT, 'versions', 'profiles.json'), encoding='utf-8') as stream:
        profiles = json.load(stream)['profiles']
    checksum = hashlib.sha256()
    with open(elf, 'rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            checksum.update(block)
    digest = checksum.hexdigest()
    actual = next((profile for profile in profiles if profile['elf_sha256'] == digest), None)
    if actual is None:
        raise ValueError('Unrecognized executable SHA-256 ' + digest)
    if selected != 'auto' and selected != actual['id']:
        raise ValueError('Selected ' + selected + ' but the executable is ' + actual['id'])
    if require_buildable and not actual['buildable']:
        raise ValueError('Recognized ' + actual['id'] + ', but this source tree cannot build it: ' + actual['status'])
    return actual


def rows(name):
    with open(os.path.join(ROOT, 'versions', name), encoding='utf-8') as stream:
        return list(csv.DictReader(stream, delimiter='\t'))


def update_inputs(elf):
    profile = identify_profile(elf, os.environ.get('RACACIT_GAME_PROFILE', 'auto'),
                               require_buildable=False)
    if profile['id'] == 'BCUS98124-1.00':
        return None
    if profile['id'] != UPDATE:
        raise ValueError('No SPU input implementation for ' + profile['id'])
    addresses, payloads, jobs = {}, {}, []
    for row in rows(UPDATE + '-spu.tsv'):
        if row['status'] == 'CHANGED_HEADER_PORT':
            if row['module'] != 'sub_DB4180_10680':
                raise ValueError('Unsupported changed SPU module ' + row['module'])
        elif row['status'] != 'UNIQUE_FULL_PAYLOAD_MATCH':
            raise ValueError('Unresolved SPU module ' + row['module'])
        old = int(row['disc_ea'], 16)
        new = int(row['update120_ea'], 16)
        if old in addresses and addresses[old] != new:
            raise ValueError('Conflicting SPU address mapping')
        addresses[old] = new
        payloads[row['module']] = (new, int(row['bytes']))
        if row['module'].startswith('job_'):
            jobs.append((new, row['module'], int(row['bytes'])))
    return dict(addresses=addresses, payloads=payloads, jobs=sorted(jobs))


def mapped_address(version, address):
    return version['addresses'][address] if version else address


def changed_module(raw):
    """Derive the relocatable entry and proven code prefix from update bytes."""
    if len(raw) != 0x5700:
        raise ValueError('Changed update module envelope differs')
    words = struct.unpack('>12I', raw[:48])
    if not all(word & 0xFE00007F == 0x42000002 for word in words[:4]):
        raise ValueError('Changed update module GUID header differs')
    entry = (words[6] & 0xFFFF) * 4
    if entry == 0:
        if words[5:] != (0, 0, 0x10, 0xFFFFFFEF, 0, 0xDEADDEAD, 0xDEADDEAD):
            raise ValueError('Unsupported SPU entry header')
        entry = 32 + words[4]
    if entry != 0x20:
        raise ValueError('Changed update module entry differs')
    literal = b'ASSERT FAILED'
    code_end = raw.find(literal)
    if code_end <= entry + 4 or code_end % 16 or raw.find(literal, code_end + 1) != -1:
        raise ValueError('Changed update module literal boundary differs')
    for (word,) in struct.iter_unpack('>I', raw[entry:code_end]):
        if word >> 23 in (0x60, 0x62):
            raise ValueError('Absolute SPU branch requires a separate relocation port')
    return 0x100 + entry, 0x100 + code_end
