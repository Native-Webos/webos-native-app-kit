# SPDX-License-Identifier: GPL-3.0-or-later
"""Minimal ELF reader for packaging checks: header facts, dynamic tags and dynamic symbols.

Stdlib only, so package checks run on any host without the NDK's binutils. Supports ELF32 and
ELF64, little and big endian. Reads section headers (.dynsym/.dynstr/.dynamic/.gnu.version_r);
a stripped binary keeps these because the dynamic loader needs them.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

EM_ARM = 40
EM_X86_64 = 62
ET_EXEC, ET_DYN = 2, 3

DT_NULL, DT_NEEDED, DT_STRTAB, DT_SONAME, DT_RPATH, DT_RUNPATH = 0, 1, 5, 14, 15, 29

SHT_DYNSYM, SHT_DYNAMIC, SHT_GNU_VERNEED = 11, 6, 0x6FFFFFFE

STB_LOCAL, STB_GLOBAL, STB_WEAK = 0, 1, 2
SHN_UNDEF = 0


class ElfError(ValueError):
    pass


@dataclass
class DynSymbol:
    name: str
    defined: bool
    binding: int  # STB_*
    type: int


@dataclass
class ElfInfo:
    elf_class: int  # 32 or 64
    little_endian: bool
    machine: int
    elf_type: int
    needed: list[str] = field(default_factory=list)
    soname: str | None = None
    rpath: list[str] = field(default_factory=list)
    runpath: list[str] = field(default_factory=list)
    symbols: list[DynSymbol] = field(default_factory=list)
    # (library file, version name) pairs from .gnu.version_r, e.g. ("libc.so.6", "GLIBC_2.4")
    version_needs: list[tuple[str, str]] = field(default_factory=list)
    arm_eabi_version: int | None = None
    arm_hard_float: bool | None = None

    @property
    def undefined(self) -> list[DynSymbol]:
        return [s for s in self.symbols if not s.defined and s.name]

    @property
    def exported(self) -> list[DynSymbol]:
        return [s for s in self.symbols if s.defined and s.name and s.binding != STB_LOCAL]


def is_elf(blob: bytes) -> bool:
    return blob[:4] == b"\x7fELF"


def _cstr(blob: bytes, off: int) -> str:
    end = blob.find(b"\0", off)
    if end < 0:
        raise ElfError("unterminated string")
    return blob[off:end].decode("utf-8", "replace")


def parse(blob: bytes) -> ElfInfo:
    if not is_elf(blob) or len(blob) < 52:
        raise ElfError("not an ELF file")
    ei_class, ei_data = blob[4], blob[5]
    if ei_class not in (1, 2) or ei_data not in (1, 2):
        raise ElfError("bad ELF class/data")
    e = "<" if ei_data == 1 else ">"
    is64 = ei_class == 2
    if is64:
        (e_type, e_machine, _ver, _entry, _phoff, e_shoff, e_flags, _ehsize, _phentsize, _phnum,
         e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from(e + "HHIQQQIHHHHHH", blob, 16)
    else:
        (e_type, e_machine, _ver, _entry, _phoff, e_shoff, e_flags, _ehsize, _phentsize, _phnum,
         e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from(e + "HHIIIIIHHHHHH", blob, 16)
    info = ElfInfo(elf_class=64 if is64 else 32, little_endian=ei_data == 1, machine=e_machine,
                   elf_type=e_type)
    if e_machine == EM_ARM:
        info.arm_eabi_version = (e_flags >> 24) & 0xFF
        info.arm_hard_float = bool(e_flags & 0x400)  # EF_ARM_ABI_FLOAT_HARD

    sections = []
    shfmt = e + ("IIQQQQIIQQ" if is64 else "IIIIIIIIII")
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        if off + e_shentsize > len(blob):
            raise ElfError("section header out of range")
        (sh_name, sh_type, _flags, _addr, sh_offset, sh_size, sh_link, sh_info, _align,
         sh_entsize) = struct.unpack_from(shfmt, blob, off)
        sections.append((sh_name, sh_type, sh_offset, sh_size, sh_link, sh_info, sh_entsize))

    def section_bytes(idx: int) -> bytes:
        _n, _t, o, s, *_ = sections[idx]
        return blob[o:o + s]

    for (_n, sh_type, sh_offset, sh_size, sh_link, sh_info, sh_entsize) in sections:
        if sh_type == SHT_DYNAMIC:
            strtab = section_bytes(sh_link)
            dfmt = e + ("qQ" if is64 else "iI")
            step = struct.calcsize(dfmt)
            for p in range(sh_offset, sh_offset + sh_size, step):
                tag, val = struct.unpack_from(dfmt, blob, p)
                if tag == DT_NULL:
                    break
                if tag == DT_NEEDED:
                    info.needed.append(_cstr(strtab, val))
                elif tag == DT_SONAME:
                    info.soname = _cstr(strtab, val)
                elif tag == DT_RPATH:
                    info.rpath.extend(x for x in _cstr(strtab, val).split(":") if x)
                elif tag == DT_RUNPATH:
                    info.runpath.extend(x for x in _cstr(strtab, val).split(":") if x)
        elif sh_type == SHT_DYNSYM:
            strtab = section_bytes(sh_link)
            if is64:
                sfmt, size = e + "IBBHQQ", 24
            else:
                sfmt, size = e + "IIIBBH", 16
            for p in range(sh_offset, sh_offset + sh_size, sh_entsize or size):
                if is64:
                    st_name, st_info, _other, st_shndx, _value, _size = struct.unpack_from(sfmt, blob, p)
                else:
                    st_name, _value, _size, st_info, _other, st_shndx = struct.unpack_from(sfmt, blob, p)
                info.symbols.append(DynSymbol(name=_cstr(strtab, st_name), defined=st_shndx != SHN_UNDEF,
                                              binding=st_info >> 4, type=st_info & 0xF))
        elif sh_type == SHT_GNU_VERNEED:
            strtab = section_bytes(sh_link)
            p = sh_offset
            for _ in range(sh_info):
                _vn_version, vn_cnt, vn_file, vn_aux, vn_next = struct.unpack_from(e + "HHIII", blob, p)
                lib = _cstr(strtab, vn_file)
                a = p + vn_aux
                for _ in range(vn_cnt):
                    _hash, _flags, _other, vna_name, vna_next = struct.unpack_from(e + "IHHII", blob, a)
                    info.version_needs.append((lib, _cstr(strtab, vna_name)))
                    if vna_next == 0:
                        break
                    a += vna_next
                if vn_next == 0:
                    break
                p += vn_next
    return info
