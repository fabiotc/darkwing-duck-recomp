"""nesrecomp command line.

    python -m nesrecomp recompile --config CONFIG.toml --rom GAME.nes --out DIR
    python -m nesrecomp interpreter --out FILE.c
    python -m nesrecomp disasm --rom GAME.nes --bank 7 --addr C000 [--count 32]
"""

import argparse
import os
import sys
import tomllib

from .recompiler import Cheat, Recompiler, Window, disasm, gen_interpreter
from .rom import Rom


def parse_entry(text: str) -> tuple[int, int]:
    """'BB:AAAA' -> (bank, addr)."""
    bank, addr = text.split(":")
    return int(bank, 16), int(addr, 16)


def load_entries(path: str) -> list[tuple[int, int]]:
    out = []
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                out.append(parse_entry(line))
    return out


def write_if_changed(path: str, text: str) -> None:
    """Avoid touching unchanged files so incremental builds stay incremental."""
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except FileNotFoundError:
        pass
    with open(path, "w") as f:
        f.write(text)


def cmd_recompile(args) -> int:
    with open(args.config, "rb") as f:
        cfg = tomllib.load(f)
    cfg_dir = os.path.dirname(os.path.abspath(args.config))
    rom = Rom.load(args.rom)

    want = cfg.get("rom", {}).get("sha1", "").lower()
    if want and want != rom.sha1:
        print(f"error: {args.rom} has SHA-1 {rom.sha1}, but {args.config} expects {want}.\n"
              "       This recompilation only supports the exact ROM listed in the config.",
              file=sys.stderr)
        if not args.force:
            return 1

    rc = cfg.get("recomp", {})
    cheats = [Cheat(c["id"], c["name"], c.get("description", ""),
                    [parse_entry(e) for e in c.get("return", [])],
                    [parse_entry(e) for e in c.get("skip", [])],
                    [(*parse_entry(p["at"]), [int(r, 16) for r in p["ram"]], p.get("unless")) for p in c.get("poke", [])],
                    c.get("choices", []), c.get("values", []))
              for c in cfg.get("cheats", [])]
    rec = Recompiler(rom, {int(a, 16) for a in rc.get("noreturn", [])}, cheats)
    seeds = rec.default_seeds()
    for key, addrs in rec.patch_seeds().items():
        seeds.setdefault(key, set()).update(addrs)
    entries = [parse_entry(e) for e in rc.get("entries", [])]
    for name in rc.get("entries_files", []):
        p = os.path.join(cfg_dir, name)
        if os.path.exists(p):
            entries += load_entries(p)
    for bank, addr in entries:
        if addr >= 0x8000 and bank < rom.num_banks:
            seeds.setdefault((bank, (addr >> 14) & 1), set()).add(addr)

    os.makedirs(args.out, exist_ok=True)
    windows, sources, total = [], [], 0
    for (bank, w) in sorted(seeds):
        wc = rec.analyse(Window(bank, w), seeds[(bank, w)])
        windows.append(wc)
        name = f"{wc.window.name}.c"
        write_if_changed(os.path.join(args.out, name), rec.gen_window(wc))
        sources.append(name)
        total += len(wc.insns)
        if args.verbose:
            print(f"  bank {bank} @ ${wc.window.base:04X}: {len(wc.insns)} instructions, "
                  f"{len(wc.labels)} entry labels")
    write_if_changed(os.path.join(args.out, "recomp_table.c"), rec.gen_table(windows))
    sources.append("recomp_table.c")
    # Lets CMake know which files to compile.
    write_if_changed(os.path.join(args.out, "sources.cmake"),
                     "set(NESRECOMP_GENERATED\n" +
                     "".join(f"    ${{CMAKE_CURRENT_LIST_DIR}}/{s}\n" for s in sources) + ")\n")
    print(f"nesrecomp: {total} instructions in {len(windows)} windows -> {args.out}")
    return 0


def cmd_interpreter(args) -> int:
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    write_if_changed(args.out, gen_interpreter())
    return 0


def cmd_disasm(args) -> int:
    rom = Rom.load(args.rom)
    rec = Recompiler(rom)
    addr = int(args.addr, 16)
    win = Window(args.bank, (addr >> 14) & 1)
    for _ in range(args.count):
        insn = rec.decode(win, addr)
        if insn is None:
            print(f"{addr:04X}: .byte ${rec.read(win, addr):02X}")
            addr += 1
            continue
        raw = " ".join(f"{rec.read(win, addr + k):02X}" for k in range(insn.op.size))
        print(f"{addr:04X}: {raw:<9} {disasm(insn)}")
        addr += insn.op.size
        if not win.contains(addr):
            break
    return 0


def main() -> int:
    p = argparse.ArgumentParser(prog="nesrecomp", description="NES static recompiler")
    sub = p.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("recompile", help="recompile a ROM to C")
    r.add_argument("--config", required=True)
    r.add_argument("--rom", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--force", action="store_true", help="ignore ROM hash mismatch")
    r.add_argument("-v", "--verbose", action="store_true")
    r.set_defaults(fn=cmd_recompile)

    i = sub.add_parser("interpreter", help="generate the fallback interpreter")
    i.add_argument("--out", required=True)
    i.set_defaults(fn=cmd_interpreter)

    d = sub.add_parser("disasm", help="disassemble PRG ROM")
    d.add_argument("--rom", required=True)
    d.add_argument("--bank", type=int, required=True)
    d.add_argument("--addr", required=True)
    d.add_argument("--count", type=int, default=32)
    d.set_defaults(fn=cmd_disasm)

    args = p.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
