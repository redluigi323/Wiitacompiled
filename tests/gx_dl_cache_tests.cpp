// static64: the display-list vertex cache, the GPU vertex ring and draw merge keys.
//
// A frame whose geometry sits in display lists is processed without the cache (the reference),
// then again and again with it: the first frame fills, later frames hit. Every drawn vertex must
// pack to what the reference packs (clip position to rounding - the worker folds matrices), inline
// and with two vertex workers, packing into a small ring that wraps. Changing the arrays under the
// cache must be caught by the spot check within four hits.
#include "mkwv/gx/decode.hpp"
#include "mkwv/gx/dl_cache.hpp"
#include "mkwv/gx/draw_state.hpp"
#include "mkwv/gx/vertex_worker.hpp"

#include <condition_variable>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace mkwv::gx;

namespace {

std::mutex g_m;
std::condition_variable g_cv;
int g_sem = 0;
void sem_wait() { std::unique_lock<std::mutex> l(g_m); g_cv.wait(l, [] { return g_sem > 0; }); --g_sem; }
void sem_post() { { std::lock_guard<std::mutex> l(g_m); ++g_sem; } g_cv.notify_all(); }
void relax() { std::this_thread::yield(); }
unsigned g_syncs = 0;
void ring_sync() { ++g_syncs; }

float g_pos[64][3], g_nrm[64][3];
std::uint32_t g_col[64];

void fill_arrays(float bump) {
    for (unsigned i = 0; i < 64; ++i) {
        g_pos[i][0] = i * 0.31f - 9.0f + bump;
        g_pos[i][1] = (i % 7) * -0.6f + bump;
        g_pos[i][2] = -4.0f - i * 0.05f;
        g_nrm[i][0] = (i % 3) - 1.0f;
        g_nrm[i][1] = 0.5f;
        g_nrm[i][2] = (i % 2) ? 1.0f : -0.25f;
        g_col[i] = 0x80402010u + i * 0x01030507u + static_cast<std::uint32_t>(bump * 16.0f);
    }
}

constexpr std::uint32_t kDlAddr = 0x80002000u;

// The display list: strips, with a matrix load and a lighting change in the middle.
std::unique_ptr<Fifo> make_dl() {
    auto dl = std::make_unique<Fifo>();
    for (unsigned i = 0; i < 30; ++i) {
        if (i == 12) {
            const float m[12] = {1.0f, 0.2f, 0.0f, 0.5f, -0.2f, 1.0f, 0.1f, 1.0f, 0.0f, -0.1f, 1.0f, -6.0f};
            dl->load_xf_f32(0, m, 12);
        }
        if (i == 20) {
            dl->set_chan_ctrl(0, false, SrcReg, SrcVtx, 0x00, DfClamp, AfSpot);
        }
        std::uint8_t packet[10 * 4];
        const unsigned n = 3 + i % 8;
        for (unsigned v = 0; v < n; ++v) {
            const unsigned idx = (v * 5 + i * 3) % 64;
            packet[v * 4 + 0] = 0;
            packet[v * 4 + 1] = static_cast<std::uint8_t>(idx);
            packet[v * 4 + 2] = static_cast<std::uint8_t>(idx);
            packet[v * 4 + 3] = static_cast<std::uint8_t>(idx);
        }
        dl->draw(i % 3 == 0 ? Triangles : TriangleStrip, VtxFmt0, static_cast<std::uint16_t>(n), packet, n * 4u);
    }
    return dl;
}

std::unique_ptr<Fifo> make_frame(const Fifo& dl) {
    auto f = std::make_unique<Fifo>();
    configure_pos_index16_nrm_index8_clr0_index8(*f, VtxFmt0);
    f->set_array(AttrPos, g_pos, sizeof(g_pos), 12, true);
    f->set_array(AttrNrm, g_nrm, sizeof(g_nrm), 12, true);
    f->set_array(AttrClr0, g_col, sizeof(g_col), 4, true);
    f->set_gen_mode(0, 1, 1);
    const float m[12] = {0.9f, 0.1f, 0.0f, 1.0f, -0.1f, 0.9f, 0.2f, 2.0f, 0.0f, -0.2f, 1.0f, -5.0f};
    f->load_xf_f32(0, m, 12);
    f->set_chan_mat_color(0, 200, 150, 100, 255);
    f->set_chan_amb_color(0, 20, 30, 40, 255);
    f->set_chan_ctrl(0, true, SrcReg, SrcVtx, 0x03, DfClamp, AfSpot);
    f->load_light(0, 200, 180, 160, 255, 1.0f, 4.0f, 3.0f);
    f->load_light(1, 90, 90, 255, 255, -3.0f, 2.0f, 1.0f);
    f->call_dl(kDlAddr, dl.size());
    // Drawn again in the same frame with another matrix (the same cache entries, still filling).
    const float m2[12] = {0.8f, 0.0f, 0.3f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f, -0.3f, 0.0f, 0.8f, -7.0f};
    f->load_xf_f32(0, m2, 12);
    f->set_chan_ctrl(0, true, SrcReg, SrcVtx, 0x03, DfClamp, AfSpot);
    f->call_dl(kDlAddr, dl.size());
    // And a primitive straight in the FIFO (never cached).
    std::uint8_t packet[4 * 4];
    for (unsigned v = 0; v < 4; ++v) {
        packet[v * 4 + 0] = 0;
        packet[v * 4 + 1] = packet[v * 4 + 2] = packet[v * 4 + 3] = static_cast<std::uint8_t>(v + 1);
    }
    f->draw(TriangleStrip, VtxFmt0, 4, packet, sizeof(packet));
    return f;
}

struct Packed {
    std::vector<std::uint8_t> bytes;
    std::vector<unsigned> strides;
};

bool collect(const Processor& p, Packed& out) {
    PackedBatch pb{};
    if (!packed_batch(p, &pb)) return false;
    for (unsigned d = 0; d < p.draw_count; ++d) {
        const DrawCall& dc = p.draws[d];
        const bool gl = draw_gpu_lit(p, d);
        const unsigned st = pack_stride(pack_ntc(dc.num_tex_gens), gl);
        for (unsigned k = 0; k < dc.vertex_count; ++k) {
            const unsigned v = dc.first_vertex + k;
            const unsigned src = pb.remap[v] >= 0 ? static_cast<unsigned>(pb.remap[v]) : v;
            const std::uint8_t* got = pb.arena + pb.offset[src];
            out.bytes.insert(out.bytes.end(), got, got + st);
            out.strides.push_back(st);
        }
    }
    return true;
}

bool same(const Packed& a, const Packed& b, const char* what) {
    if (a.strides != b.strides || a.bytes.size() != b.bytes.size()) {
        std::fprintf(stderr, "%s: vertex count/layout differs (%zu/%zu verts)\n", what, a.strides.size(), b.strides.size());
        return false;
    }
    std::size_t off = 0;
    for (std::size_t i = 0; i < a.strides.size(); ++i) {
        const unsigned st = a.strides[i];
        for (unsigned k = 0; k < st; k += 4) {
            if (k == kPackOffColor0 || k == kPackOffColor1) {
                if (std::memcmp(&a.bytes[off + k], &b.bytes[off + k], 4) != 0) {
                    std::fprintf(stderr, "%s: vertex %zu colour differs\n", what, i);
                    return false;
                }
                continue;
            }
            float x, y;
            std::memcpy(&x, &a.bytes[off + k], 4);
            std::memcpy(&y, &b.bytes[off + k], 4);
            if (std::fabs(x - y) > 1e-4f * (1.0f + std::fabs(x))) {
                std::fprintf(stderr, "%s: vertex %zu float at +%u: %f vs %f\n", what, i, k, x, y);
                return false;
            }
        }
        off += st;
    }
    return true;
}

bool run_frame(Processor& p, const Fifo& f, const Fifo& dl, Packed& out) {
    p.reset();
    p.map_range(kDlAddr, dl.data(), dl.size());
    if (!p.process(f.data(), f.size(), true)) return false;
    vertex_worker_drain();
    return collect(p, out);
}

} // namespace

int main() {
    set_gpu_lighting(true);
    fill_arrays(0.0f);
    auto dl = make_dl();
    auto frame = make_frame(*dl);
    auto p = std::make_unique<Processor>();

    // Reference: no cache.
    dl_cache_install(nullptr, 0);
    Packed ref;
    if (!run_frame(*p, *frame, *dl, ref)) return 1;
    if (ref.strides.empty()) return 1;

    std::vector<std::uint8_t> pool(12u * 1024u * 1024u);
    std::vector<std::uint8_t> ring(1024u * 1024u);  // wraps every ~35 frames
    for (int staging = 0; staging <= 1; ++staging) {
    vertex_ring_set_staging(staging != 0);
    for (int workers = 0; workers <= 2; workers += 2) {
        std::thread t1, t2;
        if (workers != 0) {
            t1 = std::thread([] { vertex_worker_main(); });
            t2 = std::thread([] { vertex_worker_main(); });
            vertex_worker_install(VertexWorkerHooks{&sem_wait, &sem_post, &relax});
            vertex_worker_set_mirror(false);  // as on the Vita
        }
        dl_cache_install(pool.data(), static_cast<std::uint32_t>(pool.size()));
        vertex_ring_install(ring.data(), static_cast<std::uint32_t>(ring.size()), &ring_sync);
        DlCacheStats st{};
        dl_cache_take_stats(&st);
        for (int f = 0; f < 90; ++f) {
            Packed got;
            if (!run_frame(*p, *frame, *dl, got)) return 1;
            vertex_ring_frame_mark();
            char what[64];
            std::snprintf(what, sizeof(what), "%s frame %d", workers ? "workers" : "inline", f);
            if (!same(ref, got, what)) return 1;
            dl_cache_take_stats(&st);
            if (f == 0 && (st.fills == 0 || st.hits != 0)) {
                std::fprintf(stderr, "%s: expected fills only (hits %u fills %u)\n", what, st.hits, st.fills);
                return 1;
            }
            if (f >= 1 && (st.hits == 0 || st.fills != 0)) {
                std::fprintf(stderr, "%s: expected hits only (hits %u fills %u rej %u busy %u)\n", what, st.hits, st.fills,
                             st.rejects, st.busy);
                return 1;
            }
        }
        // Arrays change under the cache: within four hits every primitive must be re-decoded.
        fill_arrays(0.5f);
        Packed ref2;
        dl_cache_install(nullptr, 0);
        vertex_ring_install(nullptr, 0, nullptr);
        if (!run_frame(*p, *frame, *dl, ref2)) return 1;
        dl_cache_install(pool.data(), static_cast<std::uint32_t>(pool.size()));
        vertex_ring_install(ring.data(), static_cast<std::uint32_t>(ring.size()), &ring_sync);
        {
            // Fill with the new arrays' values, then change back: the spot checks must catch it.
            fill_arrays(0.0f);
            Packed warm;
            for (int f = 0; f < 3; ++f) {
                warm = Packed{};
                if (!run_frame(*p, *frame, *dl, warm)) return 1;
                vertex_ring_frame_mark();
            }
            fill_arrays(0.5f);
            Packed got;
            for (int f = 0; f < 5; ++f) {
                got = Packed{};
                if (!run_frame(*p, *frame, *dl, got)) return 1;
                vertex_ring_frame_mark();
            }
            dl_cache_take_stats(&st);
            if (!same(ref2, got, "after array change")) return 1;
            if (st.rejects == 0) {
                std::fprintf(stderr, "array change: no spot check failed\n");
                return 1;
            }
            std::printf("array change caught: %u rejects\n", st.rejects);
        }
        fill_arrays(0.0f);
        vertex_ring_install(nullptr, 0, nullptr);
        dl_cache_install(nullptr, 0);
        if (workers != 0) {
            vertex_worker_uninstall();
            vertex_worker_quit();
            vertex_worker_quit();
            sem_post();
            sem_post();
            t1.join();
            t2.join();
            vertex_worker_set_mirror(true);
        }
        std::printf("dl cache + ring (%s): ok\n", workers ? "2 workers" : "inline");
    }

    }
    vertex_ring_set_staging(true);

    // A staged batch larger than the small test GPU ring must remain in the cached arena
    // for client submission, instead of copying past the ring's end.
    {
        std::vector<std::uint8_t> small_ring(1024u * 1024u, 0xCC);
        p->reset();
        vertex_ring_install(small_ring.data(), static_cast<std::uint32_t>(small_ring.size()), &ring_sync);
        vertex_batch_begin(p.get());
        const unsigned stride = pack_stride(kMaxTexGens, true);
        const auto offset = vertex_batch_alloc(0, kMaxDecodedVerts, kMaxTexGens, true);
        std::memset(vertex_batch_arena() + offset, 0x51, kMaxDecodedVerts * stride);
        p->vert_count = kMaxDecodedVerts;
        PackedBatch batch{};
        if (!packed_batch(*p, &batch) || batch.arena == small_ring.data() || batch.arena[0] != 0x51 ||
            small_ring[0] != 0xCC || small_ring.back() != 0xCC) return 1;
        vertex_ring_install(nullptr, 0, nullptr);
    }

    // Before the third frame, two large batches can already wrap a small ring. The
    // second upload must synchronize before overwriting the first GPU-visible batch.
    {
        std::vector<std::uint8_t> gpu(3u * 1024u * 1024u, 0xCC);
        p->reset();
        vertex_ring_install(gpu.data(), static_cast<std::uint32_t>(gpu.size()), &ring_sync);
        const unsigned before = g_syncs;
        for (unsigned batch_no = 0; batch_no < 2; ++batch_no) {
            vertex_batch_begin(p.get());
            const unsigned stride = pack_stride(kMaxTexGens, true);
            vertex_batch_alloc(0, kMaxDecodedVerts, kMaxTexGens, true);
            std::memset(vertex_batch_arena(), batch_no ? 0x22 : 0x11, kMaxDecodedVerts * stride);
            p->vert_count = kMaxDecodedVerts;
            PackedBatch batch{};
            if (!packed_batch(*p, &batch) || batch.arena != gpu.data() ||
                batch.arena[batch.offset[0]] != (batch_no ? 0x22 : 0x11)) return 1;
            const auto bytes = vertex_ring_frame_bytes();
            if (!packed_batch(*p, &batch) || vertex_ring_frame_bytes() != bytes) return 1;
        }
        if (g_syncs != before + 1) return 1;
        vertex_ring_install(nullptr, 0, nullptr);
    }

    // Merge keys: equal exactly when the full compare says the draws may share a GL call.
    {
        dl_cache_install(nullptr, 0);
        p->reset();
        p->map_range(kDlAddr, dl->data(), dl->size());
        if (!p->process(frame->data(), frame->size(), true)) return 1;
        unsigned equal = 0, pairs = 0;
        for (unsigned a = 0; a < p->draw_count; ++a) {
            for (unsigned b = a + 1; b < p->draw_count; ++b) {
                DrawCall x = p->draws[a], y = p->draws[b];
                x.prim = y.prim = Triangles;
                x.fmt = y.fmt = VtxFmt0;
                x.first_vertex = y.first_vertex = 0;
                x.vertex_count = y.vertex_count = 0;
                const DrawExtra* ea = draw_extra(*p, a);
                const DrawExtra* eb = draw_extra(*p, b);
                const bool full = std::memcmp(&x, &y, sizeof(x)) == 0 &&
                                  (ea == eb || std::memcmp(ea, eb, offsetof(DrawExtra, tg0_mtx)) == 0);
                const bool keyed = draw_merge_key(*p, a) == draw_merge_key(*p, b);
                ++pairs;
                equal += full ? 1u : 0u;
                if (full != keyed) {
                    std::fprintf(stderr, "merge key disagrees for draws %u/%u (full %d)\n", a, b, full ? 1 : 0);
                    return 1;
                }
            }
        }
        std::printf("merge keys: %u pairs, %u mergeable\n", pairs, equal);
    }
    set_gpu_lighting(false);
    std::printf("dl cache tests: ok\n");
    return 0;
}
