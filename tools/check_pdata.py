#!/usr/bin/env python3
r"""Checks the exception table (.pdata) of a Windows x64 executable.

Windows and Wine find the unwind information of a code address by binary-searching the
RUNTIME_FUNCTION entries {BeginAddress, EndAddress, UnwindData} of .pdata (RtlLookupFunctionEntry),
then follow it to unwind the stack: a C++ throw (libgcc's SEH unwinder), a crash report, a debugger.
So the entries must be sorted by BeginAddress and must not overlap, and each UnwindData must point
at an UNWIND_INFO of version 1 or 2 (or, with its low bit set, chain to another entry).

    tools/check_pdata.py EXE [--function NAME ...] [--list N] [--verbose] [--quiet]

Prints the number of entries and of broken ones (unsorted, overlapping, empty, invalid unwind
info, outside the code), with the first N of each (named from the COFF symbol table, which MinGW
keeps unless the exe is stripped), then simulates the lookup for every byte of each --function
(default: the C++ exception path of libgcc and libstdc++) and counts the bytes it sends to an entry
that is not the function's own. Exit status 0 when everything is right, 1 otherwise. --quiet
prints the report only when something is wrong (the build runs it after each link of the exes).

Requirements: Python 3.8+, nothing else.
"""
import argparse
import bisect
import struct
import sys

DEFAULT_FUNCTIONS = ["__cxa_throw", "__cxa_rethrow", "_Unwind_RaiseException", "_Unwind_Resume",
                     "_Unwind_Resume_or_Rethrow", "_GCC_specific_handler", "__gxx_personality_seh0"]
IMAGE_SCN_CNT_CODE = 0x20


class Image:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = d = f.read()
        if d[:2] != b"MZ":
            raise SystemExit(f"{path}: not a PE file")
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise SystemExit(f"{path}: not a PE file")
        _, nsections, _, self.symtab, self.nsyms, opt_size, _ = struct.unpack_from("<HHIIIHH", d, pe + 4)
        opt = pe + 24
        if struct.unpack_from("<H", d, opt)[0] != 0x20B:
            raise SystemExit(f"{path}: not a 64-bit (PE32+) image")
        ndirs = struct.unpack_from("<I", d, opt + 108)[0]
        # data directory 3: the exception table
        self.pdata_rva, self.pdata_size = struct.unpack_from("<II", d, opt + 112 + 8 * 3) if ndirs > 3 else (0, 0)
        self.sections = []  # (name, rva, virtual size, file offset, raw size, characteristics)
        for i in range(nsections):
            off = opt + opt_size + 40 * i
            name, vsize, rva, rawsize, rawptr = struct.unpack_from("<8sIIII", d, off)
            flags = struct.unpack_from("<I", d, off + 36)[0]
            name = name.rstrip(b"\0").decode("latin-1")
            if name.startswith("/") and self.symtab:  # long name, in the string table
                name = self.string(int(name[1:]))
            self.sections.append((name, rva, vsize, rawptr, rawsize, flags))

    def string(self, offset):
        start = self.symtab + 18 * self.nsyms + offset
        return self.data[start:self.data.index(b"\0", start)].decode("latin-1")

    def section(self, rva):
        for s in self.sections:
            if s[1] <= rva < s[1] + max(s[2], s[4]):
                return s
        return None

    def read(self, rva, size):
        s = self.section(rva)
        if s is None or rva + size > s[1] + max(s[2], s[4]):
            return None
        off = rva - s[1]
        chunk = self.data[s[3] + off:s[3] + min(off + size, s[4])] if off < s[4] else b""
        return chunk + b"\0" * (size - len(chunk))

    def is_code(self, rva):
        s = self.section(rva)
        return s is not None and bool(s[5] & IMAGE_SCN_CNT_CODE)

    def code_symbols(self):
        """The symbols defined in code sections, as a sorted list of (rva, name)."""
        out = []
        i = 0
        while i < self.nsyms:
            raw, value, secnum, _, storage, naux = struct.unpack_from("<8sIhHBB", self.data, self.symtab + 18 * i)
            i += 1 + naux
            if not 0 < secnum <= len(self.sections) or storage not in (2, 3, 6):  # external, static, label
                continue
            name = self.string(struct.unpack_from("<I", raw, 4)[0]) if raw[:4] == b"\0\0\0\0" \
                else raw.rstrip(b"\0").decode("latin-1")
            sec = self.sections[secnum - 1]
            if sec[5] & IMAGE_SCN_CNT_CODE and not name.startswith("."):
                out.append((sec[1] + value, name))  # values are section-relative in an image too
        out.sort()
        return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("exe")
    ap.add_argument("--function", nargs="*", default=DEFAULT_FUNCTIONS,
                    help="functions whose lookup is simulated (default: the C++ exception path)")
    ap.add_argument("--list", type=int, default=5, metavar="N", help="broken entries listed per kind (default 5)")
    ap.add_argument("--verbose", action="store_true", help="also list the entries covering each function")
    ap.add_argument("--quiet", action="store_true", help="print the report only when something is wrong")
    args = ap.parse_args()
    report = []
    failed = check(args, report.append)
    if failed or not args.quiet:
        print("\n".join(report))
    return 1 if failed else 0


def check(args, say):
    """Writes the report with say(line); True when something is wrong."""
    img = Image(args.exe)
    if not img.pdata_size:
        say(f"{args.exe}: no exception table")
        return True
    raw = img.read(img.pdata_rva, img.pdata_size)
    n = img.pdata_size // 12
    entries = [struct.unpack_from("<III", raw, 12 * i) for i in range(n)]
    begins = [b for b, _, _ in entries]
    syms = img.code_symbols()
    sym_rvas = [r for r, _ in syms]

    def where(rva):
        k = bisect.bisect_right(sym_rvas, rva) - 1
        return f"{syms[k][1]}+{rva - syms[k][0]:#x}" if k >= 0 else "?"

    def show(i):
        b, e, u = entries[i]
        return f"#{i} [{b:#x}, {e:#x}) unwind {u:#x}: {where(b)} .. {where(e - 1)}"

    def unwind_problem(u):
        if u & 1:  # chained to another RUNTIME_FUNCTION
            return None if img.read(u & ~1, 12) is not None else "chained entry outside the image"
        header = img.read(u, 4)
        if header is None:
            return "unwind info outside the image"
        version = header[0] & 7
        return None if version in (1, 2) else f"unwind info version {version}"

    # An entry overlaps when it starts before the end of an earlier one (the furthest-reaching).
    overlapped = {}
    reach = None
    for i in range(n):
        if reach is not None and begins[i] < entries[reach][1]:
            overlapped[i] = reach
        if reach is None or entries[i][1] > entries[reach][1]:
            reach = i
    kinds = {
        "unsorted": [i for i in range(1, n) if begins[i] < begins[i - 1]],
        "overlapping": sorted(overlapped),
        "empty": [i for i in range(n) if entries[i][1] <= entries[i][0]],
        "outside code": [i for i in range(n) if not (img.is_code(entries[i][0]) and img.is_code(entries[i][1] - 1))],
        "invalid unwind info": [i for i in range(n) if unwind_problem(entries[i][2])],
    }
    say(f"{args.exe}: {n} entries; " + ", ".join(f"{k}: {len(v)}" for k, v in kinds.items()))
    for kind, items in kinds.items():
        for i in items[:args.list]:
            if kind in ("unsorted", "overlapping"):
                other = i - 1 if kind == "unsorted" else overlapped[i]
                say(f"  {kind}: {show(other)}\n  {' ' * len(kind)}  {show(i)}")
            else:
                say(f"  {kind}: {show(i)}" + (f" ({unwind_problem(entries[i][2])})" if kind.startswith("invalid") else ""))
        if len(items) > args.list:
            say(f"  ... and {len(items) - args.list} more {kind}")
    failed = any(kinds.values())

    # RtlLookupFunctionEntry, as Wine's find_function_info does it: a binary search.
    def lookup(pc):
        lo, hi = 0, n - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            if pc < entries[mid][0]:
                hi = mid - 1
            elif pc >= entries[mid][1]:
                lo = mid + 1
            else:
                return mid
        return None

    invalid = set(kinds["invalid unwind info"])
    first = {}
    for k, (_, name) in enumerate(syms):
        first.setdefault(name, k)
    for name in args.function:
        if name not in first:
            say(f"  {name}: not in the symbol table")
            continue
        k = first[name]
        start = syms[k][0]
        end = next((r for r, _ in syms[k + 1:] if r > start), start + 1)
        covering = [i for i in range(n) if entries[i][0] < end and entries[i][1] > start]
        own = [i for i in covering if start <= entries[i][0] and entries[i][1] <= end and i not in invalid]
        code = wrong = 0
        for pc in range(start, end):
            truth = next((j for j in own if entries[j][0] <= pc < entries[j][1]), None)
            if truth is None:
                continue  # alignment padding
            code += 1
            if lookup(pc) != truth:
                wrong += 1
        if not covering:
            say(f"  {name}: no entry (a leaf function needs none)")
            continue
        ok = bool(own) and wrong == 0
        failed |= not ok
        say(f"  {name}: {code} bytes of code, {len(own)} own entr{'y' if len(own) == 1 else 'ies'}, "
            f"{len(covering) - len(own)} foreign overlapping; lookup wrong for {wrong}: {'OK' if ok else 'WRONG'}")
        if args.verbose:
            for j in covering:
                say(f"      {'own' if j in own else 'foreign'} {show(j)}")
    return failed


if __name__ == "__main__":
    sys.exit(main())
