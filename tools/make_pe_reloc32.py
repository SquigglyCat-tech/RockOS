#!/usr/bin/env python3
# Regenerates src/kernel/pe_reloc32.h - a relocatable PE32 image with
# one .text section, ImageBase 0x400000 and a relocation table in the
# .text section (Wine layer 1c host-test fixture).
out = bytearray()

def put(b):
    out.extend(b)

# --- DOS header: 0x40 bytes, "MZ" + e_lfanew=0x40 ---
dos = bytearray(0x40)
dos[0:2] = b"MZ"
dos[0x3C:0x40] = (0x40).to_bytes(4, "little")
put(dos)

# --- COFF: 20 bytes at 0x40, optional header at 0x48 ---
put(b"PE\0\0")
put((0x014C).to_bytes(2, "little"))  # machine i386
put((1).to_bytes(2, "little"))       # 1 section
put((0).to_bytes(4, "little"))       # timestamp
put((0).to_bytes(4, "little"))       # symbol table pointer
put((0).to_bytes(4, "little"))       # symbol count
put((0x00E0).to_bytes(2, "little"))  # size of optional
put((0x0102).to_bytes(2, "little"))  # characteristics

# --- Optional header: 0xE0 bytes at 0x48 ---
opt = bytearray()
opt += (0x010B).to_bytes(2, "little")   # PE32 magic
opt += (2).to_bytes(1, "little") + (0).to_bytes(1, "little")
opt += (0x100).to_bytes(4, "little")    # SizeOfCode
opt += (0).to_bytes(4, "little")        # SizeOfInitializedData
opt += (0).to_bytes(4, "little")        # SizeOfUninitializedData
opt += (0x1000).to_bytes(4, "little")   # EntryRVA
opt += (0x1000).to_bytes(4, "little")   # BaseOfCode
opt += (0).to_bytes(4, "little")        # BaseOfData
opt += (0x400000).to_bytes(4, "little") # ImageBase (custom, needs reloc)
opt += (0x1000).to_bytes(4, "little")   # SectionAlignment
opt += (0x1000).to_bytes(4, "little")   # FileAlignment
opt += (4).to_bytes(2, "little") + (0).to_bytes(2, "little")  # OS version
opt += (0).to_bytes(2, "little") + (0).to_bytes(2, "little")  # image/user ver
opt += (5).to_bytes(4, "little")        # SubVersion OS
opt += (0).to_bytes(4, "little")        # Win32 version value
opt += (0x2000).to_bytes(4, "little")   # SizeOfImage
opt += (0x1000).to_bytes(4, "little")   # SizeOfHeaders
opt += (0).to_bytes(4, "little")        # checksum
opt += (3).to_bytes(2, "little")        # subsystem console
opt += (0).to_bytes(2, "little")        # DLL characteristics
opt += (1).to_bytes(4, "little") + (0).to_bytes(4, "little")   # stack
opt += (0).to_bytes(4, "little") + (0).to_bytes(4, "little")   # heap
opt += (0).to_bytes(4, "little")        # loader flags
opt += (16).to_bytes(4, "little")       # NumberOfRvaAndSizes
assert len(opt) == 96  # NumberOfRvaAndSizes at 92, dirs follow
dirs = [(0, 0)] * 16                    # import table empty (dir 1)
dirs[5] = (0x1100, 0x18)                # relocation directory in .text
for rva, size in dirs:
    opt += rva.to_bytes(4, "little") + size.to_bytes(4, "little")
assert len(opt) == 0xE0  # 96 pre-dirs + 16*8
put(opt)

# --- Section table: 40 bytes at 0x128 ---
sec = bytearray()
sec += b".text\0\0\0"
sec += (0x100).to_bytes(4, "little")    # virtual size
sec += (0x1000).to_bytes(4, "little")   # virtual address
sec += (0x100).to_bytes(4, "little")    # raw size
sec += (0x1000).to_bytes(4, "little")   # raw offset
sec += (0).to_bytes(4, "little")        # relocs
sec += (0).to_bytes(4, "little")        # linenums
sec += (0).to_bytes(2, "little")        # num relocs
sec += (0).to_bytes(2, "little")        # num linenums
sec += (0x60000020).to_bytes(4, "little")
put(sec)

# pad headers to 0x1000
out += b"\x00" * (0x1000 - len(out))

# --- .text section: 0x100 bytes at RVA/file 0x1000 ---
text = bytearray(0x100)
text[0] = 0x90                                    # nop at entry
text[0xF0:0xF4] = (0x400110).to_bytes(4, "little") # absolute pointer @ RVA 0x10F0
# relocation block at RVA 0x1100 (section offset 0x100? - inside section,
# block is at section offset 0x0 => but we placed the dir at RVA 0x1100;
# note 0x1000+0x100 = 0x1100 > section virtual size, so move the block
# INTO the section: dir RVA = 0x1000 + 0x80, pointer at section offset 0x40.
# Regenerate dir below - see make_image() fix in header constants.
put(text)

# The reloc directory actually lives at section offset 0x80 (RVA 0x1080):
block = bytearray()
block += (0x1000).to_bytes(4, "little")           # page RVA
block += (0x18).to_bytes(4, "little")             # block size (8 + 8*2)
block += ((0x3 << 12) | (0xF0 & 0x0FFF)).to_bytes(2, "little")  # HIGHLOW @ +0xF0
block += (0).to_bytes(2, "little")                # ABSOLUTE pad x7
block += (0).to_bytes(2, "little")
block += (0).to_bytes(2, "little")
block += (0).to_bytes(2, "little")
block += (0).to_bytes(2, "little")
block += (0).to_bytes(2, "little")
block += (0).to_bytes(2, "little")
out[len(out) - 0x100 + 0x80: len(out) - 0x100 + 0x80 + len(block)] = block
# fix the directory RVA/size recorded in the optional header:
# optional starts at 0x58 (0x40 DOS + 0x18 COFF); dirs begin 96 in.
DIR_OFF = 0x58 + 96 + 5 * 8
out[DIR_OFF:DIR_OFF + 4] = (0x1080).to_bytes(4, "little")
out[DIR_OFF + 4:DIR_OFF + 8] = (0x18).to_bytes(4, "little")

WORDS = [out[i] | (out[i + 1] << 8) if i + 1 < len(out) else out[i]
         for i in range(0, len(out), 2)]

with open("src/kernel/pe_reloc32.h", "w") as f:
    f.write("/* Generated by tools/make_pe_reloc32.py - relocatable PE32 "
            "fixture (layer 1c). */\n")
    f.write("#ifndef ROCKOS_PE_RELOC32_H\n#define ROCKOS_PE_RELOC32_H\n")
    f.write("#define PE_RELOC32_IMAGE_BASE 0x400000u\n")
    f.write("#define PE_RELOC32_ENTRY_RVA 0x1000u\n")
    f.write("#define PE_RELOC32_RELOC_POINTER_RVA 0x10F0u\n")
    f.write("#define PE_RELOC32_SIZE_OF_IMAGE 0x2000u\n")
    f.write("static const unsigned short pe_reloc32_bytes[] = {")
    f.write(",".join(str(v) for v in WORDS))
    f.write("};\n#define PE_RELOC32_NUM_WORDS " + str(len(WORDS)) + "\n")
    f.write("#endif /* ROCKOS_PE_RELOC32_H */\n")
print("wrote src/kernel/pe_reloc32.h", len(WORDS), "words; file bytes:",
      len(out))
