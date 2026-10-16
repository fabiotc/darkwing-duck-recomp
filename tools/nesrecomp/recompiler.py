"""Static recompiler: 6502 PRG ROM -> C.

Code is organised in *windows*: one 16 KiB PRG bank as seen through one CPU
address range ($8000-$BFFF or $C000-$FFFF).  Each window becomes a single C
function containing every discovered instruction as a label, with a `switch`
at the top used to enter the window at any block start (after a call,
return, interrupt, or computed jump).  Control flow that stays inside the
window compiles to `goto`; anything else returns to the runtime dispatcher.

Code that the analysis did not discover (computed jumps to unknown places,
code copied to RAM, ...) is executed by the runtime's fallback interpreter,
so incomplete coverage costs speed, never correctness.  Running the game with
`--trace` records the block starts the interpreter had to execute; feeding
that file back here closes the gap.
"""

from dataclasses import dataclass, field

from .opcodes import OPCODES, BRANCHES, TERMINATORS, Opcode
from .rom import Rom, PRG_BANK
from .semantics import StaticCtx, DynamicCtx, emit_instruction


@dataclass
class Window:
    bank: int
    w: int  # 0 = $8000-$BFFF, 1 = $C000-$FFFF

    @property
    def base(self) -> int:
        return 0x8000 + self.w * PRG_BANK

    def contains(self, addr: int) -> bool:
        return self.base <= addr < self.base + PRG_BANK

    @property
    def name(self) -> str:
        return f"b{self.bank:02X}_{self.base:04X}"


@dataclass
class Insn:
    addr: int
    op: Opcode
    operand: int


@dataclass
class WindowCode:
    window: Window
    insns: dict[int, Insn] = field(default_factory=dict)
    labels: set[int] = field(default_factory=set)
    invalid: set[int] = field(default_factory=set)


def fmt_operand(op: Opcode, operand: int, addr: int) -> str:
    m = op.mode
    if m in ("imp",):
        return ""
    if m == "acc":
        return "A"
    if m == "imm":
        return f"#${operand:02X}"
    if m == "rel":
        off = operand - 0x100 if operand & 0x80 else operand
        return f"${(addr + 2 + off) & 0xFFFF:04X}"
    fmt = {"zp": "${:02X}", "zpx": "${:02X},X", "zpy": "${:02X},Y", "abs": "${:04X}",
           "absx": "${:04X},X", "absy": "${:04X},Y", "ind": "(${:04X})",
           "izx": "(${:02X},X)", "izy": "(${:02X}),Y"}[m]
    return fmt.format(operand)


def disasm(insn: Insn) -> str:
    s = f"{insn.op.mnemonic} {fmt_operand(insn.op, insn.operand, insn.addr)}"
    return s.strip()


@dataclass
class Cheat:
    """A runtime-toggleable patch, defined in the game config."""
    id: str
    name: str
    description: str
    returns: list[tuple[int, int]]  # (bank, addr): behave like RTS when enabled
    skips: list[tuple[int, int]]    # (bank, addr): skip the instruction when enabled


class Recompiler:
    def __init__(self, rom: Rom, noreturn: set[int] = frozenset(), cheats: list[Cheat] = ()):
        self.rom = rom
        self.noreturn = set(noreturn)
        self.cheats = list(cheats)
        # (bank, addr) -> (cheat index, "return" | "skip")
        self.patches: dict[tuple[int, int], tuple[int, str]] = {}
        for i, ch in enumerate(self.cheats):
            for key in ch.returns:
                self.patches[key] = (i, "return")
            for key in ch.skips:
                self.patches[key] = (i, "skip")

    # -- decoding -------------------------------------------------------
    def read(self, win: Window, addr: int) -> int:
        return self.rom.prg[(win.bank % self.rom.num_banks) * PRG_BANK + (addr - win.base)]

    def decode(self, win: Window, addr: int) -> Insn | None:
        op = OPCODES.get(self.read(win, addr))
        if op is None or not win.contains(addr + op.size - 1):
            return None
        operand = 0
        if op.size == 2:
            operand = self.read(win, addr + 1)
        elif op.size == 3:
            operand = self.read(win, addr + 1) | (self.read(win, addr + 2) << 8)
        return Insn(addr, op, operand)

    def patch_seeds(self) -> dict[tuple[int, int], set[int]]:
        seeds: dict[tuple[int, int], set[int]] = {}
        for bank, addr in self.patches:
            seeds.setdefault((bank, (addr >> 14) & 1), set()).add(addr)
        return seeds

    def default_seeds(self) -> dict[tuple[int, int], set[int]]:
        """Interrupt vectors of the bank mapped at $C000 on power-up."""
        last = self.rom.num_banks - 1
        win = Window(last, 1)
        seeds = set()
        for vec in (0xFFFA, 0xFFFC, 0xFFFE):
            seeds.add(self.read(win, vec) | (self.read(win, vec + 1) << 8))
        return {(last, 1): {a for a in seeds if win.contains(a)}}

    # -- analysis -------------------------------------------------------
    def analyse(self, win: Window, seeds: set[int]) -> WindowCode:
        wc = WindowCode(win)
        work = [a for a in seeds if win.contains(a)]
        wc.labels.update(work)

        def visit(a: int, label: bool):
            if not win.contains(a):
                return
            if label:
                wc.labels.add(a)
            work.append(a)

        while work:
            a = work.pop()
            if a in wc.insns or a in wc.invalid:
                continue
            insn = self.decode(win, a)
            if insn is None:
                wc.invalid.add(a)
                continue
            wc.insns[a] = insn
            mn, nxt = insn.op.mnemonic, (a + insn.op.size) & 0xFFFF
            if mn in BRANCHES:
                off = insn.operand - 0x100 if insn.operand & 0x80 else insn.operand
                visit((nxt + off) & 0xFFFF, True)
                visit(nxt, True)
            elif mn == "JMP" and insn.op.mode == "abs":
                visit(insn.operand, True)
            elif mn == "JSR":
                visit(insn.operand, True)
                if insn.operand not in self.noreturn:
                    visit(nxt, True)  # return site, entered via RTS
            elif mn not in TERMINATORS:
                visit(nxt, False)
        return wc

    # -- code generation ------------------------------------------------
    def gen_window(self, wc: WindowCode) -> str:
        win = wc.window
        order = sorted(wc.insns)
        # Any fall-through that is not physically next in the layout needs a label.
        for i, a in enumerate(order):
            insn = wc.insns[a]
            mn = insn.op.mnemonic
            if mn in BRANCHES or mn in TERMINATORS or mn == "JSR":
                continue
            nxt = a + insn.op.size
            if (i + 1 >= len(order) or order[i + 1] != nxt) and nxt in wc.insns:
                wc.labels.add(nxt)

        labels = sorted(wc.labels)
        fn = f"nes_recomp_{win.name}"
        out = [
            f"/* Generated by nesrecomp. PRG bank {win.bank} mapped at ${win.base:04X}. Do not edit. */",
            '#include "nesrt/recomp.h"',
            "",
            f"#define W {win.w}",
            f"#define BANK {win.bank}",
            "",
            f"int {fn}(nes_cpu *c) {{",
            "    uint16_t ea; (void)ea;",
            "dispatch:",
            "    if (c->quit || BANK_CHANGED) return NES_EXIT;",
            "    switch (c->pc) {",
        ]
        out += [f"    case 0x{a:04X}: goto L_{a:04X};" for a in labels]
        out += ["    default: return IN_WINDOW(c->pc) ? NES_MISS : NES_EXIT;", "    }", ""]

        for a in sorted(wc.invalid & wc.labels):
            out.append(f"L_{a:04X}: /* not decodable as code: let the interpreter handle it */")
            out.append(f"    c->pc = 0x{a:04X}; return NES_MISS;")

        for i, a in enumerate(order):
            insn = wc.insns[a]
            raw = " ".join(f"{self.read(win, a + k):02X}" for k in range(insn.op.size))
            head = f"L_{a:04X}:" if a in wc.labels else "      "
            out.append(f"{head} /* {a:04X}: {raw:<8}  {disasm(insn)} */")
            ctx = StaticCtx(a, insn.operand, insn.op, win, wc.labels)
            body = emit_instruction(insn.op, ctx)
            patch = self.patches.get((win.bank, a))
            if patch:
                idx, kind = patch
                out.append(f"    if (nes_cheats[{idx}]) {{ /* cheat: {self.cheats[idx].id} */")
                if kind == "return":
                    rts = OPCODES[0x60]
                    out += ["        " + s for s in emit_instruction(rts, StaticCtx(a, 0, rts, win, wc.labels))]
                else:
                    out.append(f"        CYC({insn.op.cycles});")
                out.append("    } else {")
                out += ["        " + s for s in body]
                out.append("    }")
            else:
                out += ["    " + s for s in body]
            mn = insn.op.mnemonic
            if mn in BRANCHES or mn in TERMINATORS or mn == "JSR":
                continue
            nxt = a + insn.op.size
            if i + 1 < len(order) and order[i + 1] == nxt:
                continue
            if nxt in wc.insns:
                out.append(f"    goto L_{nxt:04X};")
            else:
                out.append(f"    c->pc = 0x{nxt & 0xFFFF:04X}; goto dispatch;")
        out += ["}", ""]
        return "\n".join(out)

    def gen_table(self, windows: list[WindowCode]) -> str:
        out = ["/* Generated by nesrecomp. Do not edit. */",
               '#include "nesrt/recomp.h"', ""]
        def cstr(t: str) -> str:
            return '"' + t.replace("\\", "\\\\").replace('"', '\\"') + '"'
        out.append(f"const int nes_cheat_count = {len(self.cheats)};")
        out.append("const nes_cheat_def nes_cheat_defs[] = {")
        out += [f"    {{{cstr(c.id)}, {cstr(c.name)}, {cstr(c.description)}}}," for c in self.cheats]
        out += ["    {0}", "};"]
        out.append(f"const int nes_patch_count = {len(self.patches)};")
        out.append("const nes_patch_def nes_patch_defs[] = {")
        for (bank, addr), (idx, kind) in sorted(self.patches.items()):
            insn = self.decode(Window(bank, (addr >> 14) & 1), addr)
            cyc, size = (insn.op.cycles, insn.op.size) if insn else (2, 1)
            k = "NES_PATCH_RETURN" if kind == "return" else "NES_PATCH_SKIP"
            out.append(f"    {{{bank}, 0x{addr:04X}, {idx}, {k}, {cyc}, {size}}},")
        out += ["    {0}", "};", ""]
        for wc in windows:
            out.append(f"int nes_recomp_{wc.window.name}(nes_cpu *c);")
        out += ["", f"const int nes_recomp_bank_count = {self.rom.num_banks};",
                f"const uint32_t nes_recomp_prg_crc32 = 0x{self.rom.prg_crc32:08X}u;",
                f"const nes_recomp_fn nes_recomp_table[{self.rom.num_banks}][2] = {{"]
        for wc in windows:
            out.append(f"    [{wc.window.bank}][{wc.window.w}] = nes_recomp_{wc.window.name},")
        out += ["};", ""]
        return "\n".join(out)


def gen_interpreter() -> str:
    """Generate the fallback interpreter from the same semantics."""
    out = [
        "/* Generated by nesrecomp from tools/nesrecomp/semantics.py. Do not edit. */",
        '#include "nesrt/recomp.h"',
        "",
        "/* Execute one instruction at c->pc. Returns 1 after a control transfer",
        " * (an interrupt poll point), 0 otherwise. */",
        "int nes_interp_step(nes_cpu *c) {",
        "    uint16_t pc = c->pc;",
        "    uint16_t ea; (void)ea;",
        "    switch (RD(pc)) {",
    ]
    for code in sorted(OPCODES):
        op = OPCODES[code]
        out.append(f"    case 0x{code:02X}: {{ /* {op.mnemonic} {op.mode} */")
        out += ["        " + s for s in emit_instruction(op, DynamicCtx(op))]
        out.append("    }")
    out += ["    default: return nes_illegal_opcode(c);", "    }", "}", ""]
    return "\n".join(out)
