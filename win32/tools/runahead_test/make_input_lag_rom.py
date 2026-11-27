#!/usr/bin/env python3
# make_input_lag_rom.py -- builds input_lag_test.md, a tiny Mega Drive / Genesis ROM for trying Run-Ahead (Genesis Plus EX).
# It is a hand-assembled 68000 program: python3 make_input_lag_rom.py
import struct
class A:
    def __init__(s): s.b=bytearray(); s.labels={}; s.fix=[]
    def w(s,*v):
        for x in v: s.b+=struct.pack(">H",x&0xFFFF)
    def l(s,x): s.b+=struct.pack(">I",x&0xFFFFFFFF)
    def label(s,n): s.labels[n]=len(s.b)
    def nop(s): s.w(0x4E71)
    def rte(s): s.w(0x4E73)
    def move_w_imm_abs(s,i,a): s.w(0x33FC,i); s.l(a)
    def move_l_imm_abs(s,i,a): s.w(0x23FC); s.l(i); s.l(a)
    def move_b_imm_abs(s,i,a): s.w(0x13FC,i&0xFF); s.l(a)
    def move_b_abs_d(s,a,n): s.w(0x1039|(n<<9)); s.l(a)
    def move_b_d_abs(s,n,a): s.w(0x13C0|n); s.l(a)
    def move_b_abs_abs(s,src,dst): s.w(0x13F9); s.l(src); s.l(dst)
    def move_b_d_d(s,n,m): s.w(0x1000|(m<<9)|n)
    def moveq(s,i,n): s.w(0x7000|(n<<9)|(i&0xFF))
    def andi_b(s,i,n): s.w(0x0200|n,i&0xFF)
    def cmpi_b_d(s,i,n): s.w(0x0C00|n,i&0xFF)
    def cmpi_b_abs(s,i,a): s.w(0x0C39,i&0xFF); s.l(a)
    def tst_b_abs(s,a): s.w(0x4A39); s.l(a)
    def btst(s,bit,n): s.w(0x0800|n,bit)
    def addq_b_abs(s,a): s.w(0x5239); s.l(a)
    def sr(s,v): s.w(0x46FC,v)
    def br(s,op,label): s.fix.append((len(s.b),label,op)); s.w(op<<8)   # short branch
    def bra(s,lab): s.br(0x60,lab)
    def beq(s,lab): s.br(0x67,lab)
    def bne(s,lab): s.br(0x66,lab)
    def bcs(s,lab): s.br(0x65,lab)
    def done(s):
        for pos,lab,op in s.fix:
            d=s.labels[lab]-(pos+2)
            assert -128<=d<=126 and d!=0, (lab,d)
            s.b[pos]=op; s.b[pos+1]=d&0xFF
        return bytes(s.b)

VD=0x00C00000; VC=0x00C00004
RAM=0xFF0000
def cmd_vram(addr): return 0x40000000 | ((addr&0x3FFF)<<16) | ((addr>>14)&3)
def cmd_cram(addr): return 0xC0000000 | ((addr&0x3FFF)<<16)

# ---- start-up code (at 0x200): the video chip, three colored tiles, the pad port, interrupts on
s=A()
for r in (0x8004,0x8164,0x8230,0x8407,0x8578,0x8700,0x8C81,0x8D3F,0x8F02,0x9001,0x9100,0x9200): s.move_w_imm_abs(r,VC)
s.move_l_imm_abs(cmd_cram(0),VC)
for c in (0x0000,0x000E,0x00E0,0x0E00): s.move_w_imm_abs(c,VD)
for tile,pix in ((1,0x11111111),(2,0x22222222),(3,0x33333333)):
    s.move_l_imm_abs(cmd_vram(tile*32),VC)
    for _ in range(8): s.move_l_imm_abs(pix,VD)
for off in range(0,7): s.move_b_imm_abs(0,RAM+off)
s.move_b_imm_abs(0x40,0x00A10009)      # pad 1: TH as output
s.sr(0x2000)
s.label("idle"); s.bra("idle")
start=s.done()

# ---- vertical blank handler (at 0x400), once per frame
h=A()
h.move_b_imm_abs(0x40,0x00A10003); h.nop(); h.nop()
h.move_b_abs_d(0x00A10003,1)            # D1: C B R L D U (0 = pressed)
h.move_b_imm_abs(0x00,0x00A10003); h.nop(); h.nop()
h.move_b_abs_d(0x00A10003,2)            # D2: Start A (0 = pressed)
# is A, B or C held? -> D0
h.move_b_d_d(1,3); h.andi_b(0x30,3); h.cmpi_b_d(0x30,3); h.bne("held")
h.btst(4,2); h.beq("held")
h.moveq(0,0); h.bra("got")
h.label("held"); h.moveq(1,0)
h.label("got")
# the held state of the last 4 frames
h.move_b_abs_abs(RAM+4,RAM+5); h.move_b_abs_abs(RAM+3,RAM+4); h.move_b_abs_abs(RAM+2,RAM+3)
h.move_b_d_abs(0,RAM+2)
# Start: the next built-in delay (0..3), on the press only
h.btst(5,2); h.bne("nostart")
h.tst_b_abs(RAM+1); h.bne("startdone")
h.addq_b_abs(RAM+0); h.cmpi_b_abs(4,RAM+0); h.bne("startdone"); h.move_b_imm_abs(0,RAM+0)
h.label("startdone"); h.move_b_imm_abs(1,RAM+1); h.bra("startend")
h.label("nostart"); h.move_b_imm_abs(0,RAM+1)
h.label("startend")
# the held state, as many frames ago as the built-in delay says -> D5
h.move_b_abs_d(RAM+0,4)
h.tst_b_abs(RAM+0); h.bne("d1"); h.move_b_abs_d(RAM+2,5); h.bra("used")
h.label("d1"); h.cmpi_b_d(1,4); h.bne("d2"); h.move_b_abs_d(RAM+3,5); h.bra("used")
h.label("d2"); h.cmpi_b_d(2,4); h.bne("d3"); h.move_b_abs_d(RAM+4,5); h.bra("used")
h.label("d3"); h.move_b_abs_d(RAM+5,5)
h.label("used")
# the screen: white while held (after the built-in delay), black otherwise
h.move_l_imm_abs(cmd_cram(0),VC)
h.w(0x4A05)                              # tst.b D5
h.beq("black"); h.move_w_imm_abs(0x0EEE,VD); h.bra("drawn")
h.label("black"); h.move_w_imm_abs(0x0000,VD)
h.label("drawn")
# row 2: a red cell, then one green cell for each frame of built-in delay
h.move_l_imm_abs(cmd_vram(0xC000+2*128+2*2),VC)
h.move_w_imm_abs(1,VD)
for i in (1,2,3):
    h.cmpi_b_d(i,4); h.bcs("off%d"%i); h.move_w_imm_abs(2,VD); h.bra("on%d"%i)
    h.label("off%d"%i); h.move_w_imm_abs(0,VD)
    h.label("on%d"%i)
# row 4: the frame counter in binary (8 blue cells)
h.addq_b_abs(RAM+6); h.move_b_abs_d(RAM+6,6)
h.move_l_imm_abs(cmd_vram(0xC000+4*128+2*2),VC)
for b in range(7,-1,-1):
    h.btst(b,6); h.beq("cz%d"%b); h.move_w_imm_abs(3,VD); h.bra("cn%d"%b)
    h.label("cz%d"%b); h.move_w_imm_abs(0,VD)
    h.label("cn%d"%b)
h.rte()
handler=h.done()

rom=bytearray(0x1000)
struct.pack_into(">I",rom,0,0x00FF0000)
for i in range(1,64): struct.pack_into(">I",rom,i*4,0x200)
struct.pack_into(">I",rom,30*4,0x400)
rom[0x100:0x110]=b"SEGA MEGA DRIVE "
rom[0x110:0x120]=b"(C)GPEX 2026.NOV "[:16]
rom[0x120:0x150]=b"GENESIS PLUS EX INPUT LAG TEST".ljust(48)
rom[0x150:0x180]=b"GENESIS PLUS EX INPUT LAG TEST".ljust(48)
rom[0x180:0x18E]=b"GM 00000000-00"
rom[0x190:0x1A0]=b"J               "
struct.pack_into(">I",rom,0x1A0,0); struct.pack_into(">I",rom,0x1A4,0xFFF)
struct.pack_into(">I",rom,0x1A8,0xFF0000); struct.pack_into(">I",rom,0x1AC,0xFFFFFF)
rom[0x1F0:0x1F3]=b"JUE"
rom[0x200:0x200+len(start)]=start
rom[0x400:0x400+len(handler)]=handler
open("input_lag_test.md","wb").write(rom)
print("input_lag_test.md written,",len(rom),"bytes")
