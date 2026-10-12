#include "mkwv/gx/decode.hpp"
#include "mkwv/gx/draw_state.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "mkwv/gx/vertex_worker.hpp"
using namespace mkwv::gx;

namespace {
std::mutex g_sem_m;
std::condition_variable g_sem_cv;
int g_sem = 0;
void sem_wait() { std::unique_lock<std::mutex> l(g_sem_m); g_sem_cv.wait(l, [] { return g_sem > 0; }); --g_sem; }
void sem_post() { { std::lock_guard<std::mutex> l(g_sem_m); ++g_sem; } g_sem_cv.notify_one(); }
void relax() { std::this_thread::yield(); }
} // namespace

// Worker-composed matrices may change float rounding (and the sign of zero).
// Cache/reuse colours and layouts remain exact; geometry uses the same tolerance
// as the dedicated worker/general-path differential tests.
static bool near_float(float a,float b){return !std::memcmp(&a,&b,4)||std::fabs(a-b)<=1e-4f*(1+std::fabs(a));}
static bool near_floats(const void* a,const void* b,unsigned bytes){
    for(unsigned k=0;k<bytes;k+=4){float x,y;std::memcpy(&x,static_cast<const char*>(a)+k,4);std::memcpy(&y,static_cast<const char*>(b)+k,4);if(!near_float(x,y))return false;}return true;
}
static bool near_packed(const std::uint8_t* a,const std::uint8_t* b,unsigned bytes){
    return near_floats(a,b,16)&&!std::memcmp(a+16,b+16,8)&&near_floats(a+24,b+24,bytes-24);
}

static bool test_prepared_lighting() {
    State state;
    unsigned rng = 73;
    auto next = [&]() { rng = rng * 1664525u + 1013904223u; return rng; };
    auto scalar = [&]() { return (int(next() % 2001) - 1000) / 113.0f; };
    for (unsigned trial = 0; trial < 4000; ++trial) {
        state.reset();
        state.num_chans = trial % 3;
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
            for (unsigned c=0;c<3;++c) {
                light.pos[c] = scalar(); light.dir[c] = scalar();
                light.cos_att[c] = scalar();
                light.dist_att[c] = 1.0f + float(next() % 100) / 100.0f;
            }
            if (trial % 17 == 0) for (auto& c : light.dir) c = 0;
        }
        for (unsigned c=0;c<2;++c) { state.chan_mat[c]=next(); state.ambient[c]=next(); }
        DecodedVertex raw{};
        raw.x=scalar(); raw.y=scalar(); raw.z=scalar();
        raw.nx=scalar(); raw.ny=scalar(); raw.nz=scalar();
        raw.color=next(); raw.color1=next(); raw.pn_mtx=trial % kMaxPnMtx;
        for (auto& c : state.pn_mtx[raw.pn_mtx]) c=scalar();
        for (auto& c : state.nrm_mtx[raw.pn_mtx]) c=scalar();
        if (trial % 19 == 0) raw.nx=raw.ny=raw.nz=0;
        PreparedLighting prepared;
        prepare_lighting(state, prepared);
        for (unsigned vertex=0;vertex<8;++vertex) {
            auto reference=raw, actual=raw;
            apply_lighting(state, reference);
            float clip[4]; transform_vertex(state, reference, clip);
            light_and_transform(state, prepared, actual);
            // Colours within one step per byte: the prepared plan and light_channel differ in the
            // odd rounding step (seen once in 4000 trials since the user's static5x lighting work).
            auto near = [](std::uint32_t a, std::uint32_t b) {
                for (unsigned k = 0; k < 4u; ++k) {
                    const int d = int((a >> (8u * k)) & 0xFFu) - int((b >> (8u * k)) & 0xFFu);
                    if (d > 1 || d < -1) return false;
                }
                return true;
            };
            if (!near(actual.color, reference.color) || !near(actual.color1, reference.color1) ||
                actual.x!=clip[0] || actual.y!=clip[1] || actual.z!=clip[2] || actual.w!=clip[3]) {
                std::fprintf(stderr,"prepared lighting mismatch at %u/%u\n",trial,vertex);
                return false;
            }
            raw.x += .125f; raw.color=next();
        }
    }
    return true;
}

int main() {
    if (!test_prepared_lighting()) return 1;
    auto f = std::make_unique<Fifo>();
    auto ref = std::make_unique<Processor>();
    auto cached = std::make_unique<Processor>();
    float positions[8][3];
    std::uint32_t colors[8];
    for (unsigned i=0; i<8; ++i) {
        positions[i][0] = i * .1f; positions[i][1] = i * -.2f; positions[i][2] = -1.0f;
        colors[i] = 0xff102030u + i;
    }
    configure_pos_index16_clr0_index8(*f, VtxFmt0);
    configure_pos_index16_clr0_index8(*f, VtxFmt1);
    f->set_array(AttrPos, positions, sizeof(positions), 12, true);
    f->set_array(AttrClr0, colors, sizeof(colors), 4, true);
    f->set_gen_mode(1, 1, 1);
    f->set_tex_coord_gen(0, TgMtx2x4, TgPos, kTexMtxIdentity);
    // Both repeated and partially overlapping packets, with explicit changes
    // in VAT selection, matrices, lighting and cache invalidation between runs.
    for (unsigned i=0; i<600; ++i) {
        if (i % 80 == 0) {
            float m[12] = {1,0,0,float(i), 0,1,0,0, 0,0,1,0};
            f->load_xf_f32(0, m, 12);
            f->set_chan_mat_color(0, i & 255, 100, 200);
            // Alternate unlit and lit epochs (the worker path snapshots lights and matrices).
            f->set_chan_ctrl(0, (i / 80) % 2 != 0, SrcReg, SrcVtx, 0x03, DfClamp, AfSpot);
            f->load_light(0, 200, 180, 160, 255, float(i % 7), 4.0f, 3.0f);
            f->load_light(1, 90, 90, 255, 255, -3.0f, 2.0f, float(i % 5));
        }
        if (i % 31 == 0) f->write_u8(0x48); // GXInvalidateVtxCache
        std::uint8_t packet[12];
        for (unsigned v=0; v<4; ++v) {
            const unsigned index = (v + i % 4) % 8;
            packet[v*3]=0; packet[v*3+1]=index; packet[v*3+2]=index;
        }
        f->draw(i % 2 ? TriangleStrip : Quads, i % 71 == 0 ? VtxFmt1 : VtxFmt0, 4, packet, sizeof(packet));
    }
    auto run = [&](Processor& p, bool reuse) {
        p.reset(); set_vertex_reuse(reuse);
        return p.process(f->data(), f->size(), true);
    };
    if (!run(*ref, false)) return 1;
    std::vector<DrawExtra> extras;
    for (unsigned i=0; i<ref->draw_count; ++i) extras.push_back(*draw_extra(*ref,i));
    if (!run(*cached, true) || ref->vert_count != cached->vert_count || ref->draw_count != cached->draw_count) return 1;
    for (unsigned i=0; i<ref->draw_count; ++i) {
        const DrawCall& a=ref->draws[i]; const DrawCall& b=cached->draws[i];
        const DrawExtra& x=extras[i]; const DrawExtra& y=*draw_extra(*cached,i);
        // Compare values, not host ABI padding (TlutObj has internal padding).
        if (a.prim!=b.prim || a.fmt!=b.fmt || a.first_vertex!=b.first_vertex || a.vertex_count!=b.vertex_count ||
            a.tev_key!=b.tev_key || a.num_tex_gens!=b.num_tex_gens || a.num_chans!=b.num_chans ||
            std::memcmp(a.tev_stages,b.tev_stages,sizeof(a.tev_stages)) ||
            std::memcmp(a.tev_reg,b.tev_reg,sizeof(a.tev_reg)) ||
            std::memcmp(x.tg0_rows,y.tg0_rows,sizeof(x.tg0_rows)) || x.depth_write!=y.depth_write ||
            x.color_update!=y.color_update || x.alpha_update!=y.alpha_update) {
            std::fprintf(stderr,"reused draw state differs at %u\n",i); return 1;
        }
        for (unsigned t=0;t<kMaxTexMaps;++t) {
            if (x.tex[t].data!=y.tex[t].data || x.tex[t].version!=y.tex[t].version ||
                x.tlut_valid[t]!=y.tlut_valid[t] || x.tlut[t].data!=y.tlut[t].data ||
                x.tlut[t].version!=y.tlut[t].version) return 1;
        }
    }
    for (unsigned i=0; i<ref->vert_count; ++i) {
        const auto& a=ref->verts[i]; const auto& b=cached->verts[i];
        if (a.x!=b.x || a.y!=b.y || a.z!=b.z || a.w!=b.w || a.color!=b.color || a.color1!=b.color1 ||
            std::memcmp(a.tc,b.tc,sizeof(a.tc)) != 0) {
            std::fprintf(stderr,"vertex reuse differs at %u\n",i); return 1;
        }
    }
    // The packed batch the renderer draws from: every vertex (through its remap) packs to
    // what the reference vertex packs to, within composed-matrix rounding.
    auto check_packed = [&](const Processor& p, const char* what) {
        PackedBatch pb{};
        if (!packed_batch(p, &pb)) {
            std::fprintf(stderr, "%s: no packed batch\n", what);
            return false;
        }
        for (unsigned d = 0; d < p.draw_count; ++d) {
            const DrawCall& dc = p.draws[d];
            const unsigned ntc = pack_ntc(dc.num_tex_gens);
            for (unsigned k = 0; k < dc.vertex_count; ++k) {
                const unsigned v = dc.first_vertex + k;
                const unsigned src = pb.remap[v] >= 0 ? static_cast<unsigned>(pb.remap[v]) : v;
                std::uint8_t want[kPackOffTex + 12 * kMaxTexGens];
                pack_vertex(ref->verts[v], ntc, want);
                if (!near_packed(want, pb.arena + pb.offset[src], pack_stride(ntc))) {
                    std::fprintf(stderr, "%s: packed vertex %u (from %u) differs\n", what, v, src);
                    return false;
                }
            }
        }
        return true;
    };
    if (!check_packed(*cached, "inline")) return 1;
    // The vertex worker (another thread doing texgen/lighting/transform from snapshots) must
    // produce the same vertices within composed-matrix rounding, with and without reuse.
    {
        auto worked = std::make_unique<Processor>();
        std::thread worker([] { vertex_worker_main(); });
        vertex_worker_install(VertexWorkerHooks{&sem_wait, &sem_post, &relax});
        for (bool reuse : {false, true}) {
            worked->reset();
            set_vertex_reuse(reuse);
            if (!worked->process(f->data(), f->size(), true)) return 1;
            vertex_worker_drain();
            if (!check_packed(*worked, reuse ? "worker+reuse" : "worker")) return 1;
            resolve_vertex_copies(*worked);  // reused vertices are remaps until resolved
            if (worked->vert_count != ref->vert_count || worked->draw_count != ref->draw_count) {
                std::fprintf(stderr, "worker counts differ\n");
                return 1;
            }
            for (unsigned i = 0; i < ref->vert_count; ++i) {
                const auto& a = ref->verts[i];
                const auto& b = worked->verts[i];
                if (!near_float(a.x,b.x) || !near_float(a.y,b.y) || !near_float(a.z,b.z) || !near_float(a.w,b.w) || a.color != b.color ||
                    a.color1 != b.color1 || !near_floats(a.tc,b.tc,sizeof(a.tc))) {
                    std::fprintf(stderr, "vertex worker differs at %u (reuse %d)\n", i, reuse ? 1 : 0);
                    return 1;
                }
            }
        }
        vertex_worker_uninstall();
        vertex_worker_quit();
        worker.join();
    }
    // GPU lighting (static63): lit epochs pack raw colours plus eye position/normal; the shader's
    // lighting (gpu_lit_reference) over those gives the reference's lit colours. Inline and two
    // vertex workers pack identical bytes.
    {
        set_gpu_lighting(true);
        auto check_lit = [&](const Processor& p, const char* what, std::vector<std::uint8_t>* bytes) {
            PackedBatch pb{};
            if (!packed_batch(p, &pb)) return false;
            unsigned lit_draws = 0;
            for (unsigned d = 0; d < p.draw_count; ++d) {
                const DrawCall& dc = p.draws[d];
                const unsigned ntc = pack_ntc(dc.num_tex_gens);
                const bool gl = draw_gpu_lit(p, d);
                const DrawExtra* x = draw_extra(p, d);
                lit_draws += gl ? 1u : 0u;
                for (unsigned k = 0; k < dc.vertex_count; ++k) {
                    const unsigned v = dc.first_vertex + k;
                    const unsigned src = pb.remap[v] >= 0 ? static_cast<unsigned>(pb.remap[v]) : v;
                    const std::uint8_t* got = pb.arena + pb.offset[src];
                    if (bytes != nullptr) bytes->insert(bytes->end(), got, got + pack_stride(ntc, gl));
                    std::uint8_t want[kPackOffTex + 12 * kMaxTexGens];
                    pack_vertex(ref->verts[v], ntc, want);
                    if (!gl) {
                        if (!near_packed(want,got,pack_stride(ntc))) {
                            std::fprintf(stderr, "%s: unlit packed vertex %u differs\n", what, v);
                            return false;
                        }
                        continue;
                    }
                    if (!near_floats(want,got,16) || !near_floats(want+kPackOffTex,got+kPackOffTex,12*ntc)) {
                        std::fprintf(stderr, "%s: GPU-lit vertex %u position/texcoords differ\n", what, v);
                        return false;
                    }
                    std::uint32_t raw0, raw1;
                    float eye[3], nrm[3];
                    std::memcpy(&raw0, got + kPackOffColor0, 4);
                    std::memcpy(&raw1, got + kPackOffColor1, 4);
                    std::memcpy(eye, got + kPackOffTex + 12 * ntc, 12);
                    std::memcpy(nrm, got + kPackOffTex + 12 * ntc + 12, 12);
                    std::uint32_t c0 = 0, c1 = 0;
                    gpu_lit_reference(x->lit, eye, nrm, raw0, raw1, &c0, &c1);
                    for (unsigned b = 0; b < 4; ++b) {
                        const int d0 = int((c0 >> (8 * b)) & 255u) - int((ref->verts[v].color >> (8 * b)) & 255u);
                        const int d1 = int((c1 >> (8 * b)) & 255u) - int((ref->verts[v].color1 >> (8 * b)) & 255u);
                        if (d0 > 1 || d0 < -1 || d1 > 1 || d1 < -1) {
                            std::fprintf(stderr, "%s: GPU-lit vertex %u lights to %08X/%08X, CPU %08X/%08X\n", what, v, c0,
                                         c1, ref->verts[v].color, ref->verts[v].color1);
                            return false;
                        }
                    }
                }
            }
            if (lit_draws == 0) {
                std::fprintf(stderr, "%s: no GPU-lit draws\n", what);
                return false;
            }
            return true;
        };
        auto lit_inline = std::make_unique<Processor>();
        lit_inline->reset();
        set_vertex_reuse(true);
        if (!lit_inline->process(f->data(), f->size(), true)) return 1;
        std::vector<std::uint8_t> inline_bytes, worker_bytes;
        if (!check_lit(*lit_inline, "gpu-lit inline", &inline_bytes)) return 1;
        auto lit_worked = std::make_unique<Processor>();
        std::thread w1([] { vertex_worker_main(); });
        std::thread w2([] { vertex_worker_main(); });
        vertex_worker_install(VertexWorkerHooks{&sem_wait, &sem_post, &relax});
        lit_worked->reset();
        if (!lit_worked->process(f->data(), f->size(), true)) return 1;
        vertex_worker_drain();
        if (!check_lit(*lit_worked, "gpu-lit 2 workers", &worker_bytes)) return 1;
        bool packed_match=inline_bytes.size()==worker_bytes.size();
        std::size_t off=0;
        for(unsigned d=0;packed_match && d<lit_inline->draw_count;++d){
            const auto& dc=lit_inline->draws[d];const unsigned stride=pack_stride(pack_ntc(dc.num_tex_gens),draw_gpu_lit(*lit_inline,d));
            for(unsigned k=0;packed_match && k<dc.vertex_count;++k,off+=stride){
                packed_match=near_packed(inline_bytes.data()+off,worker_bytes.data()+off,stride);

            }
        }
        if (!packed_match) {
            std::fprintf(stderr, "gpu-lit: workers packed different bytes than inline\n");
            return 1;
        }
        vertex_worker_uninstall();
        vertex_worker_quit();
        vertex_worker_quit();
        sem_post();
        sem_post();
        w1.join();
        w2.join();
        set_gpu_lighting(false);
        std::printf("gpu-lit vertex path ok\n");
    }
    // Frame skipping: state is still decoded, primitives are not.
    {
        auto skipped = std::make_unique<Processor>();
        skipped->reset();
        set_skip_draws(true);
        const bool ok = skipped->process(f->data(), f->size(), true);
        set_skip_draws(false);
        if (!ok || skipped->draw_count != 0 || skipped->vert_count != 0 || skipped_primitives() != 600 ||
            std::memcmp(skipped->state.pn_mtx, ref->state.pn_mtx, sizeof(ref->state.pn_mtx)) != 0 ||
            skipped->state.chan_mat[0] != ref->state.chan_mat[0]) {
            std::fprintf(stderr, "skip_draws: draws=%u verts=%u skipped=%u\n", skipped->draw_count,
                         skipped->vert_count, skipped_primitives());
            return 1;
        }
        skipped->end_frame();
        if (skipped_primitives() != 0) return 1;
    }
    // Process boundaries must retire cached external array values.
    positions[0][0] += 10;
    if (!run(*ref, false) || !run(*cached, true) || ref->verts[0].x != cached->verts[0].x) return 1;
    for (bool reuse : {false,true}) {
        auto start=std::chrono::steady_clock::now();
        for (unsigned i=0;i<100;++i) if (!run(*cached,reuse)) return 1;
        double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        std::printf("600-draw decode x100, vertex reuse %s: %.2f ms\n",reuse?"on":"off",ms);
    }
    set_vertex_reuse(true);
}
