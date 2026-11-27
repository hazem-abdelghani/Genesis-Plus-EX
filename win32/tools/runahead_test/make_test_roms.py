#!/usr/bin/env python3
"""Makes the small test ROMs the Run-Ahead tests use (hand-assembled machine code; no other tools needed).

   python3 make_test_roms.py            -> test_snd.bin test_fm.bin test_both.bin test_vint.bin test_dac.bin test_il.bin
                                           test.gg test_loop.gg test_poll.gg (and input_lag_test.md from make_input_lag_rom.py)
"""
import struct, subprocess, sys, os

def md_rom(code, vint=None):
    rom = bytearray(0x1000)
    struct.pack_into(">I", rom, 0, 0x00FF0000)
    for i in range(1, 64): struct.pack_into(">I", rom, i * 4, 0x200)
    if vint is not None: struct.pack_into(">I", rom, 30 * 4, 0x300)
    rom[0x100:0x110] = b"SEGA MEGA DRIVE "
    rom[0x110:0x120] = b"(C)TEST 2026.NOV "[:16]
    rom[0x120:0x150] = b"TEST ROM".ljust(48)
    rom[0x150:0x180] = b"TEST ROM".ljust(48)
    rom[0x180:0x18E] = b"GM 00000000-00"
    rom[0x190:0x1A0] = b"J               "
    struct.pack_into(">I", rom, 0x1A0, 0); struct.pack_into(">I", rom, 0x1A4, 0xFFF)
    struct.pack_into(">I", rom, 0x1A8, 0xFF0000); struct.pack_into(">I", rom, 0x1AC, 0xFFFFFF)
    rom[0x1F0:0x1F3] = b"JUE"
    rom[0x200:0x200 + len(code)] = code
    if vint is not None: rom[0x300:0x300 + len(vint)] = vint
    return rom

def vdp_init(c):
    for r in (0x8004, 0x8164, 0x8230, 0x8407, 0x8578, 0x8700, 0x8C81, 0x8D3F, 0x8F02, 0x9001, 0x9100, 0x9200):
        c += struct.pack(">HHI", 0x33FC, r, 0x00C00004)
    c += struct.pack(">HII", 0x23FC, 0xC0000000, 0x00C00004)
    c += struct.pack(">HHI", 0x33FC, 0x0E00, 0x00C00000)

def psg(c, vals):
    for b in vals: c += struct.pack(">HHI", 0x13FC, b, 0x00C00011)

def fm_setup(c):
    c += struct.pack(">HHI", 0x33FC, 0x0100, 0x00A11200)
    c += struct.pack(">HHI", 0x33FC, 0x0100, 0x00A11100)
    for r, v in ((0x30,1),(0x34,1),(0x38,1),(0x3C,1),(0x40,0x18),(0x44,0x7F),(0x48,0x7F),(0x4C,0x7F),(0x50,0x1F),(0x54,0x1F),(0x58,0x1F),(0x5C,0x1F),
                 (0x60,0),(0x64,0),(0x68,0),(0x6C,0),(0x70,0),(0x74,0),(0x78,0),(0x7C,0),(0x80,0x0F),(0x84,0x0F),(0x88,0x0F),(0x8C,0x0F),
                 (0xB0,0x07),(0xB4,0xC0),(0xA4,0x22),(0xA0,0x69),(0x28,0xF0)):
        c += struct.pack(">HHI", 0x13FC, r, 0x00A04000)
        c += struct.pack(">HHI", 0x13FC, v, 0x00A04001)

BRA = struct.pack(">H", 0x60FE)

# a plain PSG tone
c = bytearray(); vdp_init(c); psg(c, (0x85, 0x0F, 0x90, 0xA5, 0x07, 0xB2)); c += BRA
open("test_snd.bin", "wb").write(md_rom(c))
# a plain FM tone
c = bytearray(); vdp_init(c); fm_setup(c); c += BRA
open("test_fm.bin", "wb").write(md_rom(c))
# both
c = bytearray(); vdp_init(c); fm_setup(c); psg(c, (0x85, 0x0F, 0x92, 0xA5, 0x07, 0xB4)); c += BRA
open("test_both.bin", "wb").write(md_rom(c))
# both, changed in every frame's interrupt (random PSG bytes, FM pitch)
c = bytearray(); vdp_init(c); fm_setup(c); psg(c, (0x85, 0x0F, 0x92, 0xA5, 0x07, 0xB4)); c += struct.pack(">HH", 0x46FC, 0x2000) + BRA
h = bytearray()
h += struct.pack(">HI", 0x5239, 0x00FF0000); h += struct.pack(">HI", 0x1039, 0x00FF0000); h += struct.pack(">HI", 0x13C0, 0x00C00011)
h += struct.pack(">HHI", 0x13FC, 0x00A0, 0x00A04000); h += struct.pack(">HI", 0x13C0, 0x00A04001)
h += struct.pack(">HHI", 0x13FC, 0x0028, 0x00A04000); h += struct.pack(">HHI", 0x13FC, 0x00F0, 0x00A04001); h += struct.pack(">H", 0x4E73)
open("test_vint.bin", "wb").write(md_rom(c, h))
# a sample played by hand in a tight loop (the SEGA chime kind): the 68000's timing matters
c = bytearray(); vdp_init(c); fm_setup(c)
c += struct.pack(">HHI", 0x13FC, 0x002B, 0x00A04000); c += struct.pack(">HHI", 0x13FC, 0x0080, 0x00A04001)
c += struct.pack(">HHI", 0x13FC, 0x002A, 0x00A04000)
c += struct.pack(">H", 0x7000) + struct.pack(">HH", 0x0600, 0x000D) + struct.pack(">HI", 0x13C0, 0x00A04001) + struct.pack(">H", 0x60F4)
open("test_dac.bin", "wb").write(md_rom(c))
# interlaced picture (for the picture tests)
c = bytearray(); vdp_init(c); c += BRA
r = md_rom(c); i = r.find(struct.pack(">H", 0x8C81)); r[i:i+2] = struct.pack(">H", 0x8C87)
open("test_il.bin", "wb").write(r)

# Game Gear (Z80): PSG from the interrupt, from a tight loop, by polling the vertical blank
def gg(body_after_init, handler=None):
    rom = bytearray(0x8000)
    code = bytearray([0xF3, 0xED, 0x56, 0x31, 0xF0, 0xDF])
    for val, reg in ((0x04, 0), (0x60, 1)): code += bytes([0x3E, val, 0xD3, 0xBF, 0x3E, 0x80 | reg, 0xD3, 0xBF])
    for b in (0x85, 0x0F, 0x90): code += bytes([0x3E, b, 0xD3, 0x7F])
    code += body_after_init
    rom[0:len(code)] = code
    if handler: rom[0x38:0x38 + len(handler)] = handler
    rom[0x7FF0:0x7FF8] = b"TMR SEGA"; rom[0x7FFF] = 0x6C
    return rom
irq = bytes([0x3A,0x00,0xC0,0x3C,0x32,0x00,0xC0,0xD3,0x7F,0xDB,0xBF,0xFB,0xED,0x4D])
open("test.gg", "wb").write(gg(bytes([0xFB, 0x18, 0xFE]), irq))
open("test_loop.gg", "wb").write(gg(bytes([0xFB, 0x3E, 0x90, 0x3C, 0xD3, 0x7F, 0x18, 0xFB])))
open("test_poll.gg", "wb").write(gg(bytes([0xDB,0xBF,0xE6,0x80,0x28,0xFA, 0x3A,0x00,0xC0,0x3C,0x32,0x00,0xC0,0xD3,0x7F, 0x18,0xF0])))

subprocess.check_call([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_input_lag_rom.py")])
print("test ROMs made")
