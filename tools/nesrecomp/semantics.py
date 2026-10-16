"""6502 instruction semantics -> C.

Every instruction is lowered by `emit_instruction`, which is parameterised by
an *addressing context*:

* `StaticCtx`  - used by the recompiler.  The instruction address and operand
  bytes are known at build time, so addresses fold to constants and control
  flow becomes `goto` between labels.
* `DynamicCtx` - used to generate the fallback interpreter.  Operands are
  fetched from the bus at runtime and control flow updates `c->pc`.

Because both are produced from the same code, recompiled and interpreted
execution are cycle-for-cycle identical, which is what makes the
differential test (`tests/diff_test.py`) meaningful.
"""

from .opcodes import Opcode, BRANCHES

BRANCH_COND = {
    "BCC": "!c->fc", "BCS": "c->fc", "BNE": "!c->fz", "BEQ": "c->fz",
    "BPL": "!c->fn", "BMI": "c->fn", "BVC": "!c->fv", "BVS": "c->fv",
}

ALU = {"ORA": "|", "AND": "&", "EOR": "^"}
RMW = {"ASL": "op_asl", "LSR": "op_lsr", "ROL": "op_rol", "ROR": "op_ror",
       "INC": "op_inc", "DEC": "op_dec"}
LOADS = {"LDA": "c->a", "LDX": "c->x", "LDY": "c->y"}
STORES = {"STA": "c->a", "STX": "c->x", "STY": "c->y"}
COMPARES = {"CMP": "c->a", "CPX": "c->x", "CPY": "c->y"}
TRANSFERS = {"TAX": ("c->x", "c->a"), "TAY": ("c->y", "c->a"), "TXA": ("c->a", "c->x"),
             "TYA": ("c->a", "c->y"), "TSX": ("c->x", "c->s")}
FLAGS = {"CLC": "c->fc = 0;", "SEC": "c->fc = 1;", "CLI": "c->fi = 0;", "SEI": "c->fi = 1;",
         "CLV": "c->fv = 0;", "CLD": "c->fd = 0;", "SED": "c->fd = 1;"}


class StaticCtx:
    """Addressing/control-flow lowering for a known instruction address."""

    def __init__(self, pc: int, operand: int, op: Opcode, window, labels: set[int]):
        self.pc = pc
        self.operand = operand
        self.op = op
        self.window = window  # object with .contains(addr)
        self.labels = labels

    @property
    def next_pc(self) -> str:
        return f"0x{(self.pc + self.op.size) & 0xFFFF:04X}"

    # -- operands -------------------------------------------------------
    def setup(self) -> list[str]:
        m, o = self.op.mode, self.operand
        if m in ("zpx", "zpy"):
            r = "c->x" if m == "zpx" else "c->y"
            return [f"ea = (uint8_t)(0x{o:02X} + {r});"]
        if m in ("absx", "absy"):
            r = "c->x" if m == "absx" else "c->y"
            out = [f"ea = (uint16_t)(0x{o:04X} + {r});"]
            if self.op.page_penalty:
                out.append(f"CYC((0x{o & 0xFF:02X} + {r}) >> 8);")
            return out
        if m == "izx":
            return [f"ea = ZP16((uint8_t)(0x{o:02X} + c->x));"]
        if m == "izy":
            out = [f"ea = ZP16(0x{o:02X});"]
            if self.op.page_penalty:
                out.append("CYC(((ea & 0xFF) + c->y) >> 8);")
            out.append("ea = (uint16_t)(ea + c->y);")
            return out
        return []

    def read(self) -> str:
        m, o = self.op.mode, self.operand
        if m == "imm":
            return f"0x{o:02X}"
        if m == "zp":
            return f"RAM[0x{o:02X}]"
        if m in ("zpx", "zpy"):
            return "RAM[ea]"
        if m == "abs":
            if o < 0x2000:
                return f"RAM[0x{o & 0x7FF:03X}]"
            if o >= 0x8000:
                return f"PRG(0x{o:04X})"
            return f"RD(0x{o:04X})"
        return "RD(ea)"

    def is_ram_operand(self) -> bool:
        m, o = self.op.mode, self.operand
        return m in ("zp", "zpx", "zpy") or (m == "abs" and o < 0x2000)

    def write(self, v: str) -> list[str]:
        m, o = self.op.mode, self.operand
        if m == "zp":
            return [f"RAM[0x{o:02X}] = {v};"]
        if m in ("zpx", "zpy"):
            return [f"RAM[ea] = {v};"]
        if m == "abs":
            if o < 0x2000:
                return [f"RAM[0x{o & 0x7FF:03X}] = {v};"]
            return [f"WR(0x{o:04X}, {v});"]
        return [f"WR(ea, {v});"]

    def may_write_rom(self) -> bool:
        """Could this store hit $8000-$FFFF (i.e. a mapper register)?"""
        m, o = self.op.mode, self.operand
        if m in ("zp", "zpx", "zpy"):
            return False
        if m == "abs":
            return o >= 0x8000
        if m in ("absx", "absy"):
            return o + 0xFF >= 0x8000
        return True

    def after_write(self) -> list[str]:
        # A mapper write may have swapped out the bank we are executing from.
        if self.may_write_rom():
            return [f"if (BANK_CHANGED) {{ c->pc = {self.next_pc}; return NES_EXIT; }}"]
        return []

    # -- control flow ---------------------------------------------------
    def jump_const(self, target: int) -> list[str]:
        if self.window.contains(target) and target in self.labels:
            return [f"POLL_GOTO(0x{target:04X}, L_{target:04X});"]
        return [f"POLL_JUMP(0x{target:04X});"]

    def jump_dyn(self, expr: str) -> list[str]:
        return [f"POLL_JUMP({expr});"]

    def fallthrough(self) -> list[str]:
        return []  # handled by the code generator's block layout

    def branch(self, cond: str) -> list[str]:
        nxt = (self.pc + 2) & 0xFFFF
        off = self.operand - 0x100 if self.operand & 0x80 else self.operand
        tgt = (nxt + off) & 0xFFFF
        extra = 2 if (tgt & 0xFF00) != (nxt & 0xFF00) else 1
        return ([f"if ({cond}) {{ CYC({extra});"]
                 + ["    " + s for s in self.jump_const(tgt)] + ["}"]
                 + self.jump_const(nxt))

    def jsr_target(self) -> int:
        return self.operand

    def operand16(self) -> str:
        return f"0x{self.operand:04X}"

    def ret_addr(self) -> str:  # JSR pushes the address of its last byte
        return f"0x{(self.pc + 2) & 0xFFFF:04X}"


class DynamicCtx:
    """Addressing/control-flow lowering for the generated interpreter."""

    def __init__(self, op: Opcode):
        self.op = op

    @property
    def next_pc(self) -> str:
        return f"(uint16_t)(pc + {self.op.size})"

    def setup(self) -> list[str]:
        m = self.op.mode
        b1 = "RD((uint16_t)(pc + 1))"
        w1 = "RD16((uint16_t)(pc + 1))"
        if m == "zp":
            return [f"ea = {b1};"]
        if m in ("zpx", "zpy"):
            r = "c->x" if m == "zpx" else "c->y"
            return [f"ea = (uint8_t)({b1} + {r});"]
        if m == "abs":
            return [f"ea = {w1};"]
        if m in ("absx", "absy"):
            r = "c->x" if m == "absx" else "c->y"
            out = [f"ea = {w1};"]
            if self.op.page_penalty:
                out.append(f"CYC(((ea & 0xFF) + {r}) >> 8);")
            out.append(f"ea = (uint16_t)(ea + {r});")
            return out
        if m == "izx":
            return [f"ea = ZP16((uint8_t)({b1} + c->x));"]
        if m == "izy":
            out = [f"ea = ZP16({b1});"]
            if self.op.page_penalty:
                out.append("CYC(((ea & 0xFF) + c->y) >> 8);")
            out.append("ea = (uint16_t)(ea + c->y);")
            return out
        return []

    def read(self) -> str:
        m = self.op.mode
        if m == "imm":
            return "RD((uint16_t)(pc + 1))"
        if m in ("zp", "zpx", "zpy"):
            return "RAM[ea]"
        return "RD(ea)"

    def is_ram_operand(self) -> bool:
        return self.op.mode in ("zp", "zpx", "zpy")

    def write(self, v: str) -> list[str]:
        if self.op.mode in ("zp", "zpx", "zpy"):
            return [f"RAM[ea] = {v};"]
        return [f"WR(ea, {v});"]

    def after_write(self) -> list[str]:
        return []

    def jump_const(self, target: str) -> list[str]:
        return [f"c->pc = {target}; POLL(); return 1;"]

    jump_dyn = jump_const

    def fallthrough(self) -> list[str]:
        return [f"c->pc = {self.next_pc}; return 0;"]

    def branch(self, cond: str) -> list[str]:
        return [f"if ({cond}) {{",
                f"    uint16_t n_ = {self.next_pc};",
                "    uint16_t t_ = (uint16_t)(n_ + (int8_t)RD((uint16_t)(pc + 1)));",
                "    CYC(((t_ ^ n_) & 0xFF00) ? 2 : 1);",
                "    c->pc = t_; POLL(); return 1;",
                "}",
                f"c->pc = {self.next_pc}; POLL(); return 1;"]

    def operand16(self) -> str:
        return "RD16((uint16_t)(pc + 1))"

    def ret_addr(self) -> str:
        return "(uint16_t)(pc + 2)"


def emit_instruction(op: Opcode, ctx) -> list[str]:
    """Return the C statements implementing one instruction."""
    mn = op.mnemonic
    out = [f"CYC({op.cycles});"]
    out += ctx.setup()

    if mn in LOADS:
        r = LOADS[mn]
        out += [f"{r} = {ctx.read()}; NZ({r});"]
    elif mn in STORES:
        out += ctx.write(STORES[mn]) + ctx.after_write()
    elif mn in ALU:
        out += [f"c->a {ALU[mn]}= {ctx.read()}; NZ(c->a);"]
    elif mn == "ADC":
        out += [f"op_adc(c, {ctx.read()});"]
    elif mn == "SBC":
        out += [f"op_adc(c, (uint8_t)~{ctx.read()});"]
    elif mn in COMPARES:
        out += [f"op_cmp(c, {COMPARES[mn]}, {ctx.read()});"]
    elif mn == "BIT":
        out += [f"op_bit(c, {ctx.read()});"]
    elif mn in RMW:
        fn = RMW[mn]
        if op.mode == "acc":
            out += [f"c->a = {fn}(c, c->a);"]
        else:
            out += [f"{{ uint8_t v_ = {ctx.read()};"]
            if not ctx.is_ram_operand():
                out += ["  " + s for s in ctx.write("v_")]  # 6502 RMW dummy write
            out += [f"  v_ = {fn}(c, v_);"]
            out += ["  " + s for s in ctx.write("v_")] + ["}"]
            out += ctx.after_write()
    elif mn in ("INX", "INY", "DEX", "DEY"):
        r = "c->x" if mn[2] == "X" else "c->y"
        d = "+ 1" if mn[0] == "I" else "- 1"
        out += [f"{r} = (uint8_t)({r} {d}); NZ({r});"]
    elif mn in TRANSFERS:
        dst, src = TRANSFERS[mn]
        out += [f"{dst} = {src}; NZ({dst});"]
    elif mn == "TXS":
        out += ["c->s = c->x;"]
    elif mn in FLAGS:
        out += [FLAGS[mn]]
    elif mn == "NOP":
        pass
    elif mn == "PHA":
        out += ["PUSH(c->a);"]
    elif mn == "PHP":
        out += ["PUSH(nes_get_p(c) | 0x30);"]
    elif mn == "PLA":
        out += ["c->a = POP(); NZ(c->a);"]
    elif mn == "PLP":
        out += ["nes_set_p(c, POP());"]
    elif mn in BRANCHES:
        return out + ctx.branch(BRANCH_COND[mn])
    elif mn == "JMP":
        if op.mode == "abs":
            if isinstance(ctx, StaticCtx):
                return out + ctx.jump_const(ctx.operand)
            return out + ctx.jump_dyn(ctx.operand16())
        # JMP ($xxFF) wraps within the page (6502 bug).
        return out + [f"ea = {ctx.operand16()};",
                      "{ uint16_t t_ = RD(ea) | (RD((ea & 0xFF00) | ((ea + 1) & 0xFF)) << 8);"] \
            + ["  " + s for s in ctx.jump_dyn("t_")] + ["}"]
    elif mn == "JSR":
        out += [f"PUSH16({ctx.ret_addr()});"]
        if isinstance(ctx, StaticCtx):
            return out + ctx.jump_const(ctx.operand)
        return out + ["{ uint16_t t_ = RD16((uint16_t)(pc + 1));"] + \
            ["  " + s for s in ctx.jump_dyn("t_")] + ["}"]
    elif mn == "RTS":
        return out + ["{ uint16_t t_ = (uint16_t)(POP16() + 1);"] + \
            ["  " + s for s in ctx.jump_dyn("t_")] + ["}"]
    elif mn == "RTI":
        return out + ["nes_set_p(c, POP());", "{ uint16_t t_ = POP16();"] + \
            ["  " + s for s in ctx.jump_dyn("t_")] + ["}"]
    elif mn == "BRK":
        return out + [f"PUSH16((uint16_t)({ctx.ret_addr()}));",
                      "PUSH(nes_get_p(c) | 0x30); c->fi = 1;",
                      "{ uint16_t t_ = RD16(0xFFFE);"] + \
            ["  " + s for s in ctx.jump_dyn("t_")] + ["}"]
    else:
        raise ValueError(f"unhandled mnemonic {mn}")

    return out + ctx.fallthrough()
