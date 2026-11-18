#!/usr/bin/env python3
"""Prepare a separate GLOBAL_CACHED.PSARC with keyboard menu actions."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
SOURCE_SHA = 'a647e066c7611e4725b6f25c4d20746e6854618450bf392549d90a75042a9e15'
ACTIONS_SHA = '2edc51a2e75270b7f2df9850fbca6245ad8841307a4d4d7013132cae26e991d3'
MEMBER_LIMIT = 8 * 1024 * 1024


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    checksum = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            checksum.update(block)
    return checksum.hexdigest()


def number(data):
    return int.from_bytes(data, 'big')


def put(data, offset, width, value):
    data[offset:offset + width] = value.to_bytes(width, 'big')


class Archive:
    def __init__(self, source):
        self.source = source
        self.header = bytearray(source.read(32))
        require(len(self.header) == 32 and self.header[:4] == b'PSAR' and
                self.header[8:12] == b'zlib', 'expected a zlib PSARC archive')
        self.toc_size, self.entry_size, self.count, self.block = struct.unpack('>4I', self.header[12:28])
        require(32 <= self.toc_size <= 64 * 1024 * 1024 and
                self.entry_size >= 30 and self.count > 0 and
                self.count * self.entry_size <= self.toc_size - 32 and
                0 < self.block <= 1024 * 1024, 'invalid archive table')
        self.width = 2 if self.block <= 65536 else 3
        self.toc = bytearray(source.read(self.toc_size - 32))
        require(len(self.toc) == self.toc_size - 32, 'truncated archive table')
        self.length = os.fstat(source.fileno()).st_size
        self.members = []
        for index in range(self.count):
            start = index * self.entry_size
            entry = self.toc[start:start + self.entry_size]
            first, length, offset = number(entry[16:20]), number(entry[20:25]), number(entry[25:30])
            blocks = (length + self.block - 1) // self.block
            table = self.count * self.entry_size + first * self.width
            require(table + blocks * self.width <= len(self.toc), 'member exceeds block table')
            sizes = [number(self.toc[p:p + self.width]) or self.block
                     for p in range(table, table + blocks * self.width, self.width)]
            require(all(size <= self.block for size in sizes), 'invalid member block size')
            span = sum(sizes)
            require(offset + span <= self.length and
                    (not length or offset >= self.toc_size), 'invalid member extent')
            self.members.append((first, length, offset, sizes))

    def read(self, index):
        first, length, offset, sizes = self.members[index]
        require(length <= MEMBER_LIMIT, 'menu or manifest exceeds 8 MiB')
        self.source.seek(offset)
        result = bytearray()
        for size in sizes:
            raw = self.source.read(size)
            require(len(raw) == size, 'truncated member')
            wanted = min(self.block, length - len(result))
            if size != wanted:
                decoder = zlib.decompressobj()
                raw = decoder.decompress(raw, wanted + 1)
                require(decoder.eof and not decoder.unused_data and
                        not decoder.unconsumed_tail, 'invalid compressed member block')
            require(len(raw) == wanted, 'member block length mismatch')
            result.extend(raw)
        return bytes(result)

    def write(self, output, replacements):
        encoded = {}
        for index, data in replacements.items():
            chunks, sizes = [], []
            for start in range(0, len(data), self.block):
                raw = data[start:start + self.block]
                compressed = zlib.compress(raw, 9)
                chunk = compressed if len(compressed) < len(raw) else raw
                chunks.append(chunk)
                sizes.append(len(chunk) % self.block)
            encoded[index] = (b''.join(chunks), sizes)

        table = self.count * self.entry_size
        toc = bytearray(self.toc[:table])
        cursor = table
        ordered = sorted(encoded, key=lambda index: self.members[index][0])
        for index in ordered:
            first, length, offset, old_sizes = self.members[index]
            start = table + first * self.width
            require(start >= cursor, 'replacement block tables overlap')
            toc.extend(self.toc[cursor:start])
            toc.extend(b''.join(size.to_bytes(self.width, 'big') for size in encoded[index][1]))
            cursor = start + len(old_sizes) * self.width
        toc.extend(self.toc[cursor:])
        toc_delta = len(toc) - len(self.toc)

        for member, (first, length, offset, sizes) in enumerate(self.members):
            new_first = first
            new_offset = offset + toc_delta if length or offset else 0
            for index in ordered:
                old_first, old_length, old_offset, old_sizes = self.members[index]
                old_end = old_offset + sum(old_sizes)
                if member != index and length:
                    require(first + len(sizes) <= old_first or first >= old_first + len(old_sizes),
                            'replacement shares another member block table')
                    require(offset + sum(sizes) <= old_offset or offset >= old_end,
                            'replacement overlaps another member')
                if first >= old_first + len(old_sizes):
                    new_first += len(encoded[index][1]) - len(old_sizes)
                if offset >= old_end:
                    new_offset += len(encoded[index][0]) - sum(old_sizes)
            start = member * self.entry_size
            put(toc, start + 16, 4, new_first)
            put(toc, start + 25, 5, new_offset)
            if member in replacements:
                put(toc, start + 20, 5, len(replacements[member]))
        header = self.header.copy()
        put(header, 12, 4, 32 + len(toc))
        output.write(header)
        output.write(toc)
        cursor = self.toc_size
        for index in sorted(encoded, key=lambda index: self.members[index][2]):
            first, length, offset, sizes = self.members[index]
            self.copy(output, cursor, offset)
            output.write(encoded[index][0])
            cursor = offset + sum(sizes)
        self.copy(output, cursor, self.length)

    def copy(self, output, start, end):
        require(end >= start, 'replacement physical extents overlap')
        self.source.seek(start)
        while start < end:
            chunk = self.source.read(min(end - start, 1024 * 1024))
            require(chunk, 'truncated archive while copying')
            output.write(chunk)
            start += len(chunk)


def movie_actions(original, actions):
    require(len(original) >= 14 and original[:3] in (b'FWS', b'CWS', b'GFX', b'CFX'),
            'unsupported menu movie format')
    length = struct.unpack_from('<I', original, 4)[0]
    require(14 <= length <= MEMBER_LIMIT, 'movie exceeds 8 MiB')
    movie = original
    if original[0] == ord('C'):
        decoder = zlib.decompressobj()
        body = decoder.decompress(original[8:], length - 7)
        require(decoder.eof and not decoder.unused_data and not decoder.unconsumed_tail,
                'invalid compressed movie')
        movie = (b'F' if original[1] == ord('W') else b'G') + original[1:8] + body
    require(len(movie) == length, 'movie length mismatch')
    if b'ButtonPrompt' not in movie:
        return None
    require(b'__rccitPromptDriver' not in movie, 'input movie already contains keyboard actions')
    position = 8 + (5 + 4 * (movie[8] >> 3) + 7) // 8 + 4
    insertion = None
    code = None
    while position < len(movie):
        start = position
        require(position + 2 <= len(movie), 'truncated movie tag')
        tag = struct.unpack_from('<H', movie, position)[0]
        code, size = tag >> 6, tag & 63
        position += 2
        if size == 63:
            require(position + 4 <= len(movie), 'truncated long movie tag')
            size = struct.unpack_from('<I', movie, position)[0]
            position += 4
        position += size
        require(position <= len(movie), 'movie tag exceeds member')
        if code == 1 and insertion is None:
            insertion = start
        if code == 0:
            require(position == len(movie), 'bytes follow movie End tag')
            break
    require(insertion is not None and code == 0, 'movie needs ShowFrame and End tags')
    tag = struct.pack('<HI', (12 << 6) | 63, len(actions)) + actions
    patched = bytearray(movie[:insertion] + tag + movie[insertion:])
    struct.pack_into('<I', patched, 4, len(patched))
    if original[0] == ord('C'):
        return original[:4] + patched[4:8] + zlib.compress(patched[8:], 9)
    return bytes(patched)


def prepare(original, destination):
    require(original.is_file(), 'original archive does not exist')
    require(original.resolve() != destination.resolve(), 'output must differ from the original')
    require(digest(ROOT / 'game/menu_keycaps.as') == SOURCE_SHA, 'menu source changed; recompile actions')
    actions_path = ROOT / 'game/menu_keycaps.actions.bin'
    actions = actions_path.read_bytes()
    require(hashlib.sha256(actions).hexdigest() == ACTIONS_SHA,
            'compiled menu actions differ from the source receipt')
    identity = {'input_sha256': digest(original), 'source_sha256': SOURCE_SHA, 'actions_sha256': ACTIONS_SHA}
    receipt = destination.with_name(destination.name + '.keycaps.json')
    if destination.exists():
        saved = json.loads(receipt.read_text())
        require(all(saved.get(key) == value for key, value in identity.items()) and
                saved.get('output_sha256') == digest(destination),
                'existing output differs; choose a fresh output path')
        print('Reusing verified prepared archive', destination)
        return
    require(not receipt.exists(), 'receipt already exists; choose a fresh output path')
    require(destination.parent.is_dir(), 'output directory must already exist')
    temporary = None
    published = False
    receipt_created = False
    try:
        with original.open('rb') as source:
            archive = Archive(source)
            names = archive.read(0).decode('utf-8').splitlines()
            require(len(names) + 1 == archive.count, 'manifest/member count mismatch')
            replacements, menus = {}, []
            for index, name in enumerate(names, 1):
                if name.startswith('/scaleform/') and name.endswith('.swf'):
                    replacement = movie_actions(archive.read(index), actions)
                    if replacement is not None:
                        replacements[index] = replacement
                        menus.append(name)
            require(replacements, 'no ButtonPrompt menu movies found')
            with tempfile.NamedTemporaryFile(prefix='.menu-keycaps-', dir=destination.parent, delete=False) as output:
                temporary = Path(output.name)
                archive.write(output, replacements)
                output.flush()
                os.fsync(output.fileno())
            with temporary.open('rb') as output:
                prepared = Archive(output)
                require(prepared.read(0) == archive.read(0), 'manifest changed')
                for index, expected in replacements.items():
                    require(prepared.read(index) == expected, 'patched movie readback mismatch')
            require(digest(original) == identity['input_sha256'], 'original archive changed during preparation')
        identity.update(output_sha256=digest(temporary), menus=menus)
        # Hard-link publication creates a new destination atomically and never replaces a file.
        os.link(temporary, destination)
        published = True
        with receipt.open('x', encoding='utf-8') as output:
            receipt_created = True
            json.dump(identity, output, indent=2)
            output.write('\n')
        print('Prepared', len(menus), 'menu movies in', destination)
    except BaseException:
        if receipt_created:
            receipt.unlink()
        if published:
            destination.unlink()
        raise
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original', type=Path, help='your original GLOBAL_CACHED.PSARC')
    parser.add_argument('output', type=Path, help='new prepared archive, kept outside the game dump')
    args = parser.parse_args()
    try:
        prepare(args.original, args.output)
    except (OSError, ValueError, zlib.error, struct.error) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
