#!/usr/bin/env python3
"""Instruction-set audit of an executable holding several Stockfish variants (Scacelith build check,
not part of upstream Stockfish). Run by the build (CMakeLists.txt, target stockfish_isa_audit).

usage: isa_audit.py EXE MAP OBJDUMP VARIANT_REGEX=LEVEL... [--allow OBJECT_REGEX]... [--report FILE]

EXE is an x86-64 ELF (position-independent) or PE executable linked with -Wl,-Map,MAP; OBJDUMP
is the binutils objdump for its format. Each VARIANT_REGEX matches, in the link map, the object
file of one variant, and LEVEL is the highest instruction class that variant may contain:
    0  baseline x86-64 (up to SSE2)
    1  SSE3 / SSSE3 / SSE4 / POPCNT / LZCNT
    2  VEX encoding (AVX, AVX2, FMA, BMI1, BMI2, AVX-VNNI)
    3  EVEX encoding (AVX-512)
Every function of the disassembly is attributed to the object it came from (link map) and given
the highest class among its instructions. Every reference to code is collected: direct calls and
jumps, RIP-relative operands, and pointers stored in data (ELF: relative dynamic relocations; PE:
64-bit base relocations). The audit fails (exit status 1) when
  * a variant's object is missing from the map, or holds a function above its level;
  * a function outside the variants is above level 0: code shared with the rest of the program
    (inline functions, templates) compiled with a variant's flags would crash other CPUs;
  * code outside a variant object refers to code inside it, unless the referring object matches
    an --allow regex (the dispatcher, which calls the entry points);
  * the executable hides its data pointers (not position-independent, no base relocations).
The findings go to stdout, or to the --report file with a one-line summary on stdout.
"""
import bisect
import collections
import re
import struct
import subprocess
import sys

LEGACY_PREFIXES = {0x66, 0x67, 0xf2, 0xf3, 0x2e, 0x3e, 0x26, 0x36, 0x64, 0x65, 0xf0}
# SSE3 / SSSE3 / SSE4.1 / SSE4.2 / POPCNT / LZCNT mnemonics (legacy encoding). TZCNT is left out:
# GCC emits "rep bsf" (TZCNT's encoding) for __builtin_ctz at every level on purpose, as older
# CPUs run it as BSF with the same result for a non-zero input.
LEVEL1 = set('''addsubps addsubpd haddps haddpd hsubps hsubpd lddqu movddup movshdup movsldup fisttp
pshufb phaddw phaddd phaddsw phsubw phsubd phsubsw pmaddubsw pmulhrsw psignb psignw psignd pabsb
pabsw pabsd palignr pblendvb blendvps blendvpd ptest pmuldq pcmpeqq movntdqa packusdw pmovsxbw
pmovsxbd pmovsxbq pmovsxwd pmovsxwq pmovsxdq pmovzxbw pmovzxbd pmovzxbq pmovzxwd pmovzxwq pmovzxdq
pminsb pminsd pminuw pminud pmaxsb pmaxsd pmaxuw pmaxud pmulld phminposuw roundps roundpd roundss
roundsd blendps blendpd pblendw dpps dppd mpsadbw insertps pinsrb pinsrd pinsrq extractps pextrb
pextrd pextrq pcmpgtq crc32 pcmpestri pcmpestrm pcmpistri pcmpistrm popcnt lzcnt'''.split())
MNEMONIC_PREFIXES = {'rex', 'rex.w', 'data16', 'addr32', 'cs', 'ds', 'es', 'ss', 'fs', 'gs', 'lock',
                     'rep', 'repz', 'repnz', 'repe', 'repne', 'notrack', 'bnd'}


def fail_usage(message):
    sys.stderr.write('isa_audit: %s\n' % message)
    sys.exit(2)


def parse_args(argv):
    if len(argv) < 5:
        fail_usage(__doc__.split('\n\n')[1])
    exe, mapfile, objdump = argv[1:4]
    variants, allow, report = [], [], None
    rest = argv[4:]
    i = 0
    while i < len(rest):
        if rest[i] in ('--allow', '--report'):
            if i + 1 == len(rest):
                fail_usage('%s needs a value' % rest[i])
            if rest[i] == '--allow':
                allow.append(re.compile(rest[i + 1]))
            else:
                report = rest[i + 1]
            i += 2
            continue
        regex, sep, level = rest[i].rpartition('=')
        if not sep or not regex or level not in ('0', '1', '2', '3'):
            fail_usage('bad variant argument %r (REGEX=LEVEL, LEVEL 0 to 3)' % rest[i])
        variants.append((re.compile(regex), int(level)))
        i += 1
    if not variants:
        fail_usage('no variant given')
    return exe, mapfile, objdump, variants, allow, report


def read_map(mapfile):
    """Input sections of the link map: sorted (start, end, object, section name)."""
    full = re.compile(r'^ (\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$')
    name_only = re.compile(r'^ (\S+)$')
    continued = re.compile(r'^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$')
    ranges = []
    in_map = False
    pending = None
    with open(mapfile, errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if not in_map:
                in_map = line.startswith('Linker script and memory map')
                continue
            m = full.match(line)
            if m:
                start, size = int(m.group(2), 16), int(m.group(3), 16)
                if start and size:
                    ranges.append((start, start + size, m.group(4).strip(), m.group(1)))
                pending = None
                continue
            m = name_only.match(line)
            if m:
                pending = m.group(1)
                continue
            if pending:
                m = continued.match(line)
                if m:
                    start, size = int(m.group(1), 16), int(m.group(2), 16)
                    if start and size:
                        ranges.append((start, start + size, m.group(3).strip(), pending))
                pending = None
    ranges.sort()
    return ranges


def short(obj):
    """An object's name without directories: libfoo.a(bar.o), bar.o."""
    return re.sub(r'^[^(]*/', '', obj)


def instruction_level(raw, mnemonic):
    j = 0
    while j < len(raw) and raw[j] in LEGACY_PREFIXES:
        j += 1
    if j < len(raw) and 0x40 <= raw[j] <= 0x4f:  # REX
        j += 1
    if j < len(raw):
        if raw[j] == 0x62:
            return 3
        if raw[j] in (0xc4, 0xc5):
            return 2
    base = mnemonic.split('.')[0]
    if base in LEVEL1 or base[:-1] in ('popcnt', 'lzcnt', 'crc32'):  # suffixed forms: popcntq ...
        return 1
    return 0


def disassemble(exe, objdump):
    """Functions [start, name, level] and code references (from, to, kind)."""
    header = re.compile(r'^([0-9a-f]+) <(.+)>:$')
    instruction = re.compile(r'^\s*([0-9a-f]+):\t((?:[0-9a-f]{2} )+)\s*\t?(.*)$')
    branch = re.compile(r'\b(?:call|jmp|j[a-z]{1,3})\w*\s+([0-9a-f]+) <')
    rip = re.compile(r'#\s*([0-9a-f]+) <')
    functions, refs = [], []
    current = None
    proc = subprocess.Popen([objdump, '-d', '-w', exe], stdout=subprocess.PIPE, text=True, errors='replace')
    for line in proc.stdout:
        m = header.match(line)
        if m:
            current = [int(m.group(1), 16), m.group(2), 0]
            functions.append(current)
            continue
        m = instruction.match(line)
        if not m or current is None:
            continue
        address = int(m.group(1), 16)
        raw = bytes(int(x, 16) for x in m.group(2).split())
        words = m.group(3).split()
        while len(words) > 1 and words[0] in MNEMONIC_PREFIXES:
            words = words[1:]
        level = instruction_level(raw, words[0] if words else '')
        current[2] = max(current[2], level)
        text = m.group(3)
        t = branch.search(text)
        if t:
            refs.append((address, int(t.group(1), 16), 'branch'))
        else:
            t = rip.search(text)
            if t:
                refs.append((address, int(t.group(1), 16), 'rip'))
    if proc.wait() != 0:
        raise SystemExit('isa_audit: %s -d failed' % objdump)
    functions.sort()
    return functions, refs


def data_pointers(exe, objdump):
    """Pointers stored in data (from, to, 'data'); None when the executable does not show them."""
    info = subprocess.run([objdump, '-f', exe], capture_output=True, text=True, check=True).stdout
    refs = []
    if 'elf64' in info:
        if 'EXEC_P' in info:
            return None  # not position-independent: absolute pointers carry no relocation
        out = subprocess.run([objdump, '-R', exe], capture_output=True, text=True, check=True).stdout
        for line in out.splitlines():
            p = line.split()
            if len(p) == 3 and p[1] in ('R_X86_64_RELATIVE', 'R_X86_64_IRELATIVE') and p[2].startswith('*ABS*+0x'):
                refs.append((int(p[0], 16), int(p[2][len('*ABS*+'):], 16), 'data'))
        return refs
    data = open(exe, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    sections = struct.unpack_from('<H', data, pe + 6)[0]
    optional_size = struct.unpack_from('<H', data, pe + 20)[0]
    optional = pe + 24
    image_base = struct.unpack_from('<Q', data, optional + 24)[0]
    reloc_rva, reloc_size = struct.unpack_from('<II', data, optional + 112 + 5 * 8)
    if not reloc_rva or not reloc_size:
        return None
    table = []
    for k in range(sections):
        sh = optional + optional_size + 40 * k
        virtual_size, va, raw_size, raw_ptr = struct.unpack_from('<IIII', data, sh + 8)
        table.append((va, max(virtual_size, raw_size), raw_ptr, raw_size))

    def file_offset(rva):
        for va, size, raw_ptr, raw_size in table:
            if va <= rva < va + size:
                return raw_ptr + rva - va if rva - va < raw_size else None
        return None

    offset = file_offset(reloc_rva)
    end = offset + reloc_size
    while offset < end:
        page, block = struct.unpack_from('<II', data, offset)
        if block == 0:
            break
        for e in range(8, block, 2):
            entry = struct.unpack_from('<H', data, offset + e)[0]
            if entry >> 12 == 10:  # IMAGE_REL_BASED_DIR64
                rva = page + (entry & 0xfff)
                at = file_offset(rva)
                if at is not None:
                    refs.append((image_base + rva, struct.unpack_from('<Q', data, at)[0], 'data'))
        offset += block
    return refs


def main():
    exe, mapfile, objdump, variants, allow, report_file = parse_args(sys.argv)
    ranges = read_map(mapfile)
    if not ranges:
        fail_usage('no memory map in %s' % mapfile)
    starts = [r[0] for r in ranges]

    def range_at(address):
        k = bisect.bisect_right(starts, address) - 1
        return ranges[k] if k >= 0 and address < ranges[k][1] else None

    def owner(address):
        r = range_at(address)
        return r[2] if r else '?'

    def variant_of(obj):
        for regex, level in variants:
            if regex.search(obj):
                return regex.pattern
        return None

    declared = {regex.pattern: level for regex, level in variants}
    functions, refs = disassemble(exe, objdump)
    pointers = data_pointers(exe, objdump)
    problems = []
    if pointers is None:
        problems.append('the executable hides its data pointers (ELF: not position-independent; '
                        'PE: no base relocations)')
    else:
        refs += pointers
    starts_f = [f[0] for f in functions]

    def function_at(address):
        k = bisect.bisect_right(starts_f, address) - 1
        return functions[k] if k >= 0 else None

    lines = []
    per_object = collections.defaultdict(lambda: [0, 0, 0, 0])
    for f in functions:
        per_object[owner(f[0])][f[2]] += 1
    found = set()
    lines.append('== functions per object by level (variants, and any other object above level 0)')
    for obj, counts in sorted(per_object.items()):
        variant = variant_of(obj)
        top = max(level for level in range(4) if counts[level])
        note = ''
        if variant is not None:
            found.add(variant)
            if top > declared[variant]:
                note = '  <-- above its level %d' % declared[variant]
                problems.append('%s holds code of level %d, above its level %d' % (obj, top, declared[variant]))
        elif top > 0:
            note = '  <-- non-variant code above level 0'
        if variant is not None or top > 0:
            lines.append('  %-48s L0=%5d L1=%4d L2=%4d L3=%4d%s' % (short(obj), *counts, note))
    for regex, _ in variants:
        if regex.pattern not in found:
            problems.append('no function from a variant object matching %r in the link map' % regex.pattern)

    above = [f for f in functions if f[2] > 0 and variant_of(owner(f[0])) is None]
    lines.append('== functions outside the variants above level 0: %d' % len(above))
    for f in above[:40]:
        lines.append('  L%d %s  [%s]' % (f[2], f[1][:110], short(owner(f[0]))))
    if above:
        problems.append('%d functions outside the variants use instructions above level 0' % len(above))

    lines.append('== references into variant code from outside its object')
    allowed, violations, examples = collections.Counter(), collections.Counter(), {}
    for source, target, kind in refs:
        r = range_at(target)
        if r is None or not r[3].startswith('.text'):
            continue
        variant = variant_of(r[2])
        source_owner = owner(source)
        if variant is None or source_owner == r[2]:
            continue
        callee = function_at(target)
        key = (short(source_owner), kind, short(r[2]), callee[1][:90] if callee else hex(target))
        if any(a.search(source_owner) for a in allow):
            allowed[key] += 1
        else:
            violations[key] += 1
            caller = function_at(source) if kind != 'data' else None
            examples.setdefault(key, '%s %s' % (hex(source), caller[1][:80] if caller else ''))
    for key, count in sorted(allowed.items()):
        lines.append('  allowed    %dx %s -(%s)-> %s: %s' % (count, *key))
    for key, count in sorted(violations.items()):
        lines.append('  VIOLATION  %dx %s -(%s)-> %s: %s, e.g. at %s' % (count, *key, examples[key]))
    if violations:
        problems.append('%d references into variant code from outside (%d distinct)' %
                        (sum(violations.values()), len(violations)))
    kinds = collections.Counter(kind for _, _, kind in refs)
    lines.append('== totals: %d functions, %d references (%s)' % (len(functions), len(refs), dict(kinds)))
    lines.append('== result: %s' % ('FAILED' if problems else 'passed'))
    lines += ['  ' + p for p in problems]

    text = '\n'.join(lines) + '\n'
    if report_file:
        with open(report_file, 'w') as f:
            f.write(text)
        print('isa_audit: %s, %d functions, %d references, variants: %d, %s (report: %s)' %
              (exe, len(functions), len(refs), len(variants), 'FAILED' if problems else 'passed', report_file))
    else:
        sys.stdout.write(text)
    for p in problems:
        sys.stderr.write('isa_audit: %s\n' % p)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
