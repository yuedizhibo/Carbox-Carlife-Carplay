#pragma once
#include "core/session_core.hpp"
#include "wired_carplay/output.hpp"
#include <memory>
#include <mutex>
#include <deque>
#include <map>

namespace zero2w::forward {
namespace w = zero2w::wired;
using Stream = std::variant<w::ScreenConfiguration, w::AudioConfiguration>;
struct InputState {
    std::uint64_t generation{}, session{};
    bool connected{};
    std::vector<Stream> streams;
    w::Capabilities reverse_capabilities; // Only actual phone-backend support; legacy CPMF leaves empty.
};
struct VehicleContext { std::uint64_t output_epoch{}; w::PeerInfo peer; };
using ReversePayload = std::variant<w::ControlCommand, w::Iap2Message, w::ServiceMessage>;
struct ReverseRequest {
    std::uint64_t generation{}, session{}, id{};
    ReversePayload payload;
    w::TimePoint deadline;
};
struct PhoneRequest { std::uint64_t generation{}, id{}; w::Operation operation; w::TimePoint deadline; };

// Input-facing bounded seam. New input backends may provide native controls/microphone.
// The legacy CPMF capture fills media/stream state but MUST NOT advertise those missing APIs.
class WirelessEndpoint {
public:
    explicit WirelessEndpoint(std::size_t packets = 64, std::size_t bytes = 8 * 1024 * 1024);
    std::uint64_t begin(std::uint64_t session, w::Capabilities reverse = {});
    void end();
    InputState state() const;
    std::optional<VehicleContext> vehicle_info() const; // Available before phone begin: original car displays/HID/capabilities.
    w::Status set_stream(std::uint64_t generation, Stream stream);
    w::Status remove_stream(std::uint64_t generation, w::StreamId id);
    w::Status push_media(std::uint64_t generation, w::MediaPacket packet);
    std::optional<w::MediaPacket> take_media(std::uint64_t generation);
    void discard_media();
    w::Ticket request_output(std::uint64_t generation, w::Operation operation, w::Milliseconds timeout = w::Milliseconds{3000});
    std::optional<PhoneRequest> take_phone_request();
    w::Status complete_phone(std::uint64_t generation, std::uint64_t id, w::Status status, w::Bytes response = {});
    std::optional<w::Completion> pop_phone_completion();
    // Core posts a car request; input backend consumes and explicitly acknowledges execution.
    w::Ticket post_reverse(std::uint64_t generation, ReversePayload payload, w::TimePoint deadline);
    std::optional<ReverseRequest> take_reverse();
    w::Status complete_reverse(std::uint64_t generation, std::uint64_t id, w::Status status, w::Bytes response = {});
    std::optional<w::Completion> pop_reverse_completion();
    w::Status post_microphone(std::uint64_t generation, w::MediaPacket packet);
    std::optional<w::MediaPacket> take_microphone(std::uint64_t generation);
    void cancel_requests(w::Status reason);
    void expire(w::TimePoint now);
private:
    friend class CarPlayForwarder;
    void update_vehicle(std::optional<VehicleContext>);
    void reset_locked();
    mutable std::mutex mutex_;
    InputState state_;
    std::optional<VehicleContext> vehicle_;
    std::size_t packet_limit_, byte_limit_, media_bytes_{}, mic_bytes_{};
    std::uint64_t next_id_{1};
    std::deque<w::MediaPacket> media_, mic_;
    std::map<std::uint64_t, PhoneRequest> phones_;
    std::map<std::uint64_t, ReverseRequest> reverse_;
    std::vector<std::uint64_t> taken_phones_, taken_reverse_;
    std::deque<w::Completion> phone_done_, reverse_done_;
};

enum class Phase { Inactive, WaitingOutput, Configuring, Forwarding, Draining, Failed };
struct ForwardSnapshot {
    Phase phase{Phase::Inactive};
    std::uint64_t generation{}, session{}, output_epoch{};
    std::size_t active_streams{}, pending_controls{};
    std::uint64_t forwarded_packets{}, forwarded_bytes{}, dropped_packets{}, reverse_commands{}, microphone_packets{};
    w::Status last_error;
};
// Single-executor coordinator: tick/stop/snapshot are called from the same owning thread.
// Input producer and output protocol backend may run concurrently. No USB start or transcode.
class CarPlayForwarder {
public:
    CarPlayForwarder(mvp::SessionCore&, w::WiredCarPlayOutput&, std::shared_ptr<WirelessEndpoint>, std::string source_id = "catplay-real");
    ~CarPlayForwarder();
    void tick();
    void start(); // Re-enable after stop; does not activate USB.
    void stop();
    ForwardSnapshot snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace zero2w::forward
