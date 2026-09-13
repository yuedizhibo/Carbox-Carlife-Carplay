#pragma once
#include "forward/carplay_forward.hpp"
#include "wirelesscarplay/catplay_record_observer.hpp"
#include <atomic>

namespace zero2w::forward {
struct CaptureSnapshot { std::uint64_t records{}, dropped{}, unsupported{}; };
// CPMF does not contain the negotiated frame rate. Supply the actual input configuration.
class CatPlayForwardCapture final : public mvp::CatPlayRecordObserver {
public:
    explicit CatPlayForwardCapture(std::shared_ptr<WirelessEndpoint>, std::uint32_t configured_fps);
    ~CatPlayForwardCapture() override;
    void on_catplay_record(const mvp::catplay_media::Header&, std::span<const std::uint8_t>) noexcept override;
    void on_catplay_disconnect() noexcept override;
    CaptureSnapshot snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace zero2w::forward
