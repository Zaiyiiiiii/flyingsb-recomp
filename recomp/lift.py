"""x86 -> C 指令提升器。

每个来宾函数生成一个 `void f_XXXXXXXX(void)`：
  * 通用寄存器在函数入口从全局 G 载入为局部变量，调用/返回时写回；
  * 标志位用局部变量，按活跃性分析只生成后续会被用到的标志；
  * 来宾内存通过 m+地址 访问，栈保存在来宾内存里；
  * x87 通过运行时的 FPU 栈函数实现。
"""
import capstone.x86 as X
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

FLAG_BITS = {
    "c": (X.X86_EFLAGS_TEST_CF, X.X86_EFLAGS_MODIFY_CF | X.X86_EFLAGS_RESET_CF | X.X86_EFLAGS_SET_CF | X.X86_EFLAGS_UNDEFINED_CF),
    "z": (X.X86_EFLAGS_TEST_ZF, X.X86_EFLAGS_MODIFY_ZF | X.X86_EFLAGS_RESET_ZF | X.X86_EFLAGS_SET_ZF | X.X86_EFLAGS_UNDEFINED_ZF),
    "s": (X.X86_EFLAGS_TEST_SF, X.X86_EFLAGS_MODIFY_SF | X.X86_EFLAGS_RESET_SF | X.X86_EFLAGS_SET_SF | X.X86_EFLAGS_UNDEFINED_SF),
    "o": (X.X86_EFLAGS_TEST_OF, X.X86_EFLAGS_MODIFY_OF | X.X86_EFLAGS_RESET_OF | X.X86_EFLAGS_SET_OF | X.X86_EFLAGS_UNDEFINED_OF),
    "p": (X.X86_EFLAGS_TEST_PF, X.X86_EFLAGS_MODIFY_PF | X.X86_EFLAGS_RESET_PF | X.X86_EFLAGS_SET_PF | X.X86_EFLAGS_UNDEFINED_PF),
    "a": (X.X86_EFLAGS_TEST_AF, X.X86_EFLAGS_MODIFY_AF | X.X86_EFLAGS_RESET_AF | X.X86_EFLAGS_SET_AF | X.X86_EFLAGS_UNDEFINED_AF),
}
ALLF = frozenset("czsopa")

PREFIXES = ("rep", "repe", "repne", "lock", "bnd", "notrack")

COND = {
    "o": "of", "no": "!of", "b": "cf", "ae": "!cf", "e": "zf", "ne": "!zf",
    "be": "(cf|zf)", "a": "!(cf|zf)", "s": "sf", "ns": "!sf", "p": "pf", "np": "!pf",
    "l": "(sf!=of)", "ge": "(sf==of)", "le": "(zf|(sf!=of))", "g": "(!zf&&(sf==of))",
    "z": "zf", "nz": "!zf", "c": "cf", "nc": "!cf", "nae": "cf", "nb": "!cf", "na": "(cf|zf)",
    "nbe": "!(cf|zf)", "pe": "pf", "po": "!pf", "nge": "(sf!=of)", "nl": "(sf==of)",
    "ng": "(zf|(sf!=of))", "nle": "(!zf&&(sf==of))",
}
COND_USE = {
    "o": "o", "no": "o", "b": "c", "ae": "c", "e": "z", "ne": "z", "be": "cz", "a": "cz",
    "s": "s", "ns": "s", "p": "p", "np": "p", "l": "so", "ge": "so", "le": "zso", "g": "zso",
    "z": "z", "nz": "z", "c": "c", "nc": "c", "nae": "c", "nb": "c", "na": "cz", "nbe": "cz",
    "pe": "p", "po": "p", "nge": "so", "nl": "so", "ng": "zso", "nle": "zso",
}

TYPES = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}
STYPES = {1: "int8_t", 2: "int16_t", 4: "int32_t", 8: "int64_t"}
MASKS = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}

R32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
SUB = {
    "ax": ("eax", 0xFFFF, 0), "cx": ("ecx", 0xFFFF, 0), "dx": ("edx", 0xFFFF, 0), "bx": ("ebx", 0xFFFF, 0),
    "sp": ("esp", 0xFFFF, 0), "bp": ("ebp", 0xFFFF, 0), "si": ("esi", 0xFFFF, 0), "di": ("edi", 0xFFFF, 0),
    "al": ("eax", 0xFF, 0), "cl": ("ecx", 0xFF, 0), "dl": ("edx", 0xFF, 0), "bl": ("ebx", 0xFF, 0),
    "ah": ("eax", 0xFF, 8), "ch": ("ecx", 0xFF, 8), "dh": ("edx", 0xFF, 8), "bh": ("ebx", 0xFF, 8),
}

TIB_BASE = 0x00300000


class Unsupported(Exception):
    pass


def split_mn(mn):
    parts = mn.split()
    pre = [p for p in parts if p in PREFIXES]
    core = [p for p in parts if p not in PREFIXES]
    return pre, (core[0] if core else mn)


class Lifter:
    def __init__(self, img, funcs):
        self.img = img
        self.funcs = funcs
        self.roots = set(funcs)
        self.unsupported = {}
        self.hooks = {}   # 地址 -> (C 函数名, 跳转目标或 None)：在该地址的指令执行前插入一次调用（见 recomp/hooks.txt）

    # ------------------------------------------------------------ 标志位
    def use_def(self, ins):
        pre, mn = split_mn(ins.mnemonic)
        if mn in ("lahf", "pushfd", "pushf", "pushad"):
            return (ALLF if mn != "pushad" else frozenset()), frozenset()
        if mn in ("sahf", "popfd", "popf"):
            return frozenset(), ALLF
        if mn in ("call", "ret", "retf"):
            return frozenset(), ALLF
        if mn in ("cmpxchg", "xadd"):
            return frozenset(), ALLF
        use = set()
        df = set()
        e = ins.eflags
        for k, (t, d) in FLAG_BITS.items():
            if e & t:
                use.add(k)
            if e & d:
                df.add(k)
        if mn in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr", "shld", "shrd"):
            # 计数为 0 时标志不变 -> 视为同时“使用”旧值，保证活跃
            use |= df
        if mn in ("rcl", "rcr", "adc", "sbb"):
            use.add("c")
        if pre and mn in ("scasb", "scasw", "scasd", "cmpsb", "cmpsw", "cmpsd", "scas", "cmps"):
            use |= df
        return frozenset(use), frozenset(df)

    def liveness(self, f):
        """返回 addr -> 指令执行之后仍活跃的标志集合。"""
        insns = f.insns
        info = {}
        succs = {}
        for a, ins in insns.items():
            u, d = self.use_def(ins)
            pre, mn = split_mn(ins.mnemonic)
            nxt = a + ins.size
            s = []
            if mn == "jmp":
                o = ins.operands[0]
                if o.type == X86_OP_IMM:
                    t = o.imm & 0xFFFFFFFF
                    if t in insns:
                        s.append(t)
                else:
                    s.extend(t for t in f.jtabs.get(a, ()) if t in insns)
            elif mn in ("ret", "retf", "int3", "hlt", "ud2"):
                pass
            elif mn in COND_JUMPS_LIFT or mn.startswith("loop"):
                t = ins.operands[0].imm & 0xFFFFFFFF
                if t in insns:
                    s.append(t)
                if nxt in insns:
                    s.append(nxt)
            else:
                if nxt in insns:
                    s.append(nxt)
            info[a] = (u, d)
            succs[a] = s
        # 前驱表
        preds = {a: [] for a in insns}
        for a, ss in succs.items():
            for t in ss:
                preds[t].append(a)
        live_in = {a: frozenset() for a in insns}
        live_out = {a: frozenset() for a in insns}
        work = list(insns)
        inwork = set(work)
        while work:
            a = work.pop()
            inwork.discard(a)
            lo = frozenset().union(*(live_in[t] for t in succs[a])) if succs[a] else frozenset()
            u, d = info[a]
            li = u | (lo - d)
            live_out[a] = lo
            if li != live_in[a]:
                live_in[a] = li
                for p in preds[a]:
                    if p not in inwork:
                        work.append(p)
                        inwork.add(p)
        return live_out

    # ------------------------------------------------------------ 操作数
    def reg_read(self, name):
        if name in R32:
            return name
        if name in SUB:
            full, mask, sh = SUB[name]
            if sh:
                return "((uint8_t)(%s>>%d))" % (full, sh)
            return "((%s)%s)" % (TYPES[1 if mask == 0xFF else 2], full)
        raise Unsupported("reg " + name)

    def reg_write(self, name, val):
        if name in R32:
            return "%s = %s;" % (name, val)
        if name not in SUB:
            raise Unsupported("regw " + name)
        full, mask, sh = SUB[name]
        if sh:
            return "%s = (%s & 0xFFFF00FFu) | ((uint32_t)(uint8_t)(%s) << 8);" % (full, full, val)
        keep = 0xFFFFFFFF ^ mask
        return "%s = (%s & 0x%Xu) | (uint32_t)(%s)(%s);" % (full, full, keep, TYPES[1 if mask == 0xFF else 2], val)

    def addr(self, op, ins):
        mem = op.mem
        parts = []
        if mem.base:
            parts.append(ins.reg_name(mem.base))
        if mem.index:
            sc = mem.scale
            parts.append(ins.reg_name(mem.index) + ("*%du" % sc if sc != 1 else ""))
        disp = mem.disp & 0xFFFFFFFF
        seg = ins.reg_name(mem.segment) if mem.segment else None
        if seg == "fs":
            disp = (disp + TIB_BASE) & 0xFFFFFFFF
        elif seg in ("gs",):
            raise Unsupported("gs seg")
        if not parts:
            return "0x%Xu" % disp
        e = " + ".join(parts)
        if disp:
            e += " + 0x%Xu" % disp
        return "(uint32_t)(%s)" % e

    def rd(self, op, ins, size=None):
        size = size or op.size
        if op.type == X86_OP_REG:
            return self.reg_read(ins.reg_name(op.reg))
        if op.type == X86_OP_IMM:
            return "0x%Xu" % (op.imm & MASKS[size])
        if op.type == X86_OP_MEM:
            return "RD%d(%s)" % (size * 8, self.addr(op, ins))
        raise Unsupported("rd op")

    def wr(self, op, ins, val, size=None):
        size = size or op.size
        if op.type == X86_OP_REG:
            return self.reg_write(ins.reg_name(op.reg), val)
        if op.type == X86_OP_MEM:
            return "WR%d(%s, %s);" % (size * 8, self.addr(op, ins), val)
        raise Unsupported("wr op")

    # ------------------------------------------------------------ 标志生成
    @staticmethod
    def pf(r):
        return "rt_parity[(uint8_t)(%s)]" % r

    def flags(self, live, kind, B):
        """返回设置标志的语句（只含活跃的）。变量约定：a,b,r (类型 U)，c 为进位输入。"""
        s = []
        sb = B - 1
        L = lambda k: k in live
        if kind in ("add", "adc"):
            if L("c"):
                s.append("cf = %s;" % ("r < a" if kind == "add" else "(c ? r <= a : r < a)"))
            if L("o"):
                s.append("of = (((a ^ r) & (b ^ r)) >> %d) & 1;" % sb)
            if L("a"):
                s.append("af = ((a ^ b ^ r) >> 4) & 1;")
        elif kind in ("sub", "sbb", "cmp"):
            if L("c"):
                s.append("cf = %s;" % ("a < b" if kind != "sbb" else "(c ? a <= b : a < b)"))
            if L("o"):
                s.append("of = (((a ^ b) & (a ^ r)) >> %d) & 1;" % sb)
            if L("a"):
                s.append("af = ((a ^ b ^ r) >> 4) & 1;")
        elif kind == "logic":
            if L("c"):
                s.append("cf = 0;")
            if L("o"):
                s.append("of = 0;")
            if L("a"):
                s.append("af = 0;")
        elif kind == "inc":
            if L("o"):
                s.append("of = (r == 0x%Xu);" % (1 << sb))
            if L("a"):
                s.append("af = ((a ^ r) >> 4) & 1;")
        elif kind == "dec":
            if L("o"):
                s.append("of = (a == 0x%Xu);" % (1 << sb))
            if L("a"):
                s.append("af = ((a ^ r) >> 4) & 1;")
        elif kind == "neg":
            if L("c"):
                s.append("cf = (a != 0);")
            if L("o"):
                s.append("of = (a == 0x%Xu);" % (1 << sb))
            if L("a"):
                s.append("af = ((a ^ r) >> 4) & 1;")
        if L("z"):
            s.append("zf = (r == 0);")
        if L("s"):
            s.append("sf = (r >> %d) & 1;" % sb)
        if L("p"):
            s.append("pf = %s;" % self.pf("r"))
        return s

    # ------------------------------------------------------------ 主入口
    def lift_function(self, f):
        live_out = self.liveness(f)
        out = []
        entry = f.entry
        out.append("void f_%08X(void) {" % entry)
        out.append("  REGS_DECL; uint32_t cf=0,zf=0,sf=0,of=0,pf=0,af=0; int df=0;")
        out.append("  REGS_LOAD();")
        out.append("L_%08X:;" % entry)
        addrs = sorted(f.insns)
        # 需要标签的地址
        labels = set()
        for a in addrs:
            ins = f.insns[a]
            pre, mn = split_mn(ins.mnemonic)
            if mn == "jmp" and ins.operands[0].type == X86_OP_IMM:
                labels.add(ins.operands[0].imm & 0xFFFFFFFF)
            elif mn in COND_JUMPS_LIFT or mn.startswith("loop"):
                labels.add(ins.operands[0].imm & 0xFFFFFFFF)
        for a, ts in f.jtabs.items():
            labels.update(ts)
        for a, (_, tgt) in self.hooks.items():
            if tgt is not None and a in f.insns:
                labels.add(tgt)   # 钩子返回非零时要跳到的标签
        for i, a in enumerate(addrs):
            ins = f.insns[a]
            pre, mn = split_mn(ins.mnemonic)
            if mn not in ("jmp", "ret", "retf", "int3", "hlt", "ud2", "iret", "iretd"):
                nxt = a + ins.size
                if not (i + 1 < len(addrs) and addrs[i + 1] == nxt) and nxt in f.insns:
                    labels.add(nxt)
        for i, a in enumerate(addrs):
            ins = f.insns[a]
            if a in labels and a != entry:
                out.append("L_%08X:;" % a)
            try:
                body = self.lift_insn(f, a, ins, live_out[a])
            except Unsupported as e:
                self.unsupported.setdefault(str(e), []).append(a)
                body = ['RT_UNIMPL(0x%X, "%s %s");' % (a, ins.mnemonic, ins.op_str.replace('"', "'"))]
            out.append("  /* %X: %s %s */" % (a, ins.mnemonic, ins.op_str.replace("*/", "* /")))
            if a in self.hooks:
                # 钩子在指令之前执行：可通过全局寄存器 G 和来宾内存读写状态，返回后寄存器重新载入。
                # 带跳转目标时钩子返回 int，非零则跳过本条及其后的原有逻辑，直接去目标标签
                name, tgt = self.hooks[a]
                if tgt is None:
                    out.append("  { SPILL(); %s(); REGS_RELOAD(); }" % name)
                else:
                    out.append("  { SPILL(); int hk = %s(); REGS_RELOAD(); if (hk) goto L_%08X; }" % (name, tgt))
            for b in body:
                out.append("  " + b)
            # 不连续的顺序执行需要显式 goto
            pre, mn = split_mn(ins.mnemonic)
            nxt = a + ins.size
            falls = mn not in ("jmp", "ret", "retf", "int3", "hlt", "ud2", "iret", "iretd")
            if falls:
                nxt_in = i + 1 < len(addrs) and addrs[i + 1] == nxt
                if not nxt_in:
                    if nxt in f.insns:
                        labels.add(nxt)
                        out.append("  goto L_%08X;" % nxt)
                    elif nxt in self.roots and nxt != entry:
                        out.append("  { SPILL(); f_%08X(); return; }" % nxt)
                    elif nxt == entry:
                        out.append("  goto L_%08X;" % entry)
                    else:
                        out.append('  RT_UNIMPL(0x%X, "fall off");' % a)
        out.append("}")
        text = "\n".join(out)
        # 补充后期发现的标签（由 goto 引入）
        return text, labels

    # ------------------------------------------------------------ 跳转
    def jump_to(self, f, tgt, cond=None):
        if tgt != f.entry and tgt in self.roots:
            stmt = "{ SPILL(); f_%08X(); return; }" % tgt
        elif tgt in f.insns:
            stmt = "goto L_%08X;" % tgt
        else:
            stmt = 'RT_UNIMPL(0x%X, "jump out of function");' % tgt
        if cond is None:
            return stmt
        return "if (%s) %s" % (cond, stmt)

    def call_stmt(self, ins, a, tgt_expr_or_addr, direct):
        ret = a + ins.size
        lines = ["esp -= 4; WR32(esp, 0x%Xu);" % ret]
        if direct:
            lines.append("SPILL(); f_%08X(); REGS_RELOAD();" % tgt_expr_or_addr)
        else:
            lines.append("{ uint32_t tgt = %s; SPILL(); rt_call_ind(tgt); REGS_RELOAD(); }" % tgt_expr_or_addr)
        return lines

    def lift_insn(self, f, a, ins, live):
        pre, mn = split_mn(ins.mnemonic)
        ops = ins.operands
        n = len(ops)
        rep = "rep" in pre or "repe" in pre
        repne = "repne" in pre
        nxt = a + ins.size
        S = []

        def sz(i=0):
            return ops[i].size

        # ---- 控制流 ----
        if mn == "call":
            o = ops[0]
            if o.type == X86_OP_IMM:
                t = o.imm & 0xFFFFFFFF
                if t == nxt:
                    return ["esp -= 4; WR32(esp, 0x%Xu);" % nxt]
                if t in self.roots:
                    return self.call_stmt(ins, a, t, True)
                return ["RT_UNIMPL(0x%X, \"call to non-root %X\");" % (a, t)]
            return self.call_stmt(ins, a, self.rd(o, ins, 4), False)
        if mn == "jmp":
            o = ops[0]
            if o.type == X86_OP_IMM:
                return [self.jump_to(f, o.imm & 0xFFFFFFFF)]
            if a in f.jtabs:
                targets = sorted(set(f.jtabs[a]))
                lines = ["switch (%s) {" % self.rd(o, ins, 4)]
                for t in targets:
                    lines.append("  case 0x%Xu: %s" % (t, self.jump_to(f, t)))
                lines.append('  default: RT_UNIMPL(0x%X, "bad jump table target"); }' % a)
                return lines
            return ["{ uint32_t tgt = %s; SPILL(); rt_jmp_ind(tgt); return; }" % self.rd(o, ins, 4)]
        if mn in ("ret", "retf"):
            extra = (ops[0].imm & 0xFFFF) if n else 0
            return ["esp += %d; SPILL(); return;" % (4 + extra)]
        if mn in ("int3", "hlt", "ud2", "int1"):
            return ['RT_UNIMPL(0x%X, "%s");' % (a, mn)]
        if mn.startswith("j") and mn[1:] in COND:
            t = ops[0].imm & 0xFFFFFFFF
            return [self.jump_to(f, t, COND[mn[1:]])]
        if mn == "jecxz":
            return [self.jump_to(f, ops[0].imm & 0xFFFFFFFF, "ecx == 0")]
        if mn == "jcxz":
            return [self.jump_to(f, ops[0].imm & 0xFFFFFFFF, "(uint16_t)ecx == 0")]
        if mn in ("loop", "loope", "loopne"):
            t = ops[0].imm & 0xFFFFFFFF
            extra = {"loop": "", "loope": " && zf", "loopne": " && !zf"}[mn]
            return ["ecx--;", self.jump_to(f, t, "ecx != 0" + extra)]
        if mn in ("nop", "wait", "fwait", "fnop", "pause", "endbr32"):
            return []
        if mn[:3] == "set" and mn[3:] in COND:
            return [self.wr(ops[0], ins, "(%s) ? 1 : 0" % COND[mn[3:]], 1)]
        if mn.startswith("cmov") and mn[4:] in COND:
            sz0 = ops[0].size
            return ["if (%s) { %s }" % (COND[mn[4:]], self.wr(ops[0], ins, self.rd(ops[1], ins, sz0), sz0))]

        # ---- 数据传送 ----
        if mn == "mov":
            s = sz(0)
            if ops[0].type == X86_OP_REG and ins.reg_name(ops[0].reg) in ("ds", "es", "fs", "gs", "ss", "cs"):
                return []
            if ops[1].type == X86_OP_REG and ins.reg_name(ops[1].reg) in ("ds", "es", "fs", "gs", "ss", "cs"):
                return [self.wr(ops[0], ins, "0x23", s)]
            return [self.wr(ops[0], ins, self.rd(ops[1], ins, s), s)]
        if mn == "movzx":
            return [self.wr(ops[0], ins, "(uint32_t)" + self.rd(ops[1], ins), ops[0].size)]
        if mn == "movsx":
            return [self.wr(ops[0], ins, "(uint32_t)(int32_t)(%s)%s" % (STYPES[ops[1].size], self.rd(ops[1], ins)), ops[0].size)]
        if mn == "lea":
            return [self.wr(ops[0], ins, self.addr(ops[1], ins), ops[0].size)]
        if mn == "xchg":
            s = sz(0)
            return ["{ %s t = %s; %s %s }" % (TYPES[s], self.rd(ops[0], ins), self.wr(ops[0], ins, self.rd(ops[1], ins)), self.wr(ops[1], ins, "t"))]
        if mn == "cdq":
            return ["edx = (uint32_t)((int32_t)eax >> 31);"]
        if mn == "cwde":
            return ["eax = (uint32_t)(int32_t)(int16_t)eax;"]
        if mn == "cwd":
            return ["edx = (edx & 0xFFFF0000u) | (uint16_t)((int16_t)eax >> 15);"]
        if mn == "cbw":
            return ["eax = (eax & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)eax;"]
        if mn == "bswap":
            r = self.reg_read(ins.reg_name(ops[0].reg))
            return ["%s = rt_bswap32(%s);" % (r, r)]
        if mn == "xlatb":
            return ["eax = (eax & 0xFFFFFF00u) | RD8(ebx + (uint8_t)eax);"]

        # ---- 栈 ----
        if mn == "push":
            s = ops[0].size
            if s == 4:
                v = self.rd(ops[0], ins, 4)
                if ops[0].type == X86_OP_IMM:
                    v = "0x%Xu" % (ops[0].imm & 0xFFFFFFFF)
                return ["{ uint32_t v = %s; esp -= 4; WR32(esp, v); }" % v]
            v = self.rd(ops[0], ins, 2)
            return ["{ uint16_t v = %s; esp -= 2; WR16(esp, v); }" % v]
        if mn == "pop":
            s = ops[0].size
            if s == 4:
                return ["{ uint32_t v = RD32(esp); esp += 4; %s }" % self.wr(ops[0], ins, "v", 4)]
            return ["{ uint16_t v = RD16(esp); esp += 2; %s }" % self.wr(ops[0], ins, "v", 2)]
        if mn == "leave":
            return ["esp = ebp; ebp = RD32(esp); esp += 4;"]
        if mn in ("pushal", "pushad"):
            return ["{ uint32_t t = esp; esp -= 32; WR32(esp+28, eax); WR32(esp+24, ecx); WR32(esp+20, edx); WR32(esp+16, ebx); WR32(esp+12, t); WR32(esp+8, ebp); WR32(esp+4, esi); WR32(esp, edi); }"]
        if mn in ("popal", "popad"):
            return ["edi = RD32(esp); esi = RD32(esp+4); ebp = RD32(esp+8); ebx = RD32(esp+16); edx = RD32(esp+20); ecx = RD32(esp+24); eax = RD32(esp+28); esp += 32;"]
        if mn in ("pushfd", "pushf"):
            return ["{ uint32_t v = 0x202u | cf | (pf<<2) | (af<<4) | (zf<<6) | (sf<<7) | ((uint32_t)df<<10) | (of<<11); esp -= 4; WR32(esp, v); }"]
        if mn in ("popfd", "popf"):
            return ["{ uint32_t v = RD32(esp); esp += 4; cf = v&1; pf = (v>>2)&1; af = (v>>4)&1; zf = (v>>6)&1; sf = (v>>7)&1; df = (v>>10)&1; of = (v>>11)&1; }"]
        if mn == "lahf":
            return ["eax = (eax & 0xFFFF00FFu) | ((cf | 2u | (pf<<2) | (af<<4) | (zf<<6) | (sf<<7)) << 8);"]
        if mn == "sahf":
            return ["{ uint32_t v = (eax >> 8) & 0xFF; cf = v&1; pf = (v>>2)&1; af = (v>>4)&1; zf = (v>>6)&1; sf = (v>>7)&1; }"]
        if mn == "cld":
            return ["df = 0;"]
        if mn == "std":
            return ["df = 1;"]
        if mn == "clc":
            return ["cf = 0;"]
        if mn == "stc":
            return ["cf = 1;"]
        if mn == "cmc":
            return ["cf = !cf;"]

        # ---- 算术逻辑 ----
        if mn in ("add", "sub", "and", "or", "xor", "cmp", "test", "adc", "sbb"):
            s = sz(0)
            U = TYPES[s]
            op2 = self.rd(ops[1], ins, s)
            pre_stmt = "%s a = %s; %s b = %s; " % (U, self.rd(ops[0], ins), U, op2)
            if mn in ("adc", "sbb"):
                pre_stmt += "uint32_t c = cf; "
            expr = {
                "add": "(a + b)", "sub": "(a - b)", "cmp": "(a - b)", "and": "(a & b)", "test": "(a & b)",
                "or": "(a | b)", "xor": "(a ^ b)", "adc": "(a + b + c)", "sbb": "(a - b - c)",
            }[mn]
            kind = {"and": "logic", "or": "logic", "xor": "logic", "test": "logic", "cmp": "cmp"}.get(mn, mn)
            body = "%s r = (%s)%s; " % (U, U, expr)
            if mn not in ("cmp", "test"):
                body += self.wr(ops[0], ins, "r") + " "
            fl = self.flags(live, kind, s * 8)
            return ["{ " + pre_stmt + body + " ".join(fl) + " }"]
        if mn in ("inc", "dec", "neg"):
            s = sz(0)
            U = TYPES[s]
            expr = {"inc": "a + 1", "dec": "a - 1", "neg": "0 - a"}[mn]
            fl = self.flags(live, mn, s * 8)
            return ["{ %s a = %s; %s r = (%s)(%s); %s %s }" % (U, self.rd(ops[0], ins), U, U, expr, self.wr(ops[0], ins, "r"), " ".join(fl))]
        if mn == "not":
            s = sz(0)
            return ["{ %s a = %s; %s }" % (TYPES[s], self.rd(ops[0], ins), self.wr(ops[0], ins, "(%s)~a" % TYPES[s]))]
        if mn in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr"):
            return self.shift(ins, mn, live)
        if mn in ("shld", "shrd"):
            s = 32
            cnt = self.rd(ops[2], ins, 1)
            fl = []
            L = lambda k: k in live
            if mn == "shld":
                core = "r = (a << c) | (b >> (32 - c)); " + ("cf = (a >> (32 - c)) & 1; " if L("c") else "")
            else:
                core = "r = (a >> c) | (b << (32 - c)); " + ("cf = (a >> (c - 1)) & 1; " if L("c") else "")
            if L("o"):
                core += "of = ((a ^ r) >> 31) & 1; "
            if L("z"):
                core += "zf = (r == 0); "
            if L("s"):
                core += "sf = r >> 31; "
            if L("p"):
                core += "pf = %s; " % self.pf("r")
            return ["{ uint32_t a = %s, b = %s, c = (%s) & 31, r; if (c) { %s %s } }" % (self.rd(ops[0], ins), self.rd(ops[1], ins), cnt, core, self.wr(ops[0], ins, "r"))]
        if mn in ("mul", "imul", "div", "idiv"):
            return self.muldiv(ins, mn, live)
        if mn in ("bt", "bts", "btr", "btc"):
            return self.bittest(ins, mn, live)
        if mn in ("bsf", "bsr"):
            s = sz(0)
            src = self.rd(ops[1], ins, s)
            f1 = "zf = 1;" if "z" in live else ""
            return ["{ uint32_t v = (uint32_t)%s; if (v == 0) { zf = 1; } else { zf = 0; %s } }" % (
                src, self.wr(ops[0], ins, "rt_ctz(v)" if mn == "bsf" else "(31 - rt_clz(v))", s))]
        if mn == "cmpxchg":
            s = sz(0)
            U = TYPES[s]
            acc = {1: "al", 2: "ax", 4: "eax"}[s]
            return ["{ %s d = %s; %s s0 = %s; %s a = %s; %s b = d; %s r = (%s)(a - b); zf = (r == 0); cf = a < b; sf = (r >> %d) & 1; of = (((a ^ b) & (a ^ r)) >> %d) & 1; pf = %s; af = ((a ^ b ^ r) >> 4) & 1; if (zf) { %s } else { %s } }" % (
                U, self.rd(ops[0], ins), U, self.rd(ops[1], ins), U, self.reg_read(acc), U, U, U, s * 8 - 1, s * 8 - 1, self.pf("r"),
                self.wr(ops[0], ins, "s0"), self.reg_write(acc, "d"))]
        if mn == "xadd":
            s = sz(0)
            U = TYPES[s]
            return ["{ %s a = %s; %s b = %s; %s r = (%s)(a + b); %s %s cf = r < a; zf = (r == 0); sf = (r >> %d) & 1; of = (((a ^ r) & (b ^ r)) >> %d) & 1; pf = %s; af = ((a ^ b ^ r) >> 4) & 1; }" % (
                U, self.rd(ops[0], ins), U, self.rd(ops[1], ins), U, U, self.wr(ops[1], ins, "a"), self.wr(ops[0], ins, "r"), s * 8 - 1, s * 8 - 1, self.pf("r"))]

        # ---- 字符串 ----
        base = mn.rstrip("bwd") if mn[:4] in ("movs", "stos", "lods", "scas", "cmps") else mn
        if mn in ("movsb", "movsw", "movsd", "stosb", "stosw", "stosd", "lodsb", "lodsw", "lodsd", "scasb", "scasw", "scasd", "cmpsb", "cmpsw", "cmpsd"):
            return self.string_op(ins, mn, rep, repne, live)

        # ---- x87 ----
        if mn.startswith("f"):
            return self.fpu(ins, mn, live)

        if mn in ("cpuid", "rdtsc", "in", "out", "int", "cli", "sti", "lds", "les", "bound", "arpl", "enter", "salc", "daa", "das", "aaa", "aas", "aam", "aad", "lcall", "ljmp", "insb", "insd", "outsb", "outsd", "into", "iret", "iretd", "xlat"):
            raise Unsupported(mn)
        raise Unsupported(mn)

    # ------------------------------------------------------------ 移位/循环
    def shift(self, ins, mn, live):
        ops = ins.operands
        s = ops[0].size
        B = s * 8
        U = TYPES[s]
        SG = STYPES[s]
        L = lambda k: k in live
        mn = "shl" if mn == "sal" else mn
        const_cnt = None
        if len(ops) == 1:
            cnt_expr = "1"
            const_cnt = 1
        elif ops[1].type == X86_OP_IMM:
            const_cnt = ops[1].imm & 31
            cnt_expr = str(const_cnt)
        else:
            cnt_expr = "((%s) & 31)" % self.reg_read("cl")
        if const_cnt == 0:
            return []
        flagstmts = []
        if mn == "shl":
            core = "uint64_t t = (uint64_t)a << c; r = (%s)t; " % U
            if L("c"):
                core += "cf = (t >> %d) & 1; " % B
            if L("o"):
                core += "of = ((r >> %d) ^ ((t >> %d) & 1)) & 1; " % (B - 1, B)
        elif mn == "shr":
            core = "r = (%s)(a >> c); " % U
            if L("c"):
                core += "cf = (a >> (c - 1)) & 1; "
            if L("o"):
                core += "of = (a >> %d) & 1; " % (B - 1)
        elif mn == "sar":
            core = "r = (%s)((%s)a >> c); " % (U, SG)
            if L("c"):
                core += "cf = ((%s)a >> (c - 1)) & 1; " % SG
            if L("o"):
                core += "of = 0; "
        elif mn == "rol":
            core = "{ uint32_t cc = c %% %d; if (cc) r = (%s)((a << cc) | (a >> (%d - cc))); cf = r & 1; of = ((r >> %d) ^ cf) & 1; } " % (B, U, B, B - 1)
            # rol 不影响 z/s/p
            return ["{ %s a = %s, r; uint32_t c = %s; r = a; if (c) { %s %s } }" % (U, self.rd(ops[0], ins), cnt_expr, core, self.wr(ops[0], ins, "r"))]
        elif mn == "ror":
            core = "{ uint32_t cc = c %% %d; if (cc) r = (%s)((a >> cc) | (a << (%d - cc))); cf = (r >> %d) & 1; of = ((r >> %d) ^ (r >> %d)) & 1; } " % (B, U, B, B - 1, B - 1, B - 2)
            return ["{ %s a = %s, r; uint32_t c = %s; r = a; if (c) { %s %s } }" % (U, self.rd(ops[0], ins), cnt_expr, core, self.wr(ops[0], ins, "r"))]
        elif mn in ("rcl", "rcr"):
            if mn == "rcl":
                loop = "for (uint32_t i = 0; i < c; i++) { uint32_t nc = (r >> %d) & 1; r = (%s)((r << 1) | cf); cf = nc; } of = ((r >> %d) ^ cf) & 1; " % (B - 1, U, B - 1)
            else:
                loop = "of = ((r >> %d) ^ cf) & 1; for (uint32_t i = 0; i < c; i++) { uint32_t nc = r & 1; r = (%s)((r >> 1) | ((uint32_t)cf << %d)); cf = nc; } " % (B - 1, U, B - 1)
            return ["{ %s r = %s; uint32_t c = %s %% %d; %s %s }" % (U, self.rd(ops[0], ins), cnt_expr, B + 1, loop, self.wr(ops[0], ins, "r"))]
        if L("z"):
            core += "zf = (r == 0); "
        if L("s"):
            core += "sf = (r >> %d) & 1; " % (B - 1)
        if L("p"):
            core += "pf = %s; " % self.pf("r")
        if const_cnt is not None:
            return ["{ %s a = %s, r; uint32_t c = %d; %s %s }" % (U, self.rd(ops[0], ins), const_cnt, core, self.wr(ops[0], ins, "r"))]
        return ["{ %s a = %s, r; uint32_t c = %s; if (c) { %s %s } }" % (U, self.rd(ops[0], ins), cnt_expr, core, self.wr(ops[0], ins, "r"))]

    # ------------------------------------------------------------ 乘除
    def muldiv(self, ins, mn, live):
        ops = ins.operands
        n = len(ops)
        if mn == "imul" and n >= 2:
            s = ops[0].size
            if n == 2:
                a, b = self.rd(ops[0], ins), self.rd(ops[1], ins, s)
            else:
                a, b = self.rd(ops[1], ins, s), "(%s)%s" % (TYPES[s], ("0x%Xu" % (ops[2].imm & MASKS[s])))
            SG = STYPES[s]
            fl = ""
            if "c" in live or "o" in live:
                fl = "cf = of = (t != (int64_t)(%s)r); " % SG
            return ["{ int64_t t = (int64_t)(%s)(%s) * (int64_t)(%s)(%s); %s r = (%s)t; %s%s }" % (SG, a, SG, b, TYPES[s], TYPES[s], fl, self.wr(ops[0], ins, "r"))]
        s = ops[0].size
        src = self.rd(ops[0], ins)
        if mn in ("mul", "imul"):
            sg = mn == "imul"
            if s == 1:
                T = "int" if sg else "unsigned"
                fl = "cf = of = ((uint16_t)eax >> 8) != 0; " if not sg else "cf = of = ((int16_t)(eax & 0xFFFF) != (int8_t)eax); "
                if sg:
                    return ["{ int32_t t = (int32_t)(int8_t)eax * (int32_t)(int8_t)(%s); eax = (eax & 0xFFFF0000u) | (uint16_t)t; cf = of = (t != (int8_t)t); }" % src]
                return ["{ uint32_t t = (uint32_t)(uint8_t)eax * (uint32_t)(%s); eax = (eax & 0xFFFF0000u) | (uint16_t)t; cf = of = (t > 0xFF); }" % src]
            if s == 2:
                if sg:
                    return ["{ int32_t t = (int32_t)(int16_t)eax * (int32_t)(int16_t)(%s); eax = (eax & 0xFFFF0000u) | (uint16_t)t; edx = (edx & 0xFFFF0000u) | (uint16_t)((uint32_t)t >> 16); cf = of = (t != (int16_t)t); }" % src]
                return ["{ uint32_t t = (uint32_t)(uint16_t)eax * (uint32_t)(%s); eax = (eax & 0xFFFF0000u) | (uint16_t)t; edx = (edx & 0xFFFF0000u) | (uint16_t)(t >> 16); cf = of = (t >> 16) != 0; }" % src]
            if sg:
                return ["{ int64_t t = (int64_t)(int32_t)eax * (int64_t)(int32_t)(%s); eax = (uint32_t)t; edx = (uint32_t)((uint64_t)t >> 32); cf = of = (t != (int32_t)t); }" % src]
            return ["{ uint64_t t = (uint64_t)eax * (uint64_t)(%s); eax = (uint32_t)t; edx = (uint32_t)(t >> 32); cf = of = (edx != 0); }" % src]
        # div / idiv
        sg = mn == "idiv"
        if s == 1:
            if sg:
                return ["{ int32_t d = (int8_t)(%s); int32_t n = (int16_t)eax; RT_DIVCHK(d); int32_t q = n / d, r = n %% d; eax = (eax & 0xFFFF0000u) | (uint8_t)q | ((uint32_t)(uint8_t)r << 8); }" % src]
            return ["{ uint32_t d = (uint8_t)(%s); uint32_t n = (uint16_t)eax; RT_DIVCHK(d); uint32_t q = n / d, r = n %% d; eax = (eax & 0xFFFF0000u) | (uint8_t)q | ((uint32_t)(uint8_t)r << 8); }" % src]
        if s == 2:
            if sg:
                return ["{ int32_t d = (int16_t)(%s); int32_t n = (int32_t)(((uint32_t)(uint16_t)edx << 16) | (uint16_t)eax); RT_DIVCHK(d); int32_t q = n / d, r = n %% d; eax = (eax & 0xFFFF0000u) | (uint16_t)q; edx = (edx & 0xFFFF0000u) | (uint16_t)r; }" % src]
            return ["{ uint32_t d = (uint16_t)(%s); uint32_t n = ((uint32_t)(uint16_t)edx << 16) | (uint16_t)eax; RT_DIVCHK(d); uint32_t q = n / d, r = n %% d; eax = (eax & 0xFFFF0000u) | (uint16_t)q; edx = (edx & 0xFFFF0000u) | (uint16_t)r; }" % src]
        if sg:
            return ["{ int64_t d = (int32_t)(%s); int64_t n = (int64_t)(((uint64_t)edx << 32) | eax); RT_DIVCHK(d); int64_t q = n / d, r = n %% d; eax = (uint32_t)q; edx = (uint32_t)r; }" % src]
        return ["{ uint64_t d = (uint32_t)(%s); uint64_t n = ((uint64_t)edx << 32) | eax; RT_DIVCHK(d); uint64_t q = n / d, r = n %% d; eax = (uint32_t)q; edx = (uint32_t)r; }" % src]

    # ------------------------------------------------------------ 位测试
    def bittest(self, ins, mn, live):
        ops = ins.operands
        s = ops[0].size
        B = s * 8
        U = TYPES[s]
        if ops[0].type == X86_OP_MEM and ops[1].type != X86_OP_IMM:
            raise Unsupported("bt mem,reg")
        bit = "(%s) %% %d" % (self.rd(ops[1], ins, s), B)
        act = {"bt": "", "bts": " | (1u << k)", "btr": " & ~(1u << k)", "btc": " ^ (1u << k)"}[mn]
        w = ""
        if mn != "bt":
            w = self.wr(ops[0], ins, "(%s)(v%s)" % (U, act))
        return ["{ %s v = %s; uint32_t k = %s; cf = (v >> k) & 1; %s }" % (U, self.rd(ops[0], ins), bit, w)]

    # ------------------------------------------------------------ 字符串
    def string_op(self, ins, mn, rep, repne, live):
        w = {"b": 1, "w": 2, "d": 4}[mn[-1]]
        kind = mn[:-1]
        pre, _ = split_mn(ins.mnemonic)
        repkind = "0"
        if "repne" in pre:
            repkind = "2"
        elif "repe" in pre or "rep" in pre:
            repkind = "1"
        if kind == "movs":
            return ["{ uint32_t s_ = esi, d_ = edi; rt_movs(m, &s_, &d_, %s, %d, df); esi = s_; edi = d_; %s }" % ("ecx" if repkind != "0" else "1", w, "ecx = 0;" if repkind != "0" else "")]
        if kind == "stos":
            val = {1: "(uint8_t)eax", 2: "(uint16_t)eax", 4: "eax"}[w]
            return ["{ uint32_t d_ = edi; rt_stos(m, &d_, %s, %d, df, %s); edi = d_; %s }" % ("ecx" if repkind != "0" else "1", w, val, "ecx = 0;" if repkind != "0" else "")]
        if kind == "lods":
            rr = {1: "eax = (eax & 0xFFFFFF00u) | RD8(esi);", 2: "eax = (eax & 0xFFFF0000u) | RD16(esi);", 4: "eax = RD32(esi);"}[w]
            if repkind == "0":
                return ["{ %s esi += df ? -%d : %d; }" % (rr, w, w)]
            return ["while (ecx) { %s esi += df ? -%d : %d; ecx--; }" % (rr, w, w)]
        # scas / cmps：运行时返回比较的两个数
        U = TYPES[w]
        acc = {1: "(uint8_t)eax", 2: "(uint16_t)eax", 4: "eax"}[w]
        B = w * 8
        fl = self.flags(live, "cmp", B)
        if kind == "scas":
            call = "ran = rt_scas(m, &d_, &c_, %d, df, %s, %s, &ra, &rb);" % (w, repkind, acc)
        else:
            call = "ran = rt_cmps(m, &s_, &d_, &c_, %d, df, %s, &ra, &rb);" % (w, repkind)
        return ["{ uint32_t ra, rb, s_ = esi, d_ = edi, c_ = ecx; int ran; %s esi = s_; edi = d_; ecx = c_; if (ran) { %s a = (%s)ra; %s b = (%s)rb; %s r = (%s)(a - b); %s } }" % (call, U, U, U, U, U, U, " ".join(fl))]

    # ------------------------------------------------------------ x87
    def fpu(self, ins, mn, live):
        ops = ins.operands
        n = len(ops)

        def isst(o):
            return o.type == X86_OP_REG and ins.reg_name(o.reg).startswith("st")

        def sti(o):
            nm = ins.reg_name(o.reg)
            return int(nm[3:-1]) if "(" in nm else int(nm[2:])

        def memaddr(o):
            return self.addr(o, ins)

        mn_ = mn
        if mn_ in ("fld", "fild", "fst", "fstp", "fist", "fistp", "fisttp"):
            o = ops[0]
            if mn_ == "fld":
                if isst(o):
                    return ["fp_push(ST(%d));" % sti(o)]
                if o.size == 4:
                    return ["fp_push((double)RDF32(%s));" % memaddr(o)]
                if o.size == 8:
                    return ["fp_push(RDF64(%s));" % memaddr(o)]
                return ["fp_push(rt_ld80(m + %s));" % memaddr(o)]
            if mn_ == "fild":
                SG = STYPES[o.size]
                return ["fp_push((double)(%s)RD%d(%s));" % (SG, o.size * 8, memaddr(o))]
            if mn_ in ("fst", "fstp"):
                pop = "fp_pop();" if mn_ == "fstp" else ""
                if isst(o):
                    return ["ST(%d) = ST(0); %s" % (sti(o), pop)]
                if o.size == 4:
                    return ["WRF32(%s, (float)ST(0)); %s" % (memaddr(o), pop)]
                if o.size == 8:
                    return ["WRF64(%s, ST(0)); %s" % (memaddr(o), pop)]
                return ["rt_st80(m + %s, ST(0)); %s" % (memaddr(o), pop)]
            # fist/fistp/fisttp
            pop = "fp_pop();" if mn_ in ("fistp", "fisttp") else ""
            conv = "fp_trunc(ST(0))" if mn_ == "fisttp" else "fp_round(ST(0))"
            sz_ = o.size
            return ["WR%d(%s, (%s)%s); %s" % (sz_ * 8, memaddr(o), TYPES[sz_], conv if sz_ == 8 else "(int64_t)%s" % conv, pop)]
        if mn_ == "fxch":
            i = sti(ops[0]) if n else 1
            return ["fp_xch(%d);" % i]
        if mn_ in ("fld1", "fldz", "fldpi", "fldl2e", "fldl2t", "fldln2", "fldlg2"):
            c = {"fld1": "1.0", "fldz": "0.0", "fldpi": "3.14159265358979323846", "fldl2e": "1.44269504088896340736",
                 "fldl2t": "3.32192809488736234787", "fldln2": "0.693147180559945309417", "fldlg2": "0.301029995663981195214"}[mn_]
            return ["fp_push(%s);" % c]
        simple = {"fchs": "ST(0) = -ST(0);", "fabs": "ST(0) = fabs(ST(0));", "fsqrt": "ST(0) = sqrt(ST(0));",
                  "fsin": "ST(0) = sin(ST(0)); fp_c2(0);", "fcos": "ST(0) = cos(ST(0)); fp_c2(0);",
                  "frndint": "ST(0) = fp_rint(ST(0));", "fscale": "ST(0) = ldexp(ST(0), (int)trunc(ST(1)));",
                  "f2xm1": "ST(0) = pow(2.0, ST(0)) - 1.0;", "fyl2x": "ST(1) = ST(1) * log2(ST(0)); fp_pop();",
                  "fyl2xp1": "ST(1) = ST(1) * log2(ST(0) + 1.0); fp_pop();",
                  "fpatan": "ST(1) = atan2(ST(1), ST(0)); fp_pop();", "fptan": "ST(0) = tan(ST(0)); fp_push(1.0); fp_c2(0);",
                  "fprem": "fp_prem(0);", "fprem1": "fp_prem(1);", "fnclex": "fp_clex();", "fclex": "fp_clex();",
                  "fninit": "fp_init();", "finit": "fp_init();", "fincstp": "fp_inc();", "fdecstp": "fp_dec();",
                  "ftst": "fp_com(ST(0), 0.0);", "fxam": "fp_xam();"}
        if mn_ in simple:
            return [simple[mn_]]
        if mn_ == "ffree" or mn_ == "ffreep":
            return ["" if mn_ == "ffree" else "fp_pop();"]
        if mn_ in ("fnstsw", "fstsw"):
            if ops[0].type == X86_OP_REG:
                return ["eax = (eax & 0xFFFF0000u) | fp_sw();"]
            return ["WR16(%s, fp_sw());" % memaddr(ops[0])]
        if mn_ in ("fnstcw", "fstcw"):
            return ["WR16(%s, fp_cw);" % memaddr(ops[0])]
        if mn_ == "fldcw":
            return ["fp_cw = RD16(%s);" % memaddr(ops[0])]
        if mn_ in ("fnsave", "fsave"):
            return ["rt_fsave(m + %s);" % memaddr(ops[0])]
        if mn_ == "frstor":
            return ["rt_frstor(m + %s);" % memaddr(ops[0])]
        if mn_ in ("fnstenv", "fstenv"):
            return ["rt_fstenv(m + %s);" % memaddr(ops[0])]
        if mn_ == "fldenv":
            return ["rt_fldenv(m + %s);" % memaddr(ops[0])]
        # 比较
        if mn_ in ("fcom", "fcomp", "fucom", "fucomp", "fcompp", "fucompp", "ficom", "ficomp"):
            if mn_ in ("fcompp", "fucompp"):
                return ["fp_com(ST(0), ST(1)); fp_pop(); fp_pop();"]
            if n == 0:
                src = "ST(1)"
            elif isst(ops[0]):
                src = "ST(%d)" % sti(ops[0])
            elif mn_.startswith("fi"):
                src = "(double)(%s)RD%d(%s)" % (STYPES[ops[0].size], ops[0].size * 8, memaddr(ops[0]))
            elif ops[0].size == 4:
                src = "(double)RDF32(%s)" % memaddr(ops[0])
            else:
                src = "RDF64(%s)" % memaddr(ops[0])
            pops = "fp_pop();" if mn_.endswith("p") else ""
            return ["fp_com(ST(0), %s); %s" % (src, pops)]
        if mn_ in ("fcomi", "fcomip", "fucomi", "fucomip"):
            i = sti(ops[1]) if n == 2 else sti(ops[0])
            pops = "fp_pop();" if mn_.endswith("p") else ""
            return ["{ double x = ST(0), y = ST(%d); %s cf = (x < y) || (x != x) || (y != y); zf = (x == y) || (x != x) || (y != y); pf = (x != x) || (y != y); of = sf = af = 0; } %s" % (i, "", pops)]
        # 算术
        arith = {"fadd": "+", "fsub": "-", "fmul": "*", "fdiv": "/", "faddp": "+", "fsubp": "-", "fmulp": "*", "fdivp": "/",
                 "fiadd": "+", "fisub": "-", "fimul": "*", "fidiv": "/",
                 "fsubr": "r-", "fdivr": "r/", "fsubrp": "r-", "fdivrp": "r/", "fisubr": "r-", "fidivr": "r/"}
        if mn_ in arith:
            opc = arith[mn_]
            rev = opc.startswith("r")
            c = opc[-1]
            pop = mn_.endswith("p") and mn_ not in ("fiadd",)  # faddp 等
            if mn_.startswith("fi"):
                pop = False
            if n == 2 and isst(ops[0]) and isst(ops[1]):
                d, s_ = sti(ops[0]), sti(ops[1])
                a_, b_ = ("ST(%d)" % d, "ST(%d)" % s_)
                if rev:
                    a_, b_ = b_, a_
                return ["ST(%d) = %s %s %s; %s" % (d, a_, c, b_, "fp_pop();" if pop else "")]
            if n == 1 and isst(ops[0]):
                d = sti(ops[0])
                a_, b_ = "ST(%d)" % d, "ST(0)"
                if rev:
                    a_, b_ = b_, a_
                return ["ST(%d) = %s %s %s; %s" % (d, a_, c, b_, "fp_pop();" if pop else "")]
            if n == 0:
                # faddp 无操作数 -> ST(1) op= ST(0); pop
                a_, b_ = "ST(1)", "ST(0)"
                if rev:
                    a_, b_ = b_, a_
                return ["ST(1) = %s %s %s; fp_pop();" % (a_, c, b_)]
            o = ops[0]
            if mn_.startswith("fi"):
                src = "(double)(%s)RD%d(%s)" % (STYPES[o.size], o.size * 8, memaddr(o))
            elif o.size == 4:
                src = "(double)RDF32(%s)" % memaddr(o)
            else:
                src = "RDF64(%s)" % memaddr(o)
            a_, b_ = "ST(0)", src
            if rev:
                a_, b_ = b_, a_
            return ["ST(0) = %s %s %s;" % (a_, c, b_)]
        raise Unsupported(mn)


COND_JUMPS_LIFT = {"j" + c for c in COND} | {"jecxz", "jcxz"}
