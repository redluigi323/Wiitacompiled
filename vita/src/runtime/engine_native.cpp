#include "mkwv/runtime/engine_native.hpp"
#include "mkwv/runtime/guest_access.hpp"

#include <atomic>
#include <cmath>
#include <cstring>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define ENGINE_NEON 1
#endif

namespace mkwv::runtime {
namespace {
bool enabled = true;
std::atomic<std::uint32_t> calls{0}, fallback{0};

float read_float(const std::uint8_t* p) {
    const std::uint32_t bits = guest_load_be32(p);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}
void load(const std::uint8_t* p, float* out, unsigned count) {
    unsigned i = 0;
#if ENGINE_NEON
    for (; i + 4 <= count; i += 4) {
        vst1q_f32(out + i, vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(p + i * 4))));
    }
#endif
    for (; i < count; ++i) out[i] = read_float(p + i * 4);
}
void store(std::uint8_t* p, const float* in, unsigned count) {
    unsigned i = 0;
#if ENGINE_NEON
    for (; i + 4 <= count; i += 4) {
        vst1q_u8(p + i * 4, vrev32q_u8(vreinterpretq_u8_f32(vld1q_f32(in + i))));
    }
#endif
    for (; i < count; ++i) {
        std::uint32_t bits;
        std::memcpy(&bits, in + i, 4);
        guest_store_be32(p + i * 4, bits);
    }
}

void concat(const float* a, const float* b, float* out) {
#if ENGINE_NEON
    const float32x4_t b0 = vld1q_f32(b), b1 = vld1q_f32(b + 4), b2 = vld1q_f32(b + 8);
    for (unsigned row = 0; row < 3; ++row) {
        const float* r = a + row * 4;
        float32x4_t v = vmulq_n_f32(b0, r[0]);
        v = vmlaq_n_f32(v, b1, r[1]);
        v = vmlaq_n_f32(v, b2, r[2]);
        v = vsetq_lane_f32(vgetq_lane_f32(v, 3) + r[3], v, 3);
        vst1q_f32(out + row * 4, v);
    }
#else
    for (unsigned row = 0; row < 3; ++row) {
        for (unsigned col = 0; col < 4; ++col) {
            out[row * 4 + col] = a[row * 4] * b[col] + a[row * 4 + 1] * b[4 + col] + a[row * 4 + 2] * b[8 + col];
        }
        out[row * 4 + 3] += a[row * 4 + 3];
    }
#endif
}
bool inverse(const float* a, float* out, bool transpose) {
    float co[9] = {
        a[5]*a[10]-a[6]*a[9], a[6]*a[8]-a[4]*a[10], a[4]*a[9]-a[5]*a[8],
        a[2]*a[9]-a[1]*a[10], a[0]*a[10]-a[2]*a[8], a[1]*a[8]-a[0]*a[9],
        a[1]*a[6]-a[2]*a[5], a[2]*a[4]-a[0]*a[6], a[0]*a[5]-a[1]*a[4]};
    const float det = a[0]*co[0]+a[1]*co[1]+a[2]*co[2];
    // Keep ill-conditioned/exceptional matrices on the exact PPC path.
    if (!std::isfinite(det) || std::fabs(det) < 1.0e-8f) return false;
    const float recip = 1.0f / det;
    for (unsigned r = 0; r < 3; ++r) {
        for (unsigned c = 0; c < 3; ++c) out[r*4+c] = co[transpose ? r*3+c : c*3+r] * recip;
        out[r*4+3] = transpose ? 0.0f : -(out[r*4]*a[3]+out[r*4+1]*a[7]+out[r*4+2]*a[11]);
    }
    return true;
}

// NW4R F96, I48 and I32 channels share the same Hermite interpolation.
// Decode the whole channel through one RAM binding instead of PPC quantized loads.
bool keyframe(std::uint32_t address, CpuContext* cpu) {
    const bool i48 = address == 0x800540F0;
    const bool i32 = address == 0x80054270;
    if (cpu->gqr[3] != 0x00050005u) return false;
    if (i48 && (cpu->gqr[6] != 0x05070507u || cpu->gqr[7] != 0x08070807u)) return false;
    if (i32 && (cpu->gqr[2] != 0x00040004u || cpu->gqr[6] != 0x05070507u)) return false;
    const unsigned header_size = i48 || i32 ? 16 : 8;
    const unsigned stride = i48 ? 6 : i32 ? 4 : 12;
    const std::uint32_t base = cpu->gpr[3];
    const auto* header = guest_fast_ptr(base, header_size);
    const float frame = static_cast<float>(cpu->fpr[1].d);
    if (!header || !std::isfinite(frame)) return false;
    const unsigned count = (unsigned(header[0]) << 8) | header[1];
    if (count == 0 || count > 4096) return false;
    const auto* data = guest_fast_ptr(base, header_size + count * stride);
    if (!data) return false;
    const auto* first = data + header_size;
    const float scale = i48 || i32 ? read_float(header + 8) : 1.0f;
    const float offset = i48 || i32 ? read_float(header + 12) : 0.0f;
    auto half = [](const std::uint8_t* p) { return (unsigned(p[0]) << 8) | p[1]; };
    auto time = [&](unsigned index) {
        const auto* p = first + index * stride;
        return i48 ? float(static_cast<std::int16_t>(half(p))) / 32.0f : i32 ? float(p[0]) : read_float(p);
    };
    auto value = [&](unsigned index) {
        const auto* p = first + index * stride;
        return i48 ? std::fma(scale, float(half(p + 2)), offset) :
               i32 ? std::fma(scale, float((guest_load_be32(p) >> 12) & 4095), offset) : read_float(p + 4);
    };
    auto slope = [&](unsigned index) {
        const auto* p = first + index * stride;
        if (i48) return float(static_cast<std::int16_t>(half(p + 4))) / 256.0f;
        if (i32) {
            const unsigned raw = guest_load_be32(p) & 4095;
            return float(int(raw ^ 2048u) - 2048) / 32.0f;
        }
        return read_float(p + 8);
    };
    const float begin = time(0), end = time(count - 1);
    if (!std::isfinite(begin) || !std::isfinite(end) || begin > end) return false;
    float result;
    if (frame <= begin) result = value(0);
    else if (frame >= end) result = value(count - 1);
    else {
        unsigned lo = 0, hi = count - 1;
        while (lo + 1 < hi) {
            const unsigned mid = (lo + hi) / 2;
            const float at = time(mid);
            if (!std::isfinite(at)) return false;
            if (at <= frame) lo = mid; else hi = mid;
        }
        const float left_time = time(lo), left_value = value(lo);
        const float span = time(hi) - left_time;
        if (!(span > 0.0f)) return false;
        const float dt = frame - left_time, t = dt / span;
        const float delta = left_value - value(hi), t1 = t - 1.0f;
        const float curve = t * (t * ((2.0f*t - 3.0f) * delta));
        const float tangent = (dt*t1) * (t1*slope(lo) + t*slope(hi));
        result = (left_value + curve) + tangent;
    }
    cpu->fpr[1].d = result;
    return true;
}

bool table_sincos(float angle, CpuContext* cpu, float& sine, float& cosine) {
    if (!std::isfinite(angle) || std::fabs(angle) > 65536.0f) return false;
    const auto* period = guest_fast_ptr(cpu->gpr[2] - 28960u, 4);
    if (!period || read_float(period) != 65536.0f || cpu->gqr[3] != 0x00050005u) return false;
    const float a = std::fabs(angle);
    // NW4R saturates to u16 before masking the table index to eight bits.
    const unsigned index = a >= 65536.0f ? 65535u : static_cast<unsigned>(a);
    const float frac = a - static_cast<float>(index);
    const auto* row = guest_fast_ptr(0x80248010u + (index & 255u) * 16, 16);
    if (!row) return false;
    float v[4]; load(row, v, 4);
    sine = v[0] + frac*v[2]; cosine = v[1] + frac*v[3];
    if (angle < 0.0f) sine = -sine;
    return true;
}

bool trig(std::uint32_t address, CpuContext* cpu) {
    float sine, cosine;
    if (!table_sincos(static_cast<float>(cpu->fpr[1].d), cpu, sine, cosine)) return false;
    if (address == 0x800851E0) {
        auto* s = guest_fast_ptr(cpu->gpr[3], 4);
        auto* c = guest_fast_ptr(cpu->gpr[4], 4);
        if (!s || !c) return false;
        store(c, &cosine, 1); store(s, &sine, 1);
    } else cpu->fpr[1].d = address == 0x80085110 ? sine : cosine;
    return true;
}

bool run(std::uint32_t address, CpuContext* cpu) {
    if (address == 0x80052FD0 || address == 0x80053FB0 || address == 0x800540F0 || address == 0x80054270) return keyframe(address, cpu);
    if (cpu->gqr[0] != 0) return false;  // unusual paired-single formats retain PPC semantics
    if (address == 0x80085110 || address == 0x80085180 || address == 0x800851E0) return trig(address, cpu);
    auto ptr = [](std::uint32_t a, unsigned n) { return guest_fast_ptr(a, n * 4); };
    auto get = [&](std::uint32_t a, float* out, unsigned n) {
        const auto* p = ptr(a,n); if (!p) return false; load(p,out,n); return true;
    };
    float a[12], b[12], out[12]{};
    const auto r3 = cpu->gpr[3], r4 = cpu->gpr[4], r5 = cpu->gpr[5];
    const float x = static_cast<float>(cpu->fpr[1].d), y = static_cast<float>(cpu->fpr[2].d), z = static_cast<float>(cpu->fpr[3].d);
    std::uint32_t dest = r3;
    unsigned size = 12;
    switch (address) {
    case 0x80085940: {
        float sx, cx, sy, cy, sz, cz;
        if (!table_sincos(x,cpu,sx,cx) || !table_sincos(y,cpu,sy,cy) || !table_sincos(z,cpu,sz,cz)) return false;
        out[0]=cy*cz; out[1]=sx*sy*cz-cx*sz; out[2]=cx*sy*cz+sx*sz;
        out[4]=cy*sz; out[5]=sx*sy*sz+cx*cz; out[6]=cx*sy*sz-sx*cz;
        out[8]=-sy; out[9]=sx*cy; out[10]=cx*cy;
        break;
    }
    case 0x80199D04: out[0]=out[5]=out[10]=1; break;
    case 0x80085600: size=9; out[0]=out[4]=out[8]=1; break;
    case 0x80085740: break;
    case 0x80199D30:
        dest=r4; if (!get(r3,out,12)) return false; break;
    case 0x80085630:
        size=9; if (!get(r4,a,12)) return false;
        for (unsigned row=0;row<3;++row) for(unsigned col=0;col<3;++col) out[row*3+col]=a[row*4+col];
        break;
    case 0x80199D64: case 0x80041A90:
        if (!get(address==0x80199D64?r3:r4,a,12) || !get(address==0x80199D64?r4:r5,b,12)) return false;
        dest=address==0x80199D64?r5:r3; concat(a,b,out); break;
    case 0x80199E30: {
        const unsigned n=cpu->gpr[6];
        if (n==0) return false;  // original do/while behavior is deliberately retained
        if (n>4096 || !get(r3,a,12)) return false;
        auto* src=ptr(r4,n*12); auto* dst=ptr(r5,n*12);
        if (!src || !dst) return false;
        // Arbitrary partial overlap can modify future inputs: defer it to the original.
        const std::uint64_t bytes=std::uint64_t(n)*48;
        if (r4!=r5 && std::uint64_t(r4)<std::uint64_t(r5)+bytes && std::uint64_t(r5)<std::uint64_t(r4)+bytes) return false;
        for(unsigned i=0;i<n;++i) { load(src+i*48,b,12); concat(a,b,out); store(dst+i*48,out,12); }
        cpu->gpr[4]=r4+n*48; cpu->gpr[5]=r5+n*48;
        return true;
    }
    case 0x80199FC8: case 0x8019A0C0:
        dest=r4; if (!get(r3,a,12) || !inverse(a,out,address==0x8019A0C0)) return false;
        // Resolve output before publishing the success return register.
        if (!ptr(dest,12)) return false;
        cpu->gpr[3]=1; break;
    case 0x8019A3E0: out[0]=out[5]=out[10]=1;out[3]=x;out[7]=y;out[11]=z;break;
    case 0x8019A460: out[0]=x;out[5]=y;out[10]=z;break;
    case 0x8019A414: case 0x8019A488:
        dest=r4; if(!get(r3,a,12))return false;
        for(unsigned row=0;row<3;++row)for(unsigned col=0;col<4;++col) out[row*4+col]=a[row*4+col]*(address==0x8019A488?(row==0?x:row==1?y:z):1.0f);
        if(address==0x8019A414){out[3]+=x;out[7]+=y;out[11]+=z;}break;
    case 0x80085760:
        if(!get(r4,a,12))return false;
        for(unsigned i=0;i<12;++i)out[i]=a[i]*x;break;
    case 0x800857B0: case 0x80085810:
        if(!get(r4,a,12)||!get(r5,b,3))return false;
        for(unsigned row=0;row<3;++row)for(unsigned col=0;col<4;++col) out[row*4+col]=a[row*4+col]*(address==0x800857B0&&col<3?b[col]:1.0f);
        if(address==0x80085810)for(unsigned row=0;row<3;++row)out[row*4+3]+=((a[row*4]*b[0]+a[row*4+1]*b[1])+a[row*4+2]*b[2]);
        break;
    case 0x8019A91C: case 0x8019A970: case 0x80040820: case 0x80085AB0: {
        const bool sdk=address==0x8019A91C||address==0x8019A970;
        if(!get(sdk?r3:r4,a,12)||!get(sdk?r4:r5,b,3))return false;
        dest=sdk?r5:r3;size=3;
        for(unsigned row=0;row<3;++row)out[row]=(a[row*4]*b[0]+a[row*4+1]*b[1])+a[row*4+2]*b[2];
        if(address==0x8019A91C||address==0x80040820)for(unsigned row=0;row<3;++row)out[row]+=a[row*4+3];
        break;
    }
    case 0x8019ABE4: case 0x8019ACCC: case 0x80042370: {
        const bool sdk=address!=0x80042370;
        if(!get(sdk?r3:r4,a,3)||!get(sdk?r4:r5,b,3))return false;
        dest=sdk?r5:r3;size=3;
        if(address==0x8019ABE4)for(unsigned i=0;i<3;++i)out[i]=a[i]+b[i];
        else {out[0]=a[1]*b[2]-a[2]*b[1];out[1]=a[2]*b[0]-a[0]*b[2];out[2]=a[0]*b[1]-a[1]*b[0];}break;
    }
    case 0x8019AC08: case 0x8019AC24:
        if(!get(r3,a,3))return false;dest=r4;size=3;
        { const float len=(a[0]*a[0]+a[1]*a[1])+a[2]*a[2];
          if(address==0x8019AC24 && (!(len>0)||!std::isfinite(len)))return false;
          const float scale=address==0x8019AC24?1.0f/std::sqrt(len):x;
          for(unsigned i=0;i<3;++i)out[i]=a[i]*scale; }break;
    case 0x8019AC68: case 0x8019ACAC: case 0x8019ADE0: case 0x80041B70:
        if(!get(r3,a,3))return false;
        if(address!=0x8019AC68 && !get(r4,b,3))return false;
        if(address==0x8019ADE0)for(unsigned i=0;i<3;++i){a[i]-=b[i];b[i]=a[i];}
        if(address==0x8019AC68)std::memcpy(b,a,12);
        out[0]=(a[0]*b[0]+a[1]*b[1])+a[2]*b[2];
        if(address==0x8019AC68)cpu->fpr[1].d=std::sqrt(out[0]);
        else {
            // These PPC routines return a paired-single register, not an IEEE double.
            cpu->fpr[1].paired.ps0=out[0];
            cpu->fpr[1].paired.ps1=a[2]*b[2];
        }
        return true;
    default:return false;
    }
    auto* dst=ptr(dest,size);if(!dst)return false;
    store(dst,out,size);
    // NW4R pointer-returning convenience functions return their destination.
    if(address==0x80040820||address==0x80041A90||address==0x80042370)cpu->gpr[3]=dest;
    return true;
}
}

void set_native_engine(bool on) { enabled=on; }
bool native_engine_enabled() { return enabled; }
bool try_native_engine(std::uint32_t address, CpuContext* cpu) {
    if(!enabled || !cpu) return false;
    return run(address,cpu);
}
NativeEngineStats take_native_engine_stats() {
    return {calls.exchange(0,std::memory_order_relaxed),fallback.exchange(0,std::memory_order_relaxed)};
}
void note_engine_call(bool native) {
    (native?calls:fallback).fetch_add(1,std::memory_order_relaxed);
}
}

#if defined(__vita__)
// Defining-shard copies rename only the PPC definitions. Direct calls, indirect table
// entries, and same-shard calls therefore all point at these wrappers.
#define MKWV_ENGINE_ENTRY(addr) \
extern "C" void mkwv_ppc_##addr(CpuContext*); \
extern "C" void func_##addr(CpuContext* cpu) { \
    const bool native=mkwv::runtime::try_native_engine(0x##addr##u,cpu); \
    mkwv::runtime::note_engine_call(native); \
    if(!native)mkwv_ppc_##addr(cpu); \
}
#include "mkwv/runtime/engine_native_catalog.inc"
#undef MKWV_ENGINE_ENTRY
#endif
