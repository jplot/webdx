// sm3_wgsl.cpp — vs_3_0 / ps_3_0 bytecode to WGSL (see sm3_wgsl.h for the binding contract).
#include "sm3_wgsl.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace {

enum {
    OP_MOV = 1, OP_ADD = 2, OP_SUB = 3, OP_MAD = 4, OP_MUL = 5, OP_RCP = 6, OP_RSQ = 7,
    OP_DP3 = 8, OP_DP4 = 9, OP_MIN = 10, OP_MAX = 11, OP_SLT = 12, OP_SGE = 13,
    OP_EXP = 14, OP_LOG = 15, OP_LRP = 18, OP_FRC = 19, OP_DCL = 31, OP_POW = 32,
    OP_ABS = 35, OP_NRM = 36, OP_SINCOS = 37, OP_REP = 38, OP_ENDREP = 39, OP_IF = 40,
    OP_IFC = 41, OP_ELSE = 42, OP_ENDIF = 43, OP_BREAK = 44, OP_BREAKC = 45, OP_MOVA = 46,
    OP_DEFB = 47, OP_DEFI = 48, OP_TEXKILL = 65, OP_TEXLD = 66, OP_DEF = 81, OP_CMP = 88,
    OP_DP2ADD = 90, OP_DSX = 91, OP_DSY = 92, OP_TEXLDD = 93, OP_TEXLDL = 95,
    OP_COMMENT = 0xFFFE, OP_END = 0xFFFF,
};
enum {
    RT_TEMP = 0, RT_INPUT = 1, RT_CONST = 2, RT_ADDR = 3, RT_RASTOUT = 4, RT_ATTROUT = 5,
    RT_OUTPUT = 6, RT_CONSTINT = 7, RT_COLOROUT = 8, RT_DEPTHOUT = 9, RT_SAMPLER = 10,
    RT_CONSTBOOL = 14, RT_LOOP = 15, RT_MISC = 17,
};
enum { USAGE_POSITION = 0, USAGE_BLENDWEIGHT = 1, USAGE_BLENDINDICES = 2, USAGE_NORMAL = 3,
       USAGE_TEXCOORD = 5, USAGE_TANGENT = 6, USAGE_POSITIONT = 9, USAGE_COLOR = 10 };
enum { DIM_2D = 2, DIM_CUBE = 3, DIM_VOLUME = 4 };

struct Param {
    int type = 0, reg = 0, swizzle = 0xE4, mask = 0xF, srcMod = 0, dstMod = 0;
    bool relative = false;
    int relType = 0, relComp = 0;
};

Param Decode(uint32_t t)
{
    Param p;
    p.reg = (int)(t & 0x7FF);
    p.type = (int)(((t >> 28) & 0x7) | ((t & 0x1800) >> 8));
    p.mask = (int)((t >> 16) & 0xF);
    p.swizzle = (int)((t >> 16) & 0xFF);
    p.srcMod = (int)((t >> 24) & 0xF);
    p.dstMod = (int)((t >> 20) & 0xF);
    return p;
}

int DecodeWithRelative(const uint32_t *tok, Param &p)
{
    p = Decode(tok[0]);
    if (!(tok[0] & 0x2000))
        return 1;
    Param a = Decode(tok[1]);
    p.relative = true;
    p.relType = a.type;
    p.relComp = a.swizzle & 0x3;
    return 2;
}

const char kComp[] = "xyzw";

int VaryingLocation(int usage, int index)
{
    if (usage == USAGE_TEXCOORD && index <= 9) return index;
    if (usage == USAGE_COLOR && index <= 1) return 10 + index;
    return -1;
}

struct Translator {
    bool pixel = false;
    Sm3WgslOptions opt;
    Sm3WgslResult res;
    std::map<int, std::pair<int, int>> inputs, outputs;  // register -> (usage, index)
    std::map<int, int> samplerDim;
    std::map<int, std::vector<float>> defs;
    std::map<int, std::vector<int>> idefs;
    std::set<int> temps;
    bool usesA0 = false, usesPos = false, usesFace = false, usesDepth = false;
    bool usesConst = false, usesIntConst = false, usesRelConst = false;
    std::set<int> colorOuts;
    std::ostringstream body;
    int depth = 1, loops = 0, nextTemp = 0;

    std::string Indent() const { return std::string(4 * depth, ' '); }
    const char *ConstArray() const { return pixel ? "psc" : "vsc"; }

    std::string Fail(const std::string &why)
    {
        if (res.error.empty()) res.error = why;
        return "vec4f(0.0)";
    }

    std::string Register(const Param &p)
    {
        std::ostringstream s;
        switch (p.type) {
        case RT_TEMP:
            temps.insert(p.reg);
            s << "r" << p.reg;
            return s.str();
        case RT_CONST:
            if (p.relative) {
                if (p.relType != RT_ADDR) return Fail("relative addressing through a non-a0 register");
                usesConst = usesA0 = usesRelConst = true;
                res.maxConst = 255;
                s << ConstArray() << "_rel(a0." << kComp[p.relComp] << " + " << p.reg << ")";
                return s.str();
            }
            if (defs.count(p.reg)) { s << "c" << p.reg << "_def"; return s.str(); }
            usesConst = true;
            if (p.reg > res.maxConst) res.maxConst = p.reg;
            s << ConstArray() << ".c[" << p.reg << "]";
            return s.str();
        case RT_INPUT: {
            auto it = inputs.find(p.reg);
            if (it == inputs.end()) return "vec4f(0.0)";
            if (pixel) {
                int loc = VaryingLocation(it->second.first, it->second.second);
                if (loc < 0) return Fail("pixel input with an unsupported semantic");
                res.varyingMask |= 1u << loc;
                if (!(opt.varyingMask & (1u << loc))) return "vec4f(0.0)";
            }
            if (!pixel) {
                int loc = SM3AttribLocation(it->second.first, it->second.second);
                if (loc < 0) return Fail("vertex input with an unsupported semantic");
                if (!(opt.attribMask & (1u << loc))) return "vec4f(0.0, 0.0, 0.0, 1.0)";
                if ((opt.attribUintMask | opt.attribSintMask) & (1u << loc)) {
                    s << "vec4f(vin.v" << p.reg << ")";
                    return s.str();
                }
            }
            s << "vin.v" << p.reg;
            return s.str();
        }
        case RT_MISC:
            // Direct3D 9 puts pixel centers on integers, WebGPU halfway between.
            if (p.reg == 0) { usesPos = true; return "vec4f(vin.fragPos.xy - vec2f(0.5), vin.fragPos.zw)"; }
            usesFace = true;
            return "select(vec4f(-1.0), vec4f(1.0), vin.face)";
        case RT_CONSTINT:
            if (idefs.count(p.reg)) { s << "vec4f(i" << p.reg << "_def)"; return s.str(); }
            usesIntConst = true;
            s << "vec4f(" << (pixel ? "psi" : "vsi") << ".c[" << p.reg << "])";
            return s.str();
        default:
            return Fail("unsupported source register type " + std::to_string(p.type));
        }
    }

    std::string Source(const Param &p)
    {
        std::string e = Register(p);
        if (p.swizzle != 0xE4) {
            e += ".";
            for (int i = 0; i < 4; ++i) e += kComp[(p.swizzle >> (2 * i)) & 3];
        }
        switch (p.srcMod) {
        case 0:  return e;
        case 1:  return "(-" + e + ")";
        case 11: return "abs(" + e + ")";
        case 12: return "(-abs(" + e + "))";
        default: return Fail("unsupported source modifier " + std::to_string(p.srcMod));
        }
    }

    // Destination variable name; float registers are vec4f, a0 is vec4i.
    std::string Destination(const Param &p, bool &integer)
    {
        integer = false;
        std::ostringstream s;
        switch (p.type) {
        case RT_TEMP: temps.insert(p.reg); s << "r" << p.reg; return s.str();
        case RT_ADDR: usesA0 = true; integer = true; return "a0";
        case RT_OUTPUT:
            if (pixel) { Fail("output register in a pixel shader"); return "r_unused"; }
            s << "o" << p.reg;
            return s.str();
        case RT_COLOROUT: colorOuts.insert(p.reg); s << "oC" << p.reg; return s.str();
        case RT_DEPTHOUT: usesDepth = true; return "oDepth";
        default: Fail("unsupported destination register type " + std::to_string(p.type)); return "r_unused";
        }
    }

    void Write(const Param &dst, const std::string &expr)
    {
        bool integer;
        std::string d = Destination(dst, integer);
        std::string value = (dst.dstMod & 1) ? "saturate(" + expr + ")" : expr;
        if (integer) value = "vec4i(floor(" + value + " + vec4f(0.5)))";
        if (dst.mask == 0xF) { body << Indent() << d << " = " << value << ";\n"; return; }
        std::string t = "t" + std::to_string(nextTemp++);
        body << Indent() << "let " << t << " = " << value << ";\n";
        body << Indent() << d << " = " << (integer ? "vec4i(" : "vec4f(");
        for (int i = 0; i < 4; ++i)
            body << (i ? ", " : "") << ((dst.mask >> i) & 1 ? t : d) << "." << kComp[i];
        body << ");\n";
    }

    std::string Coords(int dim, const std::string &c)
    {
        return "(" + c + ")" + (dim == DIM_2D ? ".xy" : ".xyz");
    }

    std::string Sample(int op, int ctrl, const Param *src, int nsrc)
    {
        if (nsrc < 2 || src[1].type != RT_SAMPLER) return Fail("texture instruction without a sampler");
        int s = src[1].reg;
        auto dimIt = samplerDim.find(s);
        if (dimIt == samplerDim.end()) return Fail("sampler used without dcl");
        int dim = dimIt->second;
        res.samplerMask |= 1u << s;
        if (dim == DIM_2D) res.sampler2DMask |= 1u << s;
        else if (dim == DIM_CUBE) res.samplerCubeMask |= 1u << s;
        else if (dim == DIM_VOLUME) res.samplerVolumeMask |= 1u << s;
        std::string t = (pixel ? "tex" : "vs_tex") + std::to_string(s);
        std::string smp = (pixel ? "smp" : "vs_smp") + std::to_string(s);
        std::string c = Source(src[0]);
        bool projected = op == OP_TEXLD && ctrl == 1;
        bool explicitLod = op == OP_TEXLDL || !pixel;
        if (pixel && ((opt.shadowMask >> s) & 1)) {
            if (dim != DIM_2D) return Fail("comparison sampling on a non-2D sampler");
            std::string p = projected ? "((" + c + ").xyz / (" + c + ").w)" : "(" + c + ").xyz";
            return explicitLod ? "vec4f(textureSampleCompareLevel(" + t + ", " + smp + ", " + p + ".xy, " + p + ".z))"
                               : "vec4f(textureSampleCompare(" + t + ", " + smp + ", " + p + ".xy, " + p + ".z))";
        }
        std::string coords = projected ? "((" + c + ")" + (dim == DIM_2D ? ".xy" : ".xyz") + " / (" + c + ").w)"
                                       : Coords(dim, c);
        if (op == OP_TEXLDD) {
            if (nsrc < 4) return Fail("texldd without gradients");
            return "textureSampleGrad(" + t + ", " + smp + ", " + coords + ", " + Coords(dim, Source(src[2])) + ", " +
                   Coords(dim, Source(src[3])) + ")";
        }
        if (op == OP_TEXLD && ctrl == 2)
            return "textureSampleBias(" + t + ", " + smp + ", " + coords + ", (" + c + ").w)";
        if (explicitLod)
            return "textureSampleLevel(" + t + ", " + smp + ", " + coords + ", " +
                   (op == OP_TEXLDL ? "(" + c + ").w" : std::string("0.0")) + ")";
        return "textureSample(" + t + ", " + smp + ", " + coords + ")";
    }

    std::string Compare(int ctrl, const Param &a, const Param &b)
    {
        static const char *ops[] = { "", ">", "==", ">=", "<", "!=", "<=" };
        if (ctrl < 1 || ctrl > 6) { Fail("bad comparison"); return "false"; }
        return "(" + Source(a) + ").x " + ops[ctrl] + " (" + Source(b) + ").x";
    }

    void Instruction(int op, int ctrl, const Param &dst, const Param *src, int nsrc)
    {
        auto s = [&](int i) { return Source(src[i]); };
        std::string e;
        switch (op) {
        case OP_MOV:    e = s(0); break;
        case OP_ADD:    e = s(0) + " + " + s(1); break;
        case OP_SUB:    e = s(0) + " - " + s(1); break;
        case OP_MUL:    e = s(0) + " * " + s(1); break;
        case OP_MAD:    e = s(0) + " * " + s(1) + " + " + s(2); break;
        case OP_LRP:    e = "mix(" + s(2) + ", " + s(1) + ", " + s(0) + ")"; break;
        case OP_MIN:    e = "min(" + s(0) + ", " + s(1) + ")"; break;
        case OP_MAX:    e = "max(" + s(0) + ", " + s(1) + ")"; break;
        case OP_FRC:    e = "fract(" + s(0) + ")"; break;
        case OP_ABS:    e = "abs(" + s(0) + ")"; break;
        case OP_SLT:    e = "vec4f(" + s(0) + " < " + s(1) + ")"; break;
        case OP_SGE:    e = "vec4f(" + s(0) + " >= " + s(1) + ")"; break;
        case OP_CMP:    e = "select(" + s(2) + ", " + s(1) + ", " + s(0) + " >= vec4f(0.0))"; break;
        case OP_RCP:    e = "vec4f(1.0 / (" + s(0) + ").x)"; break;
        case OP_RSQ:    e = "vec4f(inverseSqrt(abs((" + s(0) + ").x)))"; break;
        case OP_EXP:    e = "vec4f(exp2((" + s(0) + ").x))"; break;
        case OP_LOG:    e = "vec4f(log2(abs((" + s(0) + ").x)))"; break;
        case OP_POW:    e = "vec4f(pow(abs((" + s(0) + ").x), (" + s(1) + ").x))"; break;
        case OP_NRM:    e = "(" + s(0) + ") * inverseSqrt(dot((" + s(0) + ").xyz, (" + s(0) + ").xyz))"; break;
        case OP_SINCOS: e = "vec4f(cos((" + s(0) + ").x), sin((" + s(0) + ").x), 0.0, 0.0)"; break;
        case OP_DP3:    e = "vec4f(dot((" + s(0) + ").xyz, (" + s(1) + ").xyz))"; break;
        case OP_DP4:    e = "vec4f(dot(" + s(0) + ", " + s(1) + "))"; break;
        case OP_DP2ADD: e = "vec4f(dot((" + s(0) + ").xy, (" + s(1) + ").xy) + (" + s(2) + ").x)"; break;
        case OP_DSX:    e = "dpdx(" + s(0) + ")"; break;
        case OP_DSY:    e = "dpdy(" + s(0) + ")"; break;
        case OP_MOVA:   e = s(0); break;
        case OP_TEXLD: case OP_TEXLDL: case OP_TEXLDD:
            e = Sample(op, ctrl, src, nsrc);
            break;
        case OP_TEXKILL:
            body << Indent() << "if (any(" << Register(dst) << ".xyz < vec3f(0.0))) { discard; }\n";
            return;
        default:
            Fail("unsupported opcode " + std::to_string(op));
            return;
        }
        Write(dst, e);
    }

    void Parse(const uint32_t *tok)
    {
        for (;;) {
            uint32_t t = *tok++;
            int op = (int)(t & 0xFFFF);
            if (op == OP_END) break;
            if (op == OP_COMMENT) { tok += (t >> 16) & 0x7FFF; continue; }
            int len = (int)((t >> 24) & 0xF);
            int ctrl = (int)((t >> 16) & 0xFF);
            if (t & 0x10000000) Fail("predicated instruction");
            switch (op) {
            case OP_DCL: {
                uint32_t usage = tok[0];
                Param reg = Decode(tok[1]);
                if (reg.type == RT_SAMPLER) samplerDim[reg.reg] = (int)((usage >> 27) & 0xF);
                else if (reg.type == RT_INPUT) inputs[reg.reg] = {(int)(usage & 0x1F), (int)((usage >> 16) & 0xF)};
                else if (reg.type == RT_OUTPUT) outputs[reg.reg] = {(int)(usage & 0x1F), (int)((usage >> 16) & 0xF)};
                else if (reg.type == RT_MISC && reg.reg == 0) usesPos = true;
                else if (reg.type == RT_MISC) usesFace = true;
                break;
            }
            case OP_DEF: {
                std::vector<float> f(4);
                std::memcpy(f.data(), &tok[1], 16);
                defs[Decode(tok[0]).reg] = f;
                break;
            }
            case OP_DEFI: idefs[Decode(tok[0]).reg] = {(int)tok[1], (int)tok[2], (int)tok[3], (int)tok[4]}; break;
            case OP_DEFB: Fail("boolean constants"); break;
            case OP_IFC: {
                Param a = Decode(tok[0]), b = Decode(tok[1]);
                body << Indent() << "if (" << Compare(ctrl, a, b) << ") {\n";
                ++depth;
                break;
            }
            case OP_IF: Fail("if on a boolean register"); break;
            case OP_ELSE: --depth; body << Indent() << "} else {\n"; ++depth; break;
            case OP_ENDIF: case OP_ENDREP: --depth; body << Indent() << "}\n"; break;
            case OP_BREAK: body << Indent() << "break;\n"; break;
            case OP_BREAKC: {
                Param a = Decode(tok[0]), b = Decode(tok[1]);
                body << Indent() << "if (" << Compare(ctrl, a, b) << ") { break; }\n";
                break;
            }
            case OP_REP: {
                Param n = Decode(tok[0]);
                std::string count = idefs.count(n.reg) ? "i" + std::to_string(n.reg) + "_def.x"
                                                       : std::string(pixel ? "psi" : "vsi") + ".c[" + std::to_string(n.reg) + "].x";
                if (!idefs.count(n.reg)) usesIntConst = true;
                int id = loops++;
                body << Indent() << "for (var aL" << id << ": i32 = 0; aL" << id << " < " << count << "; aL" << id << "++) {\n";
                ++depth;
                break;
            }
            default: {
                Param dst, src[4];
                int k = DecodeWithRelative(tok, dst), nsrc = 0;
                while (k < len && nsrc < 4) k += DecodeWithRelative(tok + k, src[nsrc++]);
                Instruction(op, ctrl, dst, src, nsrc);
                break;
            }
            }
            tok += len;
        }
    }

    std::string Assemble()
    {
        std::ostringstream o;
        o << "diagnostic(off, derivative_uniformity);\n\n";
        o << "struct Consts { c: array<vec4f, 256> }\n";
        o << "struct IntConsts { c: array<vec4i, 16> }\n";
        if (usesConst) o << "@group(0) @binding(" << (pixel ? 1 : 0) << ") var<uniform> " << ConstArray() << ": Consts;\n";
        if (usesIntConst) o << "@group(0) @binding(" << (pixel ? 4 : 3) << ") var<uniform> " << (pixel ? "psi" : "vsi") << ": IntConsts;\n";
        o << "struct DrawParams { alphaFunc: i32, alphaRef: f32, pixelShift: vec2f }\n";
        o << "@group(0) @binding(2) var<uniform> kb: DrawParams;\n";
        for (auto &sd : samplerDim) {
            int s = sd.first, dim = sd.second;
            bool shadow = pixel && ((opt.shadowMask >> s) & 1);
            int base = pixel ? 2 * s : 32 + 2 * s;
            const char *type = shadow ? "texture_depth_2d" : dim == DIM_CUBE ? "texture_cube<f32>"
                             : dim == DIM_VOLUME ? "texture_3d<f32>" : "texture_2d<f32>";
            o << "@group(1) @binding(" << base << ") var " << (pixel ? "tex" : "vs_tex") << s << ": " << type << ";\n";
            o << "@group(1) @binding(" << base + 1 << ") var " << (pixel ? "smp" : "vs_smp") << s << ": "
              << (shadow ? "sampler_comparison" : "sampler") << ";\n";
        }
        for (auto &d : defs) {
            char v[160];
            snprintf(v, sizeof v, "vec4f(%.9g, %.9g, %.9g, %.9g)", d.second[0], d.second[1], d.second[2], d.second[3]);
            o << "const c" << d.first << "_def = " << v << ";\n";
        }
        for (auto &d : idefs)
            o << "const i" << d.first << "_def = vec4i(" << d.second[0] << ", " << d.second[1] << ", "
              << d.second[2] << ", " << d.second[3] << ");\n";
        // D3D9 relative reads see def constants: Nuketown's sprinkler drops pick their quad corners in c56..c59 by a0.
        if (usesRelConst) {
            o << "fn " << ConstArray() << "_rel(i: i32) -> vec4f {\n";
            for (auto &d : defs) o << "    if (i == " << d.first << ") { return c" << d.first << "_def; }\n";
            o << "    return " << ConstArray() << ".c[clamp(i, 0, 255)];\n}\n";
        }
        o << "\n";

        if (!pixel) {
            o << "struct VertexIn {\n";
            bool any = false;
            for (auto &in : inputs) {
                int loc = SM3AttribLocation(in.second.first, in.second.second);
                if (loc < 0) { Fail("vertex input with an unsupported semantic"); continue; }
                if (!(opt.attribMask & (1u << loc))) continue;
                const char *type = (opt.attribUintMask & (1u << loc)) ? "vec4u" : (opt.attribSintMask & (1u << loc)) ? "vec4i" : "vec4f";
                o << "    @location(" << loc << ") v" << in.first << ": " << type << ",\n";
                any = true;
            }
            if (!any) o << "    @builtin(vertex_index) unused: u32,\n";
            o << "}\n";
            o << "struct VertexOut {\n    @builtin(position) pos: vec4f,\n";
            int positionReg = -1;
            for (auto &out : outputs) {
                int usage = out.second.first;
                if (usage == USAGE_POSITION || usage == USAGE_POSITIONT) { positionReg = out.first; continue; }
                int loc = VaryingLocation(usage, out.second.second);
                if (loc < 0) { Fail("vertex output with an unsupported semantic"); continue; }
                res.varyingMask |= 1u << loc;
                o << "    @location(" << loc << ") o" << out.first << ": vec4f,\n";
            }
            o << "}\n\n@vertex\nfn main(vin: VertexIn) -> VertexOut {\n";
            for (int r : temps) o << "    var r" << r << " = vec4f(0.0);\n";
            if (usesA0) o << "    var a0 = vec4i(0);\n";
            for (auto &out : outputs) o << "    var o" << out.first << " = vec4f(0.0);\n";
            o << body.str();
            o << "    var result: VertexOut;\n";
            if (positionReg >= 0)
                o << "    result.pos = vec4f(o" << positionReg << ".xy + kb.pixelShift * o" << positionReg << ".w, o"
                  << positionReg << ".zw);\n";
            for (auto &out : outputs)
                if (out.first != positionReg) o << "    result.o" << out.first << " = o" << out.first << ";\n";
            o << "    return result;\n}\n";
        } else {
            o << "struct PixelIn {\n";
            if (usesPos) o << "    @builtin(position) fragPos: vec4f,\n";
            if (usesFace) o << "    @builtin(front_facing) face: bool,\n";
            bool any = usesPos || usesFace;
            for (auto &in : inputs) {
                int loc = VaryingLocation(in.second.first, in.second.second);
                if (loc < 0 || !(opt.varyingMask & (1u << loc))) continue;
                o << "    @location(" << loc << ") v" << in.first << ": vec4f,\n";
                any = true;
            }
            if (!any) o << "    @builtin(position) fragPos: vec4f,\n";
            o << "}\nstruct PixelOut {\n";
            for (int c : colorOuts) o << "    @location(" << c << ") c" << c << ": vec4f,\n";
            if (usesDepth) o << "    @builtin(frag_depth) depth: f32,\n";
            if (colorOuts.empty() && !usesDepth) o << "    @location(0) c0: vec4f,\n";
            for (int c : colorOuts) res.colorOutMask |= 1u << c;
            if (colorOuts.empty() && !usesDepth) res.colorOutMask = 1;
            o << "}\n\n@fragment\nfn main(vin: PixelIn) -> PixelOut {\n";
            for (int r : temps) o << "    var r" << r << " = vec4f(0.0);\n";
            if (usesA0) o << "    var a0 = vec4i(0);\n";
            for (int c : colorOuts) o << "    var oC" << c << " = vec4f(0.0);\n";
            if (usesDepth) o << "    var oDepth = vec4f(0.0);\n";
            o << body.str();
            std::string color = colorOuts.count(0) ? "oC0" : "vec4f(0.0)";
            o << "    if (kb.alphaFunc != 0) {\n"
              << "        let a = " << color << ".a;\n"
              << "        let f = kb.alphaFunc;\n"
              << "        let passed = f == 8 || (f == 2 && a < kb.alphaRef) || (f == 3 && a == kb.alphaRef) ||\n"
              << "                   (f == 4 && a <= kb.alphaRef) || (f == 5 && a > kb.alphaRef) ||\n"
              << "                   (f == 6 && a != kb.alphaRef) || (f == 7 && a >= kb.alphaRef);\n"
              << "        if (!passed) { discard; }\n"
              << "    }\n";
            o << "    var result: PixelOut;\n";
            for (int c : colorOuts)
                o << "    result.c" << c << " = " << ((opt.saturateMask >> c & 1) ? "saturate(oC" : "(oC") << c << ");\n";
            if (usesDepth) o << "    result.depth = oDepth.x;\n";
            o << "    return result;\n}\n";
        }
        return o.str();
    }
};

} // namespace

int SM3AttribLocation(int usage, int index)
{
    switch (usage) {
    case USAGE_POSITION: case USAGE_POSITIONT: return 0;
    case USAGE_BLENDWEIGHT: return 1;
    case USAGE_BLENDINDICES: return 2;
    case USAGE_NORMAL: return 3;
    case USAGE_TANGENT: return 4;
    case USAGE_COLOR: return index <= 1 ? 5 + index : -1;
    case USAGE_TEXCOORD: return index <= 8 ? 7 + index : -1;
    default: return -1;
    }
}

Sm3WgslResult TranslateSm3ToWgsl(const uint32_t *tokens, const Sm3WgslOptions &options)
{
    Translator tr;
    tr.opt = options;
    uint32_t version = tokens[0];
    tr.pixel = (version >> 16) == 0xFFFF;
    tr.res.pixel = tr.pixel;
    if ((version & 0xFFFF) != 0x0300) {
        tr.res.error = "not a shader model 3 shader";
        return tr.res;
    }
    tr.Parse(tokens + 1);
    tr.res.wgsl = tr.Assemble();
    tr.res.ok = tr.res.error.empty();
    return tr.res;
}
