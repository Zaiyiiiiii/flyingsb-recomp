"""函数发现：从入口出发做递归下降反汇编，收集函数、跳转表、间接调用。"""
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

_md = Cs(CS_ARCH_X86, CS_MODE_32)
_md.detail = True

COND_JUMPS = {
    "jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns", "jp", "jnp",
    "jl", "jge", "jle", "jg", "jecxz", "jcxz", "loop", "loope", "loopne",
}


GARBAGE = {
    "in", "out", "insb", "insd", "insw", "outsb", "outsd", "outsw", "bound", "arpl", "daa", "das", "aaa", "aas",
    "aam", "aad", "iret", "iretd", "ljmp", "lcall", "retf", "cli", "sti", "enter", "into", "int", "salc", "hlt",
    "lds", "les", "lahf_", "xlatb", "wbinvd", "invd", "lar", "lsl", "sidt", "lgdt", "sgdt", "lidt", "cmpxchg8b",
    "push", "pushal",
}
GARBAGE.discard("push")
GARBAGE.discard("pushal")


class Func:
    def __init__(self, entry):
        self.entry = entry
        self.insns = {}        # addr -> capstone insn
        self.tails = set()     # 作为尾调用的目标函数入口
        self.jtabs = {}        # 间接 jmp 指令地址 -> 目标地址列表
        self.bad = []          # 无法解码的地址
        self.ijmp_unresolved = []


class Discovery:
    def __init__(self, img, extra_roots=()):
        self.img = img
        self.cache = {}
        self.roots = {img.entry}
        self.roots.update(extra_roots)
        self.funcs = {}
        self.jt_targets = set()
        self.cand = set()      # 仅被数据/立即数引用的候选入口
        self.data_refs = set()

    # ---- 解码 ----
    def decode(self, a):
        c = self.cache
        if a in c:
            return c[a]
        if not self.img.in_text(a):
            c[a] = None
            return None
        o = a - self.img.base
        it = _md.disasm(bytes(self.img.data[o : o + 15]), a, 1)
        ins = next(it, None)
        c[a] = ins
        return ins

    def classify(self, ins):
        m = ins.mnemonic
        if m.startswith("rep") or m.startswith("lock") or m.startswith("bnd") or m.startswith("notrack"):
            m = m.split()[-1]
        ops = ins.operands
        if m == "call":
            if ops[0].type == X86_OP_IMM:
                return "call", ops[0].imm
            return "icall", None
        if m == "jmp":
            if ops[0].type == X86_OP_IMM:
                return "jmp", ops[0].imm
            return "ijmp", None
        if m in COND_JUMPS:
            return "jcc", ops[0].imm
        if m in ("ret", "retf", "iret", "iretd"):
            return "ret", None
        if m in ("int3", "hlt", "ud2", "int1", "into"):
            return "halt", None
        return "seq", None

    # ---- 跳转表 ----
    def resolve_jtab(self, ins):
        op = ins.operands[0]
        if op.type != X86_OP_MEM:
            return None
        mem = op.mem
        if mem.base != 0 or mem.index == 0 or mem.scale != 4:
            return None
        tab = mem.disp & 0xFFFFFFFF
        if not self.img.in_image(tab):
            return None
        out = []
        a = tab
        lead_bad = 0
        while len(out) < 4096:
            v = self.img.u32(a)
            if not self.img.in_text(v):
                # 表首的若干项可能从不会被索引到（如 memcpy 的对齐跳转表第 0 项），允许跳过
                if not out and lead_bad < 3:
                    lead_bad += 1
                    a += 4
                    continue
                break
            out.append(v)
            a += 4
        self.jt_targets.update(out)
        # 索引寄存器可能为负数(如 memcpy 尾部的 ecx*4+表基址，ecx 为 -4..-1)：
        # 表前面紧邻的项只有在它们同时是别的跳转表目标时才并入，避免把无关数据当成代码
        a = tab - 4
        back = 0
        while back < 8 and self.img.u32(a) in self.jt_targets:
            out.append(self.img.u32(a))
            a -= 4
            back += 1
        return out or None

    # ---- 单个函数泛洪 ----
    def flood(self, entry, roots):
        f = Func(entry)
        work = [entry]
        img = self.img
        while work:
            a = work.pop()
            while True:
                if a in f.insns:
                    break
                if a != entry and a in roots:
                    f.tails.add(a)
                    break
                ins = self.decode(a)
                if ins is None:
                    f.bad.append(a)
                    break
                f.insns[a] = ins
                kind, tgt = self.classify(ins)
                nxt = a + ins.size
                if kind == "seq":
                    a = nxt
                    continue
                if kind == "call":
                    if tgt != nxt and img.in_text(tgt):
                        self.new_calls.add(tgt)
                    a = nxt
                    continue
                if kind == "icall":
                    a = nxt
                    continue
                if kind == "jmp":
                    if tgt != entry and tgt in roots:
                        f.tails.add(tgt)
                    elif img.in_text(tgt):
                        work.append(tgt)
                    else:
                        f.bad.append(a)
                    break
                if kind == "jcc":
                    if tgt != entry and tgt in roots:
                        f.tails.add(tgt)
                    elif img.in_text(tgt):
                        work.append(tgt)
                    a = nxt
                    continue
                if kind == "ijmp":
                    t = self.resolve_jtab(ins)
                    if t is None:
                        f.ijmp_unresolved.append(a)
                    else:
                        f.jtabs[a] = t
                        for x in t:
                            if x != entry and x in roots:
                                f.tails.add(x)
                            else:
                                work.append(x)
                    break
                break  # ret / halt
        return f

    def run(self):
        img = self.img
        # 第一阶段：仅以 call 目标为根做不动点
        while True:
            self.new_calls = set()
            self.funcs = {}
            for r in sorted(self.roots):
                self.funcs[r] = self.flood(r, self.roots)
            added = self.new_calls - self.roots
            if not added:
                break
            self.roots |= added
        # 第二阶段：数据/立即数引用的候选入口
        for it in range(6):
            cands = self.find_candidates()
            if not cands:
                break
            self.roots |= cands
            while True:
                self.new_calls = set()
                self.funcs = {}
                for r in sorted(self.roots):
                    self.funcs[r] = self.flood(r, self.roots)
                added = self.new_calls - self.roots
                if not added:
                    break
                self.roots |= added
        return self.funcs

    def covered(self):
        cov = {}
        for e, f in self.funcs.items():
            for a, ins in f.insns.items():
                cov.setdefault(a, e)
        return cov

    def find_candidates(self):
        img = self.img
        cov = self.covered()
        # 覆盖到的字节区间（任一函数内的指令字节）
        covbytes = set()
        for e, f in self.funcs.items():
            for a, ins in f.insns.items():
                for k in range(ins.size):
                    covbytes.add(a + k)
        refs = set()
        # 立即数引用（push imm / mov r, imm / lea 等）
        for e, f in self.funcs.items():
            for a, ins in f.insns.items():
                for op in ins.operands:
                    if op.type == X86_OP_IMM and img.in_text(op.imm & 0xFFFFFFFF):
                        k, _ = self.classify(ins)
                        if k in ("call", "jmp", "jcc"):
                            continue
                        refs.add(op.imm & 0xFFFFFFFF)
        # 数据节中的指针
        for name, lo, sz, ch in img.sections:
            if name == ".text":
                continue
            data = img.data
            o = lo - img.base
            for k in range(o, o + min(sz, len(data) - o) - 3, 4):
                v = data[k] | data[k + 1] << 8 | data[k + 2] << 16 | data[k + 3] << 24
                if img.in_text(v):
                    refs.add(v)
        # 代码段内未覆盖字节中的指针表（脚本命令表等）
        d = img.data
        lo = img.text_lo - img.base
        for k in range(lo, lo + (img.text_hi - img.text_lo) - 3):
            if (k + img.base) in covbytes:
                continue
            v = d[k] | d[k + 1] << 8 | d[k + 2] << 16 | d[k + 3] << 24
            if img.in_text(v):
                refs.add(v)
        # 序言特征扫描：未覆盖处的 push ebp; mov ebp,esp
        for k in range(lo, lo + (img.text_hi - img.text_lo) - 3):
            if d[k] == 0x55 and d[k + 1] == 0x8B and d[k + 2] == 0xEC and (k + img.base) not in covbytes:
                refs.add(k + img.base)
        out = set()
        order = sorted(refs, key=lambda v: (0 if img.read(v, 3) == b"\x55\x8b\xec" else 1, v))
        for v in order:
            if v in self.roots or v in self.cand or v in covbytes:
                continue
            f = self.valid_entry(v, covbytes, out)
            if f:
                out.add(v)
                for a, i in f.insns.items():
                    covbytes.update(range(a, a + i.size))
        self.cand |= out
        return out

    def valid_entry(self, v, covbytes, acc=frozenset()):
        """试探性泛洪：能干净地解码到终结指令，且不压在已覆盖指令的中间。"""
        ins = self.decode(v)
        if ins is None:
            return None
        f = self.flood(v, self.roots | acc | {v})
        if f.bad or len(f.insns) < 2 or f.ijmp_unresolved:
            return False
        for a, i in f.insns.items():
            if a in covbytes:
                return False
            if i.mnemonic.split()[-1] in GARBAGE:
                return False
            for op in i.operands:
                if op.type == X86_OP_REG and i.reg_name(op.reg) in ("es", "ss", "cs", "ds", "gs"):
                    return False
                if op.type == X86_OP_MEM and op.mem.segment and i.reg_name(op.mem.segment) in ("es", "ss", "gs") \
                        and not i.mnemonic.split()[-1].startswith(("movs", "stos", "cmps", "scas", "lods")):
                    return False
        if v & 0xF and self.img.data[v - 1 - self.img.base] not in (0xC3, 0xCC, 0x90):
            # 非 16 对齐时，要求有典型序言
            b = self.img.read(v, 3)
            if not (b[:3] == b"\x55\x8b\xec" or b[:2] == b"\x83\xec" or b[:2] == b"\x81\xec" or b[:1] in (b"\x8b", b"\x56", b"\x53", b"\x51", b"\x33", b"\x6a", b"\xb8", b"\x55", b"\xa1", b"\x83")):
                return False
        return f

    def after_term(self, v):
        """v 之前是否紧邻 ret n / jmp 等终结指令（通过回扫几种定长形式）。"""
        d = self.img.data
        o = v - self.img.base
        # ret n : C2 xx xx
        if o >= 3 and d[o - 3] == 0xC2:
            return True
        # jmp rel32 : E9 xx xx xx xx ; jmp rel8: EB xx
        if o >= 5 and d[o - 5] == 0xE9:
            return True
        if o >= 2 and d[o - 2] == 0xEB:
            return True
        return False
