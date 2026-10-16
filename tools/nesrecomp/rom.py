"""iNES / NES 2.0 ROM parsing."""

import hashlib
import zlib
from dataclasses import dataclass

PRG_BANK = 0x4000  # recompilation windows are 16 KiB


@dataclass
class Rom:
    prg: bytes
    chr: bytes
    mapper: int
    sha1: str
    prg_crc32: int

    @property
    def num_banks(self) -> int:
        return max(1, len(self.prg) // PRG_BANK)

    @classmethod
    def load(cls, path: str) -> "Rom":
        data = open(path, "rb").read()
        if data[:4] != b"NES\x1a":
            raise ValueError(f"{path}: not an iNES file")
        prg_size = data[4] * 0x4000
        chr_size = data[5] * 0x2000
        mapper = (data[6] >> 4) | (data[7] & 0xF0)
        if (data[7] & 0x0C) == 0x08:  # NES 2.0
            prg_size |= (data[9] & 0x0F) << 22
            chr_size |= (data[9] & 0xF0) << 17
            mapper |= (data[8] & 0x0F) << 8
        off = 16 + (512 if data[6] & 0x04 else 0)
        prg = data[off:off + prg_size]
        chr_ = data[off + prg_size:off + prg_size + chr_size]
        if len(prg) != prg_size:
            raise ValueError(f"{path}: truncated PRG ROM")
        return cls(prg, chr_, mapper, hashlib.sha1(data).hexdigest(), zlib.crc32(prg))
