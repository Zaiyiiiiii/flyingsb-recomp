"""把原版 PE 映射成一块内存镜像，供反汇编与提升（lift）使用。"""
import pefile


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.size = self.pe.OPTIONAL_HEADER.SizeOfImage
        self.entry = self.base + self.pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.data = bytearray(self.size)
        hdr = self.pe.header[: self.pe.OPTIONAL_HEADER.SizeOfHeaders]
        self.data[: len(hdr)] = hdr
        self.sections = []
        for s in self.pe.sections:
            raw = s.get_data()
            self.data[s.VirtualAddress : s.VirtualAddress + len(raw)] = raw[: self.size - s.VirtualAddress]
            name = s.Name.rstrip(b"\0").decode()
            self.sections.append((name, self.base + s.VirtualAddress, s.Misc_VirtualSize, s.Characteristics))
        text = [s for s in self.sections if s[0] == ".text"][0]
        self.text_lo = text[1]
        self.text_hi = text[1] + text[2]
        # 导入表：IAT 槽地址 -> (dll, 函数名)
        self.imports = {}
        for e in self.pe.DIRECTORY_ENTRY_IMPORT:
            dll = e.dll.decode()
            for i in e.imports:
                name = i.name.decode() if i.name else "#%d" % i.ordinal
                self.imports[i.address] = (dll, name)

    def in_text(self, a):
        return self.text_lo <= a < self.text_hi

    def in_image(self, a):
        return self.base <= a < self.base + self.size

    def read(self, addr, n):
        o = addr - self.base
        return bytes(self.data[o : o + n])

    def u32(self, addr):
        o = addr - self.base
        return int.from_bytes(self.data[o : o + 4], "little")
