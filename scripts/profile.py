#!/usr/bin/env python3
"""Symbolise a sample histogram written by scripts/rt.c (RT_PROFILE) against
the unstripped core.

  profile.py <core.so> <samples.txt> [--lines N] [--func NAME]

Prints the hottest functions, and with --func the hottest source lines of
that function (needs the .so built with debug info, which zig cc emits by
default). Needs pyelftools.
"""
import bisect
import collections
import sys

from elftools.elf.elffile import ELFFile


def load_symbols(elf):
    syms = []
    for sec in elf.iter_sections():
        if sec.name not in ('.symtab', '.dynsym'):
            continue
        for s in sec.iter_symbols():
            if s['st_info']['type'] == 'STT_FUNC' and s['st_size'] > 0:
                syms.append((s['st_value'], s['st_size'], s.name))
    syms.sort()
    return syms


def load_lines(elf):
    """Return a sorted list of (address, 'file:line')."""
    out = []
    if not elf.has_dwarf_info():
        return out
    dw = elf.get_dwarf_info()
    for cu in dw.iter_CUs():
        lp = dw.line_program_for_CU(cu)
        if lp is None:
            continue
        files = lp['file_entry']
        for e in lp.get_entries():
            st = e.state
            if st is None or st.end_sequence:
                continue
            fe = files[st.file - 1] if st.file >= 1 and st.file - 1 < len(files) else None
            name = fe.name.decode('latin1') if fe is not None else '?'
            out.append((st.address, '%s:%d' % (name, st.line)))
    out.sort()
    return out


def main():
    so, samples = sys.argv[1], sys.argv[2]
    n_lines = 40
    func = None
    args = sys.argv[3:]
    while args:
        a = args.pop(0)
        if a == '--lines':
            n_lines = int(args.pop(0))
        elif a == '--func':
            func = args.pop(0)
    elf = ELFFile(open(so, 'rb'))
    syms = load_symbols(elf)
    starts = [s[0] for s in syms]
    # The .so is mapped at its load address with vaddr 0 at the base, so the
    # sample offsets are ELF virtual addresses directly.
    total = other = 0
    per_func = collections.Counter()
    per_addr = {}
    per_lr = collections.Counter()
    per_lr_addr = {}
    for line in open(samples):
        parts = line.split()
        if parts[0] == 'total':
            total = int(parts[1]); continue
        if parts[0] == 'other':
            other = int(parts[1]); continue
        if parts[0] == 'ext':
            continue
        if parts[0] == 'lr':
            addr, count = int(parts[1], 16), int(parts[2])
            i = bisect.bisect_right(starts, addr) - 1
            name = syms[i][2] if i >= 0 and addr < syms[i][0] + syms[i][1] else '?'
            per_lr[name] += count
            per_lr_addr[addr] = per_lr_addr.get(addr, 0) + count
            continue
        addr, count = int(parts[0], 16), int(parts[1])
        per_addr[addr] = count
        i = bisect.bisect_right(starts, addr) - 1
        name = '?'
        if i >= 0 and addr < syms[i][0] + syms[i][1]:
            name = syms[i][2]
        per_func[name] += count
    print('samples: %d total, %d (%.1f%%) outside the core (libc, kernel)' % (total, other, 100.0 * other / max(total, 1)))
    print('%7s %6s  %s' % ('samples', '%', 'function'))
    for name, c in per_func.most_common(n_lines):
        print('%7d %5.1f%%  %s' % (c, 100.0 * c / max(total, 1), name))
    if per_lr:
        print(chr(10) + 'libc samples by calling function (link register):')
        for name, c in per_lr.most_common(8):
            print('%7d %5.1f%%  %s' % (c, 100.0 * c / max(total, 1), name))
    if func:
        lines = load_lines(elf)
        laddrs = [l[0] for l in lines]
        per_line = collections.Counter()
        rng = [s for s in syms if s[2] == func]
        if not rng:
            print('no symbol', func); return
        lo, hi = rng[0][0], rng[0][0] + rng[0][1]
        for addr, c in per_addr.items():
            if lo <= addr < hi:
                j = bisect.bisect_right(laddrs, addr) - 1
                per_line[lines[j][1] if j >= 0 else '?'] += c
        fc = per_func[func]
        print('\nhottest lines in %s (%d samples):' % (func, fc))
        for l, c in per_line.most_common(n_lines):
            print('%7d %5.1f%%  %s' % (c, 100.0 * c / max(fc, 1), l))


if __name__ == '__main__':
    main()
