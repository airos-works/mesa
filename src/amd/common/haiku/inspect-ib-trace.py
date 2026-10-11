#!/usr/bin/env python3
# Copyright 2026 air/OS. SPDX-License-Identifier: MIT
"""Inspect bounded native Haiku IB captures. This is not a command validator."""

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re
import struct


BEGIN = re.compile(r"amdgpu: native IB begin fd (\d+) context (\d+) capture (\d+) dwords (\d+)$")
DATA = re.compile(r"amdgpu: native IB data (\d+):((?: [0-9a-f]{8}){1,8})$")


def captures(path):
    current = None
    with path.open() as stream:
        for line_number, line in enumerate(stream, 1):
            line = line.rstrip("\n")
            if not line.startswith("amdgpu: native IB "):
                continue
            begin, data = BEGIN.fullmatch(line), DATA.fullmatch(line)
            if begin:
                if current is not None:
                    raise ValueError(f"{path}:{line_number}: unfinished previous capture")
                fd, context, sequence, size = map(int, begin.groups())
                if not 0 < size <= 16384 or size % 256:
                    raise ValueError(f"{path}:{line_number}: invalid native IB size {size}")
                current = dict(fd=fd, context=context, sequence=sequence, dwords=size, words=[])
            elif data and current is not None:
                if int(data[1]) != len(current["words"]):
                    raise ValueError(f"{path}:{line_number}: noncontiguous capture")
                current["words"].extend(int(word, 16) for word in data[2].split())
                if len(current["words"]) > current["dwords"]:
                    raise ValueError(f"{path}:{line_number}: oversized capture")
            elif line == "amdgpu: native IB end" and current is not None:
                if len(current["words"]) != current["dwords"]:
                    raise ValueError(f"{path}:{line_number}: truncated capture")
                yield current
                current = None
            else:
                raise ValueError(f"{path}:{line_number}: malformed capture record")
    if current is not None:
        raise ValueError(f"{path}: missing capture end")


def packets(words):
    offset = 0
    while offset < len(words):
        header = words[offset]
        kind = header >> 30
        # sid.h PKT3_NOP_PAD is a header-only exception to the count field.
        if kind == 2 or header == 0xffff1000:
            size = 1
        elif kind in (0, 3):
            size = ((header >> 16) & 0x3fff) + 2
        else:
            raise ValueError(f"DWORD {offset}: unsupported packet type {kind}")
        if size > len(words) - offset:
            raise ValueError(f"DWORD {offset}: packet extends past the captured IB")
        yield offset, header, words[offset + 1:offset + size]
        offset += size


def inspect(paths, export):
    common = Path(__file__).resolve().parent.parent
    names = {int(value, 16): name for name, value in re.findall(
        r"^#define PKT3_(\w+)\s+(0x[0-9a-fA-F]+)\b", (common / "sid.h").read_text(), re.M)}
    registers = json.loads((common.parent / "registers/gfx8.json").read_text())
    register_names = {item["map"]["at"]: item["name"] for item in registers["register_mappings"]
                      if item["map"]["to"] == "mm"}
    kinds, opcodes, lengths, flags = Counter(), Counter(), defaultdict(Counter), defaultdict(Counter)
    examples, reg_values, reg_counts = defaultdict(list), defaultdict(set), Counter()
    reg_and, reg_or, reg_indices = {}, {}, defaultdict(set)
    records = []
    # GFX8 byte register windows. Preserve indexed selector bits separately.
    bases = {0x68: 0x8000, 0x69: 0x28000, 0x76: 0xb000, 0x79: 0x30000, 0x9b: 0xb000}
    if export:
        export.mkdir(parents=True, exist_ok=True)
    for path in paths:
        found = 0
        for capture in captures(path):
            found += 1
            words = capture.pop("words")
            capture.update(source=str(path), ordinal=found)
            records.append(capture)
            if export:
                target = export / f"{path.stem}-{found:04d}.ib"
                target.write_bytes(struct.pack(f"<{len(words)}I", *words))
            for offset, header, payload in packets(words):
                kind = header >> 30
                kinds[kind] += 1
                if kind != 3:
                    continue
                op = (header >> 8) & 255
                opcodes[op] += 1
                lengths[op][len(payload)] += 1
                flags[op][header & 255] += 1
                if len(examples[op]) < 4:
                    examples[op].append(dict(source=str(path), capture=found, offset=offset,
                                             header=f"0x{header:08x}",
                                             payload=[f"0x{x:08x}" for x in payload[:8]]))
                if op in bases:
                    if len(payload) < 2:
                        raise ValueError(f"{path}: empty SET register packet at DWORD {offset}")
                    first = bases[op] + (payload[0] & 0xffff) * 4
                    for i, value in enumerate(payload[1:]):
                        address = first + i * 4
                        reg_counts[address] += 1
                        reg_values[address].add(value)
                        reg_and[address] = reg_and.get(address, 0xffffffff) & value
                        reg_or[address] = reg_or.get(address, 0) | value
                        reg_indices[address].add(payload[0] >> 16)
        if not found:
            raise ValueError(f"{path}: no complete native IB captures")
    return dict(
        scope="Observed outer IB framing and direct SET register coverage only; not command validation or a safe allowlist. Nested buffers and shader bodies are not captured.",
        captures=len(records), dwords=sum(r["dwords"] for r in records), packet_types=dict(sorted(kinds.items())),
        opcodes={f"0x{op:02x}": dict(name=names.get(op, "UNKNOWN"), count=count,
                  payload_lengths=dict(sorted(lengths[op].items())),
                  header_flags={f"0x{k:02x}": v for k, v in sorted(flags[op].items())}, examples=examples[op])
                 for op, count in sorted(opcodes.items())},
        registers={f"0x{address:05x}": dict(name=register_names.get(address, "UNKNOWN"), count=count,
                   distinct_values=len(reg_values[address]),
                   values=[f"0x{x:08x}" for x in sorted(reg_values[address])[:16]],
                   observed_and=f"0x{reg_and[address]:08x}", observed_or=f"0x{reg_or[address]:08x}",
                   indices=sorted(reg_indices[address])) for address, count in sorted(reg_counts.items())},
        records=records)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("traces", type=Path, nargs="+")
    parser.add_argument("--export", type=Path, help="Export complete little-endian .ib files for ac_ib_parser polaris10")
    args = parser.parse_args()
    try:
        print(json.dumps(inspect(args.traces, args.export), indent=2))
    except (ValueError, OSError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
