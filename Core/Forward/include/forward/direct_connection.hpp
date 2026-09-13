#pragma once
#include "forward/catplay_capture.hpp"
#include "wirelesscarplay/catplay_media_client.hpp"

namespace zero2w::forward {
// Composition for the existing native CPMF client. Holds no transport thread and
// does not start USB. Construct BEFORE client.start(), then tick on Core's executor.
// Client/Output/Core must outlive this object. Owns the client's single observer slot.
class CarPlayDirectConnection {
public:
    CarPlayDirectConnection(mvp::SessionCore& core, mvp::CatPlayMediaClient& client,
        w::WiredCarPlayOutput& output, std::uint32_t configured_fps)
        : client_(client), endpoint_(std::make_shared<WirelessEndpoint>()),
          capture_(std::make_shared<CatPlayForwardCapture>(endpoint_, configured_fps)),
          forwarder_(core, output, endpoint_) { client_.set_record_observer(capture_); }
    ~CarPlayDirectConnection() { client_.set_record_observer(nullptr); forwarder_.stop(); }
    CarPlayDirectConnection(const CarPlayDirectConnection&) = delete;
    CarPlayDirectConnection& operator=(const CarPlayDirectConnection&) = delete;
    void tick() { forwarder_.tick(); }
    void start() { forwarder_.start(); }
    void stop() { forwarder_.stop(); }
    ForwardSnapshot snapshot() const { return forwarder_.snapshot(); }
    CaptureSnapshot capture_snapshot() const { return capture_->snapshot(); }
private:
    mvp::CatPlayMediaClient& client_;
    std::shared_ptr<WirelessEndpoint> endpoint_;
    std::shared_ptr<CatPlayForwardCapture> capture_;
    CarPlayForwarder forwarder_;
};
} // namespace zero2w::forward
