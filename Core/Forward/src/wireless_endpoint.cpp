#include "forward/carplay_forward.hpp"
#include <algorithm>
#include <stdexcept>

namespace zero2w::forward {
namespace {
constexpr std::size_t kRequests = 32, kPayload = 1024 * 1024;
w::Status err(w::Error e) { return {e, {}}; }
template<class T> bool has(const std::vector<T>& list, const T& item) { return std::find(list.begin(), list.end(), item) != list.end(); }
w::StreamId stream_id(const Stream& s) { return std::visit([](const auto& v) { return v.id; }, s); }
bool valid(const ReversePayload& p) {
    return std::visit([](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, w::ControlCommand>) return w::validate_control_command(v);
        else if constexpr (std::is_same_v<T, w::Iap2Message>) return w::describe(v.type) && v.parameters.size() <= kPayload;
        else return w::validate_service_message(v);
    }, p);
}
bool valid_phone(const w::Operation& op) {
    if (auto p = std::get_if<w::SendControl>(&op)) return w::validate_control_command(p->command) && w::describe(p->command.type)->direction != w::Direction::AccessoryToDevice;
    if (auto p = std::get_if<w::SendIap2>(&op)) return valid(p->message);
    if (auto p = std::get_if<w::SendService>(&op)) return valid(p->message);
    if (auto p = std::get_if<w::AssertModes>(&op)) return w::validate_control_command({w::CommandType::ModesChanged, p->state});
    return false;
}
}
WirelessEndpoint::WirelessEndpoint(std::size_t packets, std::size_t bytes) : packet_limit_(packets), byte_limit_(bytes) {
    if (!packets || packets > 256 || !bytes || bytes > 32 * 1024 * 1024) throw std::invalid_argument("invalid Forward input queue limits");
}
void WirelessEndpoint::reset_locked() {
    state_.streams.clear(); media_.clear(); mic_.clear(); media_bytes_ = mic_bytes_ = 0;
    phones_.clear(); reverse_.clear(); taken_phones_.clear(); taken_reverse_.clear(); phone_done_.clear(); reverse_done_.clear();
}
std::uint64_t WirelessEndpoint::begin(std::uint64_t session, w::Capabilities cap) {
    std::lock_guard lock(mutex_);
    if (!session || cap.commands.size() > 13 || cap.csm_messages.size() > 256 || cap.services.size() > 64 || cap.video_codecs.size() > 4 || cap.audio_formats.size() > 64 || cap.audio_streams.size() > 16 || cap.display_roles.size() > 3) return 0;
    if (state_.connected && state_.session == session) return state_.generation;
    reset_locked(); ++state_.generation; state_.session = session; state_.connected = true; state_.reverse_capabilities = std::move(cap); return state_.generation;
}
void WirelessEndpoint::end() { std::lock_guard lock(mutex_); reset_locked(); ++state_.generation; state_.connected = false; state_.session = 0; state_.reverse_capabilities = {}; }
InputState WirelessEndpoint::state() const { std::lock_guard lock(mutex_); return state_; }
std::optional<VehicleContext> WirelessEndpoint::vehicle_info() const { std::lock_guard lock(mutex_); return vehicle_; }
void WirelessEndpoint::update_vehicle(std::optional<VehicleContext> v) { std::lock_guard lock(mutex_); vehicle_ = std::move(v); }
w::Status WirelessEndpoint::set_stream(std::uint64_t g, Stream s) {
    std::lock_guard lock(mutex_);
    if (g != state_.generation) return err(w::Error::StaleSession);
    if (!state_.connected) return err(w::Error::InvalidState);
    const auto id = stream_id(s); if (!id) return err(w::Error::InvalidArgument);
    if (auto p = std::get_if<w::ScreenConfiguration>(&s); p && (p->display_uuid.size() > 1024 || p->codec_configuration.size() > 64 * 1024)) return err(w::Error::InvalidArgument);
    auto it = std::find_if(state_.streams.begin(), state_.streams.end(), [&](const auto& v) { return stream_id(v) == id; });
    if (it == state_.streams.end()) {
        if (state_.streams.size() >= 8) return err(w::Error::Backpressure);
        state_.streams.push_back(std::move(s));
    } else {
        *it = std::move(s);
        std::erase_if(media_, [&](const auto& p) { if (p.stream_id != id) return false; media_bytes_ -= p.data.size(); return true; });
        std::erase_if(mic_, [&](const auto& p) { if (p.stream_id != id) return false; mic_bytes_ -= p.data.size(); return true; });
    }
    return {};
}
w::Status WirelessEndpoint::remove_stream(std::uint64_t g, w::StreamId id) {
    std::lock_guard lock(mutex_);
    if (g != state_.generation) return err(w::Error::StaleSession);
    auto count = std::erase_if(state_.streams, [&](const auto& s) { return stream_id(s) == id; });
    std::erase_if(media_, [&](const auto& p) { if (p.stream_id != id) return false; media_bytes_ -= p.data.size(); return true; });
    std::erase_if(mic_, [&](const auto& p) { if (p.stream_id != id) return false; mic_bytes_ -= p.data.size(); return true; });
    return count ? w::Status{} : err(w::Error::NotFound);
}
w::Status WirelessEndpoint::push_media(std::uint64_t g, w::MediaPacket p) {
    std::lock_guard lock(mutex_);
    if (g != state_.generation || p.epoch != g) return err(w::Error::StaleSession);
    if (!state_.connected) return err(w::Error::InvalidState);
    if (!p.stream_id || p.data.empty() || p.data.size() > kPayload) return err(w::Error::InvalidArgument);
    if (std::none_of(state_.streams.begin(), state_.streams.end(), [&](const auto& s) { return stream_id(s) == p.stream_id; })) return err(w::Error::NotFound);
    if (media_.size() >= packet_limit_ || p.data.size() > byte_limit_ - media_bytes_) return err(w::Error::Backpressure);
    media_bytes_ += p.data.size(); media_.push_back(std::move(p)); return {};
}
std::optional<w::MediaPacket> WirelessEndpoint::take_media(std::uint64_t g) {
    std::lock_guard lock(mutex_);
    if (g != state_.generation || media_.empty()) return {};
    auto p = std::move(media_.front()); media_.pop_front(); media_bytes_ -= p.data.size(); return p;
}
void WirelessEndpoint::discard_media() { std::lock_guard lock(mutex_); media_.clear(); mic_.clear(); media_bytes_ = mic_bytes_ = 0; }
w::Ticket WirelessEndpoint::request_output(std::uint64_t g, w::Operation op, w::Milliseconds timeout) {
    std::lock_guard lock(mutex_);
    if (g != state_.generation) return {err(w::Error::StaleSession), g, 0};
    if (!state_.connected) return {err(w::Error::InvalidState), g, 0};
    if (!valid_phone(op) || timeout.count() <= 0 || timeout > w::Milliseconds{300000}) return {err(w::Error::InvalidArgument), g, 0};
    if (phones_.size() + phone_done_.size() >= kRequests) return {err(w::Error::Backpressure), g, 0};
    auto id = next_id_++; phones_.emplace(id, PhoneRequest{g, id, std::move(op), w::Clock::now() + timeout}); return {{}, g, id};
}
std::optional<PhoneRequest> WirelessEndpoint::take_phone_request() {
    std::lock_guard lock(mutex_);
    for (const auto& [id, r] : phones_) if (!has(taken_phones_, id)) { taken_phones_.push_back(id); return r; }
    return {};
}
w::Status WirelessEndpoint::complete_phone(std::uint64_t g, std::uint64_t id, w::Status s, w::Bytes response) {
    std::lock_guard lock(mutex_);
    if (g != state_.generation) return err(w::Error::StaleSession);
    if (response.size() > kPayload || s.detail.size() > 1024) return err(w::Error::InvalidArgument);
    auto it = phones_.find(id); if (it == phones_.end()) return err(w::Error::NotFound);
    if (w::Clock::now() >= it->second.deadline) { s = err(w::Error::Timeout); response.clear(); }
    phone_done_.push_back({g, id, std::move(s), std::move(response)}); phones_.erase(it); std::erase(taken_phones_, id); return {};
}
std::optional<w::Completion> WirelessEndpoint::pop_phone_completion() {
    std::lock_guard lock(mutex_); if (phone_done_.empty()) return {};
    auto r = std::move(phone_done_.front()); phone_done_.pop_front(); return r;
}
w::Ticket WirelessEndpoint::post_reverse(std::uint64_t g, ReversePayload p, w::TimePoint deadline) {
    std::lock_guard lock(mutex_);
    if (g != state_.generation) return {err(w::Error::StaleSession), g, 0};
    if (!state_.connected) return {err(w::Error::Disconnected), g, 0};
    if (!valid(p)) return {err(w::Error::InvalidArgument), g, 0};
    if (deadline <= w::Clock::now()) return {err(w::Error::Timeout), g, 0};
    const auto& cap = state_.reverse_capabilities;
    bool supported = std::visit([&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, w::ControlCommand>) return w::describe(v.type)->direction != w::Direction::DeviceToAccessory && has(cap.commands, v.type);
        else if constexpr (std::is_same_v<T, w::Iap2Message>) return has(cap.csm_messages, v.type);
        else return has(cap.services, v.service);
    }, p);
    if (!supported) return {err(w::Error::NotSupported), g, 0};
    if (reverse_.size() + reverse_done_.size() >= kRequests) return {err(w::Error::Backpressure), g, 0};
    auto id = next_id_++; reverse_.emplace(id, ReverseRequest{g, state_.session, id, std::move(p), deadline}); return {{}, g, id};
}
std::optional<ReverseRequest> WirelessEndpoint::take_reverse() {
    std::lock_guard lock(mutex_);
    for (const auto& [id, r] : reverse_) if (!has(taken_reverse_, id)) { taken_reverse_.push_back(id); return r; }
    return {};
}
w::Status WirelessEndpoint::complete_reverse(std::uint64_t g, std::uint64_t id, w::Status s, w::Bytes response) {
    std::lock_guard lock(mutex_);
    if (g != state_.generation) return err(w::Error::StaleSession);
    if (response.size() > kPayload || s.detail.size() > 1024) return err(w::Error::InvalidArgument);
    auto it = reverse_.find(id); if (it == reverse_.end()) return err(w::Error::NotFound);
    if (!has(taken_reverse_, id)) return err(w::Error::InvalidState);
    if (w::Clock::now() >= it->second.deadline) { s = err(w::Error::Timeout); response.clear(); }
    reverse_done_.push_back({g, id, std::move(s), std::move(response)}); reverse_.erase(it); std::erase(taken_reverse_, id); return {};
}
std::optional<w::Completion> WirelessEndpoint::pop_reverse_completion() {
    std::lock_guard lock(mutex_); if (reverse_done_.empty()) return {};
    auto r = std::move(reverse_done_.front()); reverse_done_.pop_front(); return r;
}
w::Status WirelessEndpoint::post_microphone(std::uint64_t g, w::MediaPacket p) {
    std::lock_guard lock(mutex_);
    if (g != state_.generation || p.epoch != g) return err(w::Error::StaleSession);
    if (!state_.connected) return err(w::Error::Disconnected);
    if (!state_.reverse_capabilities.microphone) return err(w::Error::NotSupported);
    auto it = std::find_if(state_.streams.begin(), state_.streams.end(), [&](const auto& s) { auto a = std::get_if<w::AudioConfiguration>(&s); return a && a->id == p.stream_id && a->microphone; });
    if (it == state_.streams.end()) return err(w::Error::NotFound);
    if (p.data.empty() || p.data.size() > 64 * 1024) return err(w::Error::InvalidArgument);
    const auto& format = std::get<w::AudioConfiguration>(*it).format;
    if (!w::wire_audio_format(format) || (format.codec == w::AudioCodec::Pcm && p.data.size() % (format.channels * (format.bits / 8)))) return err(w::Error::InvalidArgument);
    if (mic_.size() >= 64 || p.data.size() > 256 * 1024 - mic_bytes_) return err(w::Error::Backpressure);
    mic_bytes_ += p.data.size(); mic_.push_back(std::move(p)); return {};
}
std::optional<w::MediaPacket> WirelessEndpoint::take_microphone(std::uint64_t g) {
    std::lock_guard lock(mutex_);
    if (g != state_.generation || mic_.empty()) return {};
    auto p = std::move(mic_.front()); mic_.pop_front(); mic_bytes_ -= p.data.size(); return p;
}
void WirelessEndpoint::cancel_requests(w::Status reason) {
    std::lock_guard lock(mutex_);
    if (reason.detail.size() > 1024) reason.detail.resize(1024);
    if (reason) reason = err(w::Error::Cancelled);
    for (const auto& [id, p] : phones_) phone_done_.push_back({p.generation, id, reason, {}});
    for (const auto& [id, p] : reverse_) reverse_done_.push_back({p.generation, id, reason, {}});
    phones_.clear(); reverse_.clear(); taken_phones_.clear(); taken_reverse_.clear();
}
void WirelessEndpoint::expire(w::TimePoint now) {
    std::lock_guard lock(mutex_);
    for (auto it = phones_.begin(); it != phones_.end();) {
        if (it->second.deadline > now) { ++it; continue; }
        phone_done_.push_back({it->second.generation, it->first, err(w::Error::Timeout), {}}); std::erase(taken_phones_, it->first); it = phones_.erase(it);
    }
    for (auto it = reverse_.begin(); it != reverse_.end();) {
        if (it->second.deadline > now) { ++it; continue; }
        reverse_done_.push_back({it->second.generation, it->first, err(w::Error::Timeout), {}}); std::erase(taken_reverse_, it->first); it = reverse_.erase(it);
    }
}
} // namespace zero2w::forward
