// GPU lighting (static63): the vertex path without lighting plus the vertex shader's lighting
// (gpu_lit_reference, the C++ form of emit_tev_vertex_cg_lit) must give the colours the CPU
// lighting gives, to within a step of rounding. Also sanity-checks the emitted Cg.
#include "mkwv/gx/cg_emit.hpp"
#include "mkwv/gx/decode.hpp"
#include "mkwv/gx/draw_state.hpp"
#include "mkwv/gx/vertex_worker.hpp"
#include "mkwv/gx/vertex_tuning.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace mkwv::gx;

namespace {

int byte_diff(std::uint32_t a, std::uint32_t b) {
    int worst = 0;
    for (unsigned k = 0; k < 4u; ++k) {
        const int d = std::abs(static_cast<int>((a >> (8u * k)) & 0xFFu) - static_cast<int>((b >> (8u * k)) & 0xFFu));
        worst = d > worst ? d : worst;
    }
    return worst;
}

bool test_equation() {
    State state;
    unsigned rng = 1234;
    auto next = [&]() { rng = rng * 1664525u + 1013904223u; return rng; };
    auto scalar = [&]() { return (static_cast<int>(next() % 2001) - 1000) / 113.0f; };
    unsigned checked = 0, off_by_one = 0, bigger = 0;
    for (unsigned trial = 0; trial < 6000; ++trial) {
        state.reset();
        state.num_chans = 1 + trial % 2;
        state.vtx_desc[AttrClr0] = trial & 1 ? AttrDirect : AttrNone;
        state.vtx_desc[AttrClr1] = trial & 2 ? AttrDirect : AttrNone;
        for (auto& ctrl : state.chans) {
            ctrl.lighting = next() % 5 != 0;
            ctrl.light_mask = next() >> 24;
            ctrl.mat_src = next() & 256 ? SrcReg : SrcVtx;
            ctrl.amb_src = next() & 256 ? SrcReg : SrcVtx;
            ctrl.diff_fn = static_cast<DiffuseFn>(next() % 3);
            ctrl.attn_fn = static_cast<AttnFn>(next() % 3);
        }
        for (auto& light : state.lights) {
            light.color = next();
            for (unsigned c = 0; c < 3; ++c) {
                light.pos[c] = scalar();
                light.dir[c] = scalar();
                light.cos_att[c] = scalar();
                light.dist_att[c] = 1.0f + static_cast<float>(next() % 100) / 100.0f;
            }
        }
        for (unsigned c = 0; c < 2; ++c) {
            state.chan_mat[c] = next();
            state.ambient[c] = next();
        }
        DecodedVertex raw{};
        raw.x = scalar(); raw.y = scalar(); raw.z = scalar();
        raw.nx = scalar(); raw.ny = scalar(); raw.nz = scalar();
        raw.color = next(); raw.color1 = next();
        raw.pn_mtx = trial % kMaxPnMtx;
        for (auto& c : state.pn_mtx[raw.pn_mtx]) c = scalar();
        for (auto& c : state.nrm_mtx[raw.pn_mtx]) c = scalar();
        PreparedLighting prepared;
        prepare_lighting(state, prepared);
        if (!gpu_lit_eligible(state, prepared)) {
            continue;
        }
        GpuLitParams params;
        fill_gpu_lit_params(state, prepared, params);
        DecodedVertex cpu = raw, gpu = raw;
        light_and_transform(state, prepared, cpu);
        const float su[kMaxTexGens][2] = {};
        process_vertex_gpu_lit(state, su, false, gpu);
        if (gpu.x != cpu.x || gpu.y != cpu.y || gpu.z != cpu.z || gpu.w != cpu.w) {
            std::fprintf(stderr, "gpu_lit: clip position differs at %u\n", trial);
            return false;
        }
        const float eye[3] = {gpu.bx, gpu.by, gpu.bz};
        const float nrm[3] = {gpu.tx, gpu.ty, gpu.tz};
        std::uint32_t c0 = 0, c1 = 0;
        gpu_lit_reference(params, eye, nrm, gpu.color, gpu.color1, &c0, &c1);
        const int d = std::max(byte_diff(c0, cpu.color), byte_diff(c1, cpu.color1));
        ++checked;
        if (d == 1) {
            ++off_by_one;
        } else if (d > 1) {
            ++bigger;
            if (bigger <= 5) {
                std::fprintf(stderr, "gpu_lit: trial %u colours %08X/%08X vs CPU %08X/%08X\n", trial, c0, c1, cpu.color,
                             cpu.color1);
            }
        }
    }
    std::printf("gpu_lit: %u vertices, %u off by one, %u further\n", checked, off_by_one, bigger);
    // Float against the CPU's float: identical but for the odd rounding step.
    return checked > 1000 && bigger <= checked / 200u;
}

bool test_emit() {
    GpuLitParams lit{};
    lit.on = 1;
    lit.lit[0] = 1;
    lit.lit[2] = 1;
    lit.mat_vtx[0] = 1;
    lit.diff_fn[0] = DfClamp;
    lit.attn_fn[0] = AfSpot;
    lit.light_mask[0] = 0x05;
    lit.diff_fn[2] = DfSign;
    lit.attn_fn[2] = AfSpec;
    lit.light_mask[2] = 0x02;
    ShaderNeeds needs;
    needs.color0 = true;
    needs.coord_used[0] = true;
    static char buf[32768];
    std::size_t n = 0;
    if (!emit_tev_vertex_cg_lit(needs, lit, buf, sizeof(buf), &n) || n == 0) {
        std::fprintf(stderr, "gpu_lit: emitter failed\n");
        return false;
    }
    const char* want[] = {"aEyePos", "aEyeNrm", "uLP0", "uLP2", "uLC1", "uMat0", "vColor0 = float4(o0, a0) / 255.0",
                          "rsqrt", "sign(t) * floor(abs(t) + 0.5)"};
    for (const char* w : want) {
        if (std::strstr(buf, w) == nullptr) {
            std::fprintf(stderr, "gpu_lit: emitted program lacks '%s'\n%s\n", w, buf);
            return false;
        }
    }
    if (std::strstr(buf, "uLP3") != nullptr || std::strstr(buf, "vColor1") != nullptr) {
        std::fprintf(stderr, "gpu_lit: emitted program has unused parts\n%s\n", buf);
        return false;
    }
    // Balanced braces and parentheses.
    int br = 0, pa = 0;
    for (const char* p = buf; *p; ++p) {
        br += *p == '{' ? 1 : (*p == '}' ? -1 : 0);
        pa += *p == '(' ? 1 : (*p == ')' ? -1 : 0);
    }
    if (br != 0 || pa != 0) {
        std::fprintf(stderr, "gpu_lit: unbalanced program\n%s\n", buf);
        return false;
    }
    if (std::getenv("MKWV_PRINT_LIT_VS")) {
        std::printf("%s\n", buf);
    }
    return true;
}

std::mutex g_m;
std::condition_variable g_cv;
int g_sem = 0;
void sem_wait() { std::unique_lock<std::mutex> l(g_m); g_cv.wait(l, [] { return g_sem > 0; }); --g_sem; }
void sem_post() { { std::lock_guard<std::mutex> l(g_m); ++g_sem; } g_cv.notify_all(); }
void relax() { std::this_thread::yield(); }

// Position, normal and colour indexed, no texgen: the vertex worker takes its combined-matrix
// path for GPU-lit vertices. It must pack what the inline (general) GPU-lit path packs; the clip
// position may differ by rounding (the matrices are folded), everything else is exact.
bool test_fast_path() {
    static float pos[16][3], nrm[16][3];
    static std::uint32_t col[16];
    for (unsigned i = 0; i < 16; ++i) {
        pos[i][0] = i * 0.37f - 2.0f; pos[i][1] = (i % 5) * -0.8f; pos[i][2] = -3.0f - i * 0.1f;
        nrm[i][0] = (i % 3) - 1.0f; nrm[i][1] = 0.5f; nrm[i][2] = (i % 2) ? 1.0f : -0.25f;
        col[i] = 0x80402010u + i * 0x01030507u;
    }
    auto f = std::make_unique<Fifo>();
    configure_pos_index16_nrm_index8_clr0_index8(*f, VtxFmt0);
    f->set_array(AttrPos, pos, sizeof(pos), 12, true);
    f->set_array(AttrNrm, nrm, sizeof(nrm), 12, true);
    f->set_array(AttrClr0, col, sizeof(col), 4, true);
    f->set_gen_mode(0, 1, 1);
    const float m[12] = {0.9f, 0.1f, 0.0f, 1.0f, -0.1f, 0.9f, 0.2f, 2.0f, 0.0f, -0.2f, 1.0f, -5.0f};
    f->load_xf_f32(0, m, 12);
    f->set_chan_mat_color(0, 200, 150, 100, 255);
    f->set_chan_amb_color(0, 20, 30, 40, 255);
    f->set_chan_ctrl(0, true, SrcReg, SrcVtx, 0x03, DfClamp, AfSpot);
    f->load_light(0, 200, 180, 160, 255, 1.0f, 4.0f, 3.0f);
    f->load_light(1, 90, 90, 255, 255, -3.0f, 2.0f, 1.0f);
    for (unsigned i = 0; i < 40; ++i) {
        std::uint8_t packet[8 * 4];
        for (unsigned v = 0; v < 8; ++v) {
            const unsigned idx = (v * 3 + i) % 16;
            packet[v * 4 + 0] = 0;
            packet[v * 4 + 1] = static_cast<std::uint8_t>(idx);
            packet[v * 4 + 2] = static_cast<std::uint8_t>(idx);
            packet[v * 4 + 3] = static_cast<std::uint8_t>(idx);
        }
        f->draw(TriangleStrip, VtxFmt0, 8, packet, sizeof(packet));
    }
    auto collect = [&](const Processor& p, std::vector<std::uint8_t>& out, unsigned* lit) {
        PackedBatch pb{};
        if (!packed_batch(p, &pb)) return false;
        for (unsigned d = 0; d < p.draw_count; ++d) {
            const DrawCall& dc = p.draws[d];
            const bool gl = draw_gpu_lit(p, d);
            *lit += gl ? 1u : 0u;
            for (unsigned k = 0; k < dc.vertex_count; ++k) {
                const unsigned v = dc.first_vertex + k;
                const unsigned src = pb.remap[v] >= 0 ? static_cast<unsigned>(pb.remap[v]) : v;
                const std::uint8_t* got = pb.arena + pb.offset[src];
                out.insert(out.end(), got, got + pack_stride(pack_ntc(dc.num_tex_gens), gl));
            }
        }
        return true;
    };
    set_vertex_reuse(false);
    auto a = std::make_unique<Processor>();
    a->reset();
    if (!a->process(f->data(), f->size(), true)) return false;
    std::vector<std::uint8_t> inline_bytes, worker_bytes;
    unsigned lit_a = 0, lit_b = 0;
    if (!collect(*a, inline_bytes, &lit_a)) return false;
    std::thread w([] { vertex_worker_main(); });
    vertex_worker_install(VertexWorkerHooks{&sem_wait, &sem_post, &relax});
    auto b = std::make_unique<Processor>();
    b->reset();
    const bool ok = b->process(f->data(), f->size(), true);
    vertex_worker_drain();
    const bool got = ok && collect(*b, worker_bytes, &lit_b);
    vertex_worker_uninstall();
    vertex_worker_quit();
    sem_post();
    w.join();
    set_vertex_reuse(true);
    if (!got || lit_a == 0 || lit_a != lit_b || inline_bytes.size() != worker_bytes.size()) {
        std::fprintf(stderr, "gpu_lit fast: packing differs (lit %u/%u, %zu/%zu bytes)\n", lit_a, lit_b,
                     inline_bytes.size(), worker_bytes.size());
        return false;
    }
    const unsigned stride = pack_stride(1, true);
    for (std::size_t off = 0; off + stride <= inline_bytes.size(); off += stride) {
        float pa[4], pbv[4];
        std::memcpy(pa, &inline_bytes[off], 16);
        std::memcpy(pbv, &worker_bytes[off], 16);
        for (unsigned k = 0; k < 4; ++k) {
            if (std::abs(pa[k] - pbv[k]) > 1e-4f * (1.0f + std::abs(pa[k]))) {
                std::fprintf(stderr, "gpu_lit fast: clip differs at byte %zu\n", off);
                return false;
            }
        }
        // Colours exactly; eye position and normal to rounding (static64: the worker's NEON path
        // multiplies by matrix columns, the inline path takes row dot products).
        if (std::memcmp(&inline_bytes[off + 16], &worker_bytes[off + 16], 8) != 0) {
            std::fprintf(stderr, "gpu_lit fast: colours differ at byte %zu\n", off);
            return false;
        }
        for (unsigned k = 24; k < stride; k += 4) {
            float x, y;
            std::memcpy(&x, &inline_bytes[off + k], 4);
            std::memcpy(&y, &worker_bytes[off + k], 4);
            if (std::abs(x - y) > 1e-4f * (1.0f + std::abs(x))) {
                std::fprintf(stderr, "gpu_lit fast: texcoord/eye/normal differ at byte %zu+%u (%f vs %f)\n", off, k, x, y);
                return false;
            }
        }
    }
    std::printf("gpu_lit fast path: %u lit draws, %zu vertices match\n", lit_a, inline_bytes.size() / stride);
    return true;
}

// Compare the compact cache packing path with the full restored vertex. Include zero,
// one and two texgens, default and explicit matrix indices, GPU-lit and unlit draws.
bool test_compact_pack() {
    for (unsigned ngen = 0; ngen <= 2; ++ngen) {
        for (unsigned indexed = 0; indexed <= 1; ++indexed) {
            for (unsigned lit = 0; lit <= 1; ++lit) {
                State state;
                state.reset();
                state.num_tex_gens = ngen;
                state.vtx_desc[AttrPnMtxIdx] = indexed ? AttrDirect : AttrNone;
                state.vtx_desc[AttrPos] = AttrDirect;
                state.vtx_desc[AttrNrm] = AttrDirect;
                state.vtx_desc[AttrClr0] = AttrDirect;
                for (unsigned i = 0; i < ngen; ++i) {
                    state.vtx_desc[AttrTex0 + i] = AttrDirect;
                    state.texgens[i].type = TgMtx2x4;
                    state.texgens[i].src = static_cast<TexGenSrc>(TgTex0 + i);
                    state.texgens[i].mtx = kTexMtxIdentity;
                    state.texgens[i].normalize = false;
                }
                PreparedLighting lighting{};
                prepare_lighting(state, lighting);
                auto snap = std::make_unique<VertexSnapshot>();
                float su[kMaxTexGens][2];
                for (auto& row : su) { row[0] = 1; row[1] = 1; }
                capture_vertex_snapshot(state, lighting, su, false, *snap);
                snap->gpu_lit = lit;
                snap->decoder.prepare(state, VtxFmt0, true);
                CachedVertex cache[3]{};
                for (unsigned i = 0; i < 3; ++i) {
                    cache[i].x = i * 0.7f; cache[i].y = -1.5f; cache[i].z = -4.0f;
                    cache[i].nx = 0.3f; cache[i].ny = 0.5f; cache[i].nz = -0.8f;
                    cache[i].color = 0x87654321u + i;
                    cache[i].color1 = 0xABCDEF01u + i;
                    cache[i].tc[0][0] = i * 0.2f; cache[i].tc[0][1] = -0.8f;
                    cache[i].tc[1][0] = 0.9f; cache[i].tc[1][1] = i * 0.4f;
                    cache[i].pn_mtx = i;
                }
                const unsigned stride = pack_stride(pack_ntc(ngen), lit);
                std::uint32_t offsets[3] = {0, stride, stride * 2};
                std::int32_t remap[3] = {-1, -1, -1};
                std::vector<std::uint8_t> full(stride * 3), compact(stride * 3);
                DecodedVertex mirror[3];
                VertexJob job{};
                job.snap = snap.get(); job.cached = cache; job.count = 3;
                job.remap = remap; job.offset = offsets; job.verts = mirror;
                job.arena = full.data();
                if (process_vertex_job(job) != 0) return false;
                job.verts = nullptr; job.arena = compact.data();
                if (process_vertex_job(job) != 0 || full != compact) {
                    std::fprintf(stderr, "compact packing mismatch: texgens=%u indexed=%u lit=%u\n",
                                 ngen, indexed, lit);
                    return false;
                }
            }
        }
    }
    return true;
}

// Compare prepared worker texgens with the unchanged general evaluator. Cover
// reflection/normal channels, 3x4 projective output, normalization and cross-slot
// input: the trophy path could not enter the old two-coordinate fast path.
bool test_general_texgen_pack() {
    unsigned cases=0;
    for(unsigned trial=0;trial<240;++trial) {
        State state;state.reset();
        state.num_tex_gens=1+trial%8;state.dual_tex=(trial/8)%2;
        state.num_chans=1;state.chans[0].lighting=(trial%2)!=0;
        state.vtx_desc[AttrPos]=state.vtx_desc[AttrNrm]=state.vtx_desc[AttrClr0]=AttrDirect;
        for(unsigned i=0;i<2;++i)state.vtx_desc[AttrTex0+i]=AttrDirect;
        for(unsigned i=0;i<state.num_tex_gens;++i) {
            auto& g=state.texgens[i];
            g.type=static_cast<TexGenType>((trial+i)%3);
            const TexGenSrc sources[]={TgPos,TgNrm,TgBinrm,TgTangent,TgColor0,TgColor1,TgTex0,TgTex1,TgTex7};
            g.src=sources[(trial/3+i)%9];g.normalize=(trial/6+i)%2;
            g.form_ab11=(trial/12+i)%2;g.mtx=30+i*3;
            g.post_mtx=kPtTexMtx0+i*3;
            float* m=state.xf_mem+g.mtx*4;
            float* post=state.xf_dtt+i*12;
            for(unsigned k=0;k<12;++k){m[k]=float(int((trial+k*7)%13)-6)/5;post[k]=float(int((trial+k*3)%11)-5)/7;}
            if(trial%7==0){for(unsigned k=8;k<12;++k){m[k]=0;post[k]=0;}}
            if(trial%11==0)state.vtx_desc[AttrTex0MtxIdx+i]=AttrDirect;
        }
        PreparedLighting lighting{};prepare_lighting(state,lighting);
        float su[kMaxTexGens][2];for(unsigned i=0;i<kMaxTexGens;++i){su[i][0]=0.5f+i;su[i][1]=1.25f+i;}
        auto snap=std::make_unique<VertexSnapshot>();capture_vertex_snapshot(state,lighting,su,true,*snap);
        snap->gpu_lit=state.chans[0].lighting;
        std::memcpy(snap->nrm_mtx,state.nrm_mtx,sizeof(snap->nrm_mtx));
        snap->decoder.prepare(state,VtxFmt0,true);
        CachedVertex cached{};cached.x=1.25f;cached.y=-0.7f;cached.z=2.5f;
        cached.nx=0.25f;cached.ny=-0.5f;cached.nz=0.9f;
        cached.color=0x98765432;cached.color1=0x12345678;
        cached.tc[0][0]=0.3f;cached.tc[0][1]=-1.1f;cached.tc[1][0]=-0.4f;cached.tc[1][1]=0.7f;
        DecodedVertex original;snap->decoder.restore(cached,original);
        if(snap->gpu_lit)process_vertex_gpu_lit(state,su,true,original);
        else process_vertex(state,lighting,su,true,original);
        const unsigned stride=pack_stride(pack_ntc(state.num_tex_gens),snap->gpu_lit);
        std::vector<std::uint8_t> expected(stride),actual(stride);
        pack_vertex(original,pack_ntc(state.num_tex_gens),expected.data(),snap->gpu_lit);
        std::int32_t remap=-1;std::uint32_t offset=0;
        VertexJob job{};job.snap=snap.get();job.cached=&cached;job.count=1;
        job.remap=&remap;job.offset=&offset;job.arena=actual.data();
        if(process_vertex_job(job))return false;
        if(std::memcmp(expected.data()+16,actual.data()+16,8))return false;
        for(unsigned byte=0;byte<stride;byte+=4) {
            if(byte==16||byte==20)continue;
            float a,b;std::memcpy(&a,expected.data()+byte,4);std::memcpy(&b,actual.data()+byte,4);
            if(!(std::abs(a-b)<=1e-4f*(1+std::abs(a)))) {
                std::fprintf(stderr,"prepared texgen differs trial=%u byte=%u general=%g fast=%g\n",trial,byte,a,b);return false;
            }
        }
        set_prepared_texgen(false);
        std::fill(actual.begin(),actual.end(),0);
        if(process_vertex_job(job))return false;
        set_prepared_texgen(true);
        for(unsigned byte=0;byte<stride;byte+=4){
            if(byte==16||byte==20){if(std::memcmp(expected.data()+byte,actual.data()+byte,4))return false;continue;}
            float a,b;std::memcpy(&a,expected.data()+byte,4);std::memcpy(&b,actual.data()+byte,4);
            if(!(std::abs(a-b)<=1e-4f*(1+std::abs(a))))return false;
        }
        ++cases;
    }
    std::printf("prepared texgen: %u general-path differential cases passed\n",cases);
    return true;
}

} // namespace

int main() {
    if (!test_compact_pack() || !test_general_texgen_pack()) return 1;
    set_gpu_lighting(true);
    if (!test_equation() || !test_emit() || !test_fast_path()) {
        return 1;
    }
    std::printf("gpu_lit: ok\n");
    return 0;
}
