#include "mkwv/gx/backend.hpp"
#include "mkwv/gx/demo.hpp"
#include "mkwv/gx/submit.hpp"
#include "mkwv/gxm/device.hpp"

#include <new>

namespace mkwv::gx {
namespace {

class GxmBackend final : public Backend {
public:
    ~GxmBackend() override { shutdown(); }

    bool initialize() override {
        if (device_) {
            return true;
        }
        device_ = mkwv::gxm::create_device();
        return device_ != nullptr;
    }

    void shutdown() override {
        if (device_) {
            mkwv::gxm::destroy_device(device_);
            device_ = nullptr;
        }
    }

    void begin_frame() override {}

    void draw_debug_geometry() override {
        const float time = static_cast<float>(mkwv::gxm::frame_index(device_)) / 60.0f;
        render_demo(device_, time);
    }

    void present_processor(const Processor& processor) override {
        if (device_ == nullptr) {
            return;
        }
        gxm::begin_frame(device_, 0.08f, 0.10f, 0.22f, 1.0f);
        submit_processor(device_, processor);
        gxm::end_frame(device_);
    }

    void end_frame() override { mkwv::gxm::end_frame(device_); }

private:
    mkwv::gxm::Device* device_ = nullptr;
};

} // namespace

Backend* create_gxm_backend() {
    return new (std::nothrow) GxmBackend();
}

} // namespace mkwv::gx
