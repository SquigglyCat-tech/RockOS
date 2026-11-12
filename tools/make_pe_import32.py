#!/usr/bin/env python3
# Regenerates src/kernel/pe_import32.h - a PE32 fixture that imports
# kernel32.dll (WriteFile + ExitProcess) with PIC code, so the layer-1d
# IAT-stub path can be host-tested end to end.

out = bytearray()

def put(b):
    out.extend(b)

# DOS header: 0x40 bytes, "MZ" + e_lfanew = 0x40
dos = bytearray(0x40)
dos[0:2] = b"MZ"
dos[0x3C:0x40] = (0x40).to_bytes(4, "little")
put(dos)

# COFF: 20 bytes at 0x40, optional header at 0x58
put(b"PE\0\0")
put((0x014C).to_bytes(2, "little"))  # i386
put((1).to_bytes(2, "little"))       # 1 section
put((0).to_bytes(4, "little"))       # timestamp
put((0).to_bytes(4, "little"))       # symbol table ptr
put((0).to_bytes(4, "little"))       # symbol count
put((0x00E0).to_bytes(2, "little"))  # optional size
put((0x0102).to_bytes(2, "little"))  # characteristics

# Optional header: 0xE0 bytes at 0x58
opt = bytearray()
opt += (0x010B).to_bytes(2, "little")   # PE32 magic
opt += (2).to_bytes(1, "little") + (0).to_bytes(1, "little")
opt += (0x100).to_bytes(4, "little")    # SizeOfCode
opt += (0).to_bytes(4, "little")        # init data
opt += (0).to_bytes(4, "little")        # uninit data
opt += (0x1000).to_bytes(4, "little")   # EntryRVA
opt += (0x1000).to_bytes(4, "little")   # BaseOfCode
opt += (0).to_bytes(4, "little")        # BaseOfData
opt += (0x400000).to_bytes(4, "little") # ImageBase
opt += (0x1000).to_bytes(4, "little")   # SectionAlignment
opt += (0x1000).to_bytes(4, "little")   # FileAlignment
opt += (4).to_bytes(2, "little") + (0).to_bytes(2, "little")
opt += (0).to_bytes(2, "little") + (0).to_bytes(2, "little")
opt += (5).to_bytes(4, "little")        # SubVersion OS
opt += (0).to_bytes(4, "little")        # Win32 version
opt += (0x2000).to_bytes(4, "little")   # SizeOfImage
opt += (0x1000).to_bytes(4, "little")   # SizeOfHeaders
opt += (0).to_bytes(4, "little")        # checksum
opt += (3).to_bytes(2, "little")        # console subsystem
opt += (0).to_bytes(2, "little")        # DLL characteristics
opt += (1).to_bytes(4, "little") + (0).to_bytes(4, "little")
opt += (0).to_bytes(4, "little") + (0).to_bytes(4, "little")
opt += (0).to_bytes(4, "little")        # loader flags
opt += (16).to_bytes(4, "little")       # NumberOfRvaAndSizes
assert len(opt) == 96
dirs = [(0, 0)] * 16
dirs[1] = (0x1080, 0x14)                # import descriptor (in .text)
for rva, size in dirs:
    opt += rva.to_bytes(4, "little") + size.to_bytes(4, "little")
assert len(opt) == 0xE0
put(opt)

# Section table at 0x138
sec = bytearray()
sec += b".text\0\0\0"
sec += (0x200).to_bytes(4, "little")    # virtual size
sec += (0x1000).to_bytes(4, "little")   # virtual address
sec += (0x200).to_bytes(4, "little")    # raw size
sec += (0x1000).to_bytes(4, "little")   # raw offset
sec += (0).to_bytes(4, "little") + (0).to_bytes(4, "little")
sec += (0).to_bytes(2, "little") + (0).to_bytes(2, "little")
sec += (0x60000020).to_bytes(4, "little")
put(sec)
out += b"\x00" * (0x1000 - len(out))

# ---- .text section (RVA/file 0x1000..0x1200) ----
text = bytearray(0x200)

MSG = b"RockOS kernel32 write ok\n"
MSG_LEN = len(MSG)

# entry code at RVA 0x1000 (offset 0):
#   call next
# next: pop esi          ; esi = rva_of(next)
#   add esi, (msg - next)
#   push 0               ; lpWritten = NULL
#   push MSG_LEN
#   push esi             ; msg
#   push 1               ; hStdOutput
#   call dword [iat_write]  (FF 15 imm32, RVA-relative)
#   add esp, 16
#   push 0
#   call dword [iat_exit]
#   ret
code = bytearray()
code += b"\xE8\x00\x00\x00\x00"          # call next
code += b"\x5E"                          # pop esi
code += b"\x81\xC6" + (0x1030 - 0x1005).to_bytes(4, "little")  # add esi, off
code += b"\x6A\x00"                      # push 0
code += b"\x68" + (MSG_LEN).to_bytes(4, "little")  # push MSG_LEN
code += b"\x56"                          # push esi
code += b"\x6A\x01"                      # push 1
code += b"\xFF\x15" + (0x1050).to_bytes(4, "little")  # call [iat_write]
code += b"\x83\xC4\x10"                  # add esp, 16
code += b"\x6A\x00"                      # push 0
code += b"\xFF\x15" + (0x1054).to_bytes(4, "little")  # call [iat_exit]
code += b"\xC3"                          # ret
text[0:len(code)] = code

MSG = b"RockOS kernel32 write ok\n"
MSG_LEN = len(MSG)
text[0x30:0x30 + MSG_LEN] = MSG

# IAT at RVA 0x1050 (offset 0x50): initial values point at hint/name
text[0x50:0x54] = (0x10C0).to_bytes(4, "little")  # WriteFile
text[0x54:0x58] = (0x10D0).to_bytes(4, "little")  # ExitProcess

# import descriptor at RVA 0x1080 (offset 0x80):
text[0x80:0x84] = (0x10B0).to_bytes(4, "little")  # OriginalFirstThunk
text[0x84:0x88] = (0).to_bytes(4, "little")       # TimeDateStamp
text[0x88:0x8C] = (0).to_bytes(4, "little")       # ForwarderChain
text[0x8C:0x90] = (0x10A0).to_bytes(4, "little")  # Name RVA
text[0x90:0x94] = (0x1050).to_bytes(4, "little")  # FirstThunk (IAT)

# DLL name at RVA 0x10A0 (offset 0xA0)
text[0xA0:0xA0 + len(b"kernel32.dll\0")] = b"kernel32.dll\0"

# OFT array at RVA 0x10B0 (offset 0xB0): 2 hint/name RVAs + NULL
text[0xB0:0xB4] = (0x10C0).to_bytes(4, "little")
text[0xB4:0xB8] = (0x10D0).to_bytes(4, "little")
text[0xB8:0xBC] = (0).to_bytes(4, "little")       # terminator

# hint/name: WriteFile at 0x10C0, ExitProcess at 0x10D0
text[0xC0:0xC4] = b"\x00\x00" + b"WriteFile\0"
text[0xD0:0xD4] = b"\x00\x00" + b"ExitProcess\0"

out += text
WORDS = [out[i] | (out[i + 1] << 8) for i in range(0, len(out), 2)]

with open("src/kernel/pe_import32.h", "w") as f:
    f.write("/* Generated by tools/make_pe_import32.py - PE32 fixture "
            "importing kernel32.dll (layer 1d). */\n")
    f.write("#ifndef ROCKOS_PE_IMPORT32_H\n#define ROCKOS_PE_IMPORT32_H\n")
    f.write("#define PE_IMPORT32_IMAGE_BASE 0x400000u\n")
    f.write("#define PE_IMPORT32_ENTRY_RVA 0x1000u\n")
    f.write("#define PE_IMPORT32_IAT_RVA 0x1050u\n")
    f.write("#define PE_IMPORT32_SIZE_OF_IMAGE 0x2000u\n")
    f.write("static const unsigned short pe_import32_bytes[] = {")
    f.write(",".join(str(v) for v in WORDS))
    f.write("};\n#define PE_IMPORT32_NUM_WORDS " + str(len(WORDS)) + "\n")
    f.write("#endif /* ROCKOS_PE_IMPORT32_H */\n")
print("wrote src/kernel/pe_import32.h", len(WORDS), "words; file bytes:",
      len(out))
