"""Official 6502 (2A03) opcode table.

This table is the single source of truth for instruction decoding and cycle
timing.  Both the static recompiler and the generated fallback interpreter
are produced from it, so the two execution paths can never disagree.
"""

from dataclasses import dataclass

# Addressing modes and their operand sizes (bytes after the opcode).
MODE_SIZE = {
    "imp": 0, "acc": 0, "imm": 1, "zp": 1, "zpx": 1, "zpy": 1,
    "abs": 2, "absx": 2, "absy": 2, "ind": 2, "izx": 1, "izy": 1, "rel": 1,
}


@dataclass(frozen=True)
class Opcode:
    code: int
    mnemonic: str
    mode: str
    cycles: int
    page_penalty: bool  # +1 cycle when an indexed read crosses a page

    @property
    def size(self) -> int:
        return 1 + MODE_SIZE[self.mode]


# mnemonic: [(opcode, mode, cycles, page_penalty), ...]
_TABLE = {
    "ADC": [(0x69, "imm", 2, 0), (0x65, "zp", 3, 0), (0x75, "zpx", 4, 0), (0x6D, "abs", 4, 0),
            (0x7D, "absx", 4, 1), (0x79, "absy", 4, 1), (0x61, "izx", 6, 0), (0x71, "izy", 5, 1)],
    "AND": [(0x29, "imm", 2, 0), (0x25, "zp", 3, 0), (0x35, "zpx", 4, 0), (0x2D, "abs", 4, 0),
            (0x3D, "absx", 4, 1), (0x39, "absy", 4, 1), (0x21, "izx", 6, 0), (0x31, "izy", 5, 1)],
    "ASL": [(0x0A, "acc", 2, 0), (0x06, "zp", 5, 0), (0x16, "zpx", 6, 0), (0x0E, "abs", 6, 0),
            (0x1E, "absx", 7, 0)],
    "BCC": [(0x90, "rel", 2, 0)], "BCS": [(0xB0, "rel", 2, 0)],
    "BEQ": [(0xF0, "rel", 2, 0)], "BMI": [(0x30, "rel", 2, 0)],
    "BNE": [(0xD0, "rel", 2, 0)], "BPL": [(0x10, "rel", 2, 0)],
    "BVC": [(0x50, "rel", 2, 0)], "BVS": [(0x70, "rel", 2, 0)],
    "BIT": [(0x24, "zp", 3, 0), (0x2C, "abs", 4, 0)],
    "BRK": [(0x00, "imp", 7, 0)],
    "CLC": [(0x18, "imp", 2, 0)], "CLD": [(0xD8, "imp", 2, 0)],
    "CLI": [(0x58, "imp", 2, 0)], "CLV": [(0xB8, "imp", 2, 0)],
    "CMP": [(0xC9, "imm", 2, 0), (0xC5, "zp", 3, 0), (0xD5, "zpx", 4, 0), (0xCD, "abs", 4, 0),
            (0xDD, "absx", 4, 1), (0xD9, "absy", 4, 1), (0xC1, "izx", 6, 0), (0xD1, "izy", 5, 1)],
    "CPX": [(0xE0, "imm", 2, 0), (0xE4, "zp", 3, 0), (0xEC, "abs", 4, 0)],
    "CPY": [(0xC0, "imm", 2, 0), (0xC4, "zp", 3, 0), (0xCC, "abs", 4, 0)],
    "DEC": [(0xC6, "zp", 5, 0), (0xD6, "zpx", 6, 0), (0xCE, "abs", 6, 0), (0xDE, "absx", 7, 0)],
    "DEX": [(0xCA, "imp", 2, 0)], "DEY": [(0x88, "imp", 2, 0)],
    "EOR": [(0x49, "imm", 2, 0), (0x45, "zp", 3, 0), (0x55, "zpx", 4, 0), (0x4D, "abs", 4, 0),
            (0x5D, "absx", 4, 1), (0x59, "absy", 4, 1), (0x41, "izx", 6, 0), (0x51, "izy", 5, 1)],
    "INC": [(0xE6, "zp", 5, 0), (0xF6, "zpx", 6, 0), (0xEE, "abs", 6, 0), (0xFE, "absx", 7, 0)],
    "INX": [(0xE8, "imp", 2, 0)], "INY": [(0xC8, "imp", 2, 0)],
    "JMP": [(0x4C, "abs", 3, 0), (0x6C, "ind", 5, 0)],
    "JSR": [(0x20, "abs", 6, 0)],
    "LDA": [(0xA9, "imm", 2, 0), (0xA5, "zp", 3, 0), (0xB5, "zpx", 4, 0), (0xAD, "abs", 4, 0),
            (0xBD, "absx", 4, 1), (0xB9, "absy", 4, 1), (0xA1, "izx", 6, 0), (0xB1, "izy", 5, 1)],
    "LDX": [(0xA2, "imm", 2, 0), (0xA6, "zp", 3, 0), (0xB6, "zpy", 4, 0), (0xAE, "abs", 4, 0),
            (0xBE, "absy", 4, 1)],
    "LDY": [(0xA0, "imm", 2, 0), (0xA4, "zp", 3, 0), (0xB4, "zpx", 4, 0), (0xAC, "abs", 4, 0),
            (0xBC, "absx", 4, 1)],
    "LSR": [(0x4A, "acc", 2, 0), (0x46, "zp", 5, 0), (0x56, "zpx", 6, 0), (0x4E, "abs", 6, 0),
            (0x5E, "absx", 7, 0)],
    "NOP": [(0xEA, "imp", 2, 0)],
    "ORA": [(0x09, "imm", 2, 0), (0x05, "zp", 3, 0), (0x15, "zpx", 4, 0), (0x0D, "abs", 4, 0),
            (0x1D, "absx", 4, 1), (0x19, "absy", 4, 1), (0x01, "izx", 6, 0), (0x11, "izy", 5, 1)],
    "PHA": [(0x48, "imp", 3, 0)], "PHP": [(0x08, "imp", 3, 0)],
    "PLA": [(0x68, "imp", 4, 0)], "PLP": [(0x28, "imp", 4, 0)],
    "ROL": [(0x2A, "acc", 2, 0), (0x26, "zp", 5, 0), (0x36, "zpx", 6, 0), (0x2E, "abs", 6, 0),
            (0x3E, "absx", 7, 0)],
    "ROR": [(0x6A, "acc", 2, 0), (0x66, "zp", 5, 0), (0x76, "zpx", 6, 0), (0x6E, "abs", 6, 0),
            (0x7E, "absx", 7, 0)],
    "RTI": [(0x40, "imp", 6, 0)], "RTS": [(0x60, "imp", 6, 0)],
    "SBC": [(0xE9, "imm", 2, 0), (0xE5, "zp", 3, 0), (0xF5, "zpx", 4, 0), (0xED, "abs", 4, 0),
            (0xFD, "absx", 4, 1), (0xF9, "absy", 4, 1), (0xE1, "izx", 6, 0), (0xF1, "izy", 5, 1)],
    "SEC": [(0x38, "imp", 2, 0)], "SED": [(0xF8, "imp", 2, 0)], "SEI": [(0x78, "imp", 2, 0)],
    "STA": [(0x85, "zp", 3, 0), (0x95, "zpx", 4, 0), (0x8D, "abs", 4, 0), (0x9D, "absx", 5, 0),
            (0x99, "absy", 5, 0), (0x81, "izx", 6, 0), (0x91, "izy", 6, 0)],
    "STX": [(0x86, "zp", 3, 0), (0x96, "zpy", 4, 0), (0x8E, "abs", 4, 0)],
    "STY": [(0x84, "zp", 3, 0), (0x94, "zpx", 4, 0), (0x8C, "abs", 4, 0)],
    "TAX": [(0xAA, "imp", 2, 0)], "TAY": [(0xA8, "imp", 2, 0)],
    "TSX": [(0xBA, "imp", 2, 0)], "TXA": [(0x8A, "imp", 2, 0)],
    "TXS": [(0x9A, "imp", 2, 0)], "TYA": [(0x98, "imp", 2, 0)],
}

OPCODES: dict[int, Opcode] = {}
for _mn, _entries in _TABLE.items():
    for _code, _mode, _cyc, _pp in _entries:
        assert _code not in OPCODES, hex(_code)
        OPCODES[_code] = Opcode(_code, _mn, _mode, _cyc, bool(_pp))

BRANCHES = {"BCC", "BCS", "BEQ", "BMI", "BNE", "BPL", "BVC", "BVS"}
# Instructions after which control never falls through to the next byte.
TERMINATORS = {"JMP", "RTS", "RTI", "BRK"}
# Instructions that end an interpreter block / are interrupt poll points.
CONTROL = BRANCHES | TERMINATORS | {"JSR"}
