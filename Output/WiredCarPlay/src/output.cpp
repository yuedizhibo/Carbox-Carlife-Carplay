#include "wired_carplay/output.hpp"
#include "metadata_validation.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace zero2w::wired {
namespace {
Status error(Error code, std::string detail = {}) { return {code, std::move(detail)}; }
template<class T> bool contains(const std::vector<T>& list, const T& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}
bool connected(Phase phase) { return phase == Phase::Ready || phase == Phase::Streaming; }
bool bounded(const std::string& s) { return s.size() <= 1024; }
bool valid_entity(Entity e) { return e == Entity::None || e == Entity::Device || e == Entity::Accessory; }
bool valid_modes(const ModeState& m) {
    return valid_entity(m.screen.current) && valid_entity(m.screen.permanent) &&
        valid_entity(m.main_audio.current) && valid_entity(m.main_audio.permanent) &&
        valid_entity(m.phone_call) && valid_entity(m.navigation) && valid_entity(m.speech) &&
        (m.speech_mode == SpeechMode::None || m.speech_mode == SpeechMode::Speaking || m.speech_mode == SpeechMode::Recognizing);
}
bool valid_audio(const AudioFormat& f) {
    if (f.channels != 1 && f.channels != 2) return false;
    if (f.bits != 16 && f.bits != 24) return false;
    switch (f.codec) {
    case AudioCodec::Pcm:
        return ((f.sample_rate == 8000 || f.sample_rate == 16000 || f.sample_rate == 24000 || f.sample_rate == 32000) && f.bits == 16) || f.sample_rate == 44100 || f.sample_rate == 48000;
    case AudioCodec::Alac: return f.channels == 2 && (f.sample_rate == 44100 || f.sample_rate == 48000);
    case AudioCodec::AacLc: return f.channels == 2 && f.bits == 16 && (f.sample_rate == 44100 || f.sample_rate == 48000);
    case AudioCodec::AacEld: return f.bits == 16 && ((f.channels == 1 && (f.sample_rate == 16000 || f.sample_rate == 24000)) || f.sample_rate == 44100 || f.sample_rate == 48000);
    case AudioCodec::Opus: return f.bits == 16 && f.channels == 1 && (f.sample_rate == 16000 || f.sample_rate == 24000 || f.sample_rate == 48000);
    }
    return false;
}
bool valid_control(const ControlCommand& c, std::size_t max) {
    if (!describe(c.type)) return false;
    switch (c.type) {
    case CommandType::DuckAudio: case CommandType::UnduckAudio: {
        auto p = std::get_if<DuckAudio>(&c.payload);
        return p && std::isfinite(p->duration_ms) && p->duration_ms >= 0 && p->duration_ms <= 60000 &&
            std::isfinite(p->volume_db) && p->volume_db >= -144 && p->volume_db <= 0;
    }
    case CommandType::DisableBluetooth: {
        auto p = std::get_if<BluetoothDevice>(&c.payload);
        if (!p || p->address.size() != 17) return false;
        for (std::size_t i = 0; i < 17; ++i) {
            char ch = p->address[i];
            if (i % 3 == 2 ? ch != ':' : !((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) return false;
        }
        return true;
    }
    case CommandType::ModesChanged: {
        auto p = std::get_if<ModeState>(&c.payload); return p && valid_modes(*p);
    }
    case CommandType::ChangeModes: {
        auto p = std::get_if<BinaryPlist>(&c.payload); return p && !p->data.empty() && p->data.size() <= max;
    }
    case CommandType::ForceKeyFrame: return std::holds_alternative<Empty>(c.payload);
    case CommandType::HidSendReport: {
        auto p = std::get_if<HidReport>(&c.payload);
        return p && !p->uuid.empty() && bounded(p->uuid) && !p->report.empty() && p->report.size() <= max;
    }
    case CommandType::HidSetInputMode: {
        auto p = std::get_if<SetHidInputMode>(&c.payload);
        return p && !p->uuid.empty() && bounded(p->uuid) && static_cast<unsigned>(p->mode) <= 4;
    }
    case CommandType::RequestSiri: {
        auto p = std::get_if<SiriRequest>(&c.payload);
        return p && static_cast<unsigned>(p->action) >= 1 && static_cast<unsigned>(p->action) <= 3;
    }
    case CommandType::RequestUI: {
        auto p = std::get_if<UiRequest>(&c.payload); return p && (!p->url || bounded(*p->url));
    }
    case CommandType::SetNightMode: case CommandType::SetLimitedUI: return std::holds_alternative<Toggle>(c.payload);
    case CommandType::IApSendMessage: {
        auto p = std::get_if<Bytes>(&c.payload); return p && !p->empty() && p->size() <= max;
    }
    }
    return false;
}
bool mutation(const Operation& op) {
    if (auto c = std::get_if<SendControl>(&op); c && c->command.type == CommandType::ModesChanged) return true;
    return std::holds_alternative<ScreenConfiguration>(op) || std::holds_alternative<AudioConfiguration>(op) ||
        std::holds_alternative<TeardownStream>(op) || std::holds_alternative<Record>(op) ||
        std::holds_alternative<DrainTeardown>(op) || std::holds_alternative<PairingRequest>(op) || std::holds_alternative<AssertModes>(op);
}
bool valid_peer(const PeerInfo& p, std::size_t max) {
    if (p.device_id.empty() || !valid_modes(p.modes)) return false;
    std::size_t total = p.raw_info.data.size();
    for (const auto* s : {&p.device_id, &p.name, &p.manufacturer, &p.model, &p.firmware, &p.source_version, &p.network_interface, &p.peer_address}) {
        if (!bounded(*s)) return false;
        total += s->size();
    }
    if (p.displays.size() > 16 || p.hid_devices.size() > 64) return false;
    for (const auto* v : {&p.controller_features, &p.extended_features, &p.bluetooth_ids}) {
        if (v->size() > 128) return false;
        for (const auto& s : *v) { if (!bounded(s)) return false; total += s.size(); }
    }
    for (const auto& d : p.displays) {
        if (d.uuid.empty() || !bounded(d.uuid) || !d.width || !d.height || !d.max_fps) return false;
        total += d.uuid.size() + d.edid.size();
    }
    for (const auto& h : p.hid_devices) {
        if (!bounded(h.uuid) || !bounded(h.display_uuid) || !bounded(h.name)) return false;
        total += h.uuid.size() + h.display_uuid.size() + h.name.size() + h.descriptor.size();
    }
    const auto& c = p.negotiated;
    if (c.commands.size() > 13 || c.csm_messages.size() > 256 || c.services.size() > 64 || c.video_codecs.size() > 4 ||
        c.audio_formats.size() > 64 || c.audio_streams.size() > 16 || c.display_roles.size() > 3) return false;
    for (auto t : c.commands) if (!describe(t)) return false;
    for (auto t : c.csm_messages) if (!describe(t)) return false;
    for (auto t : c.services) if (static_cast<unsigned>(t) > static_cast<unsigned>(Service::WifiConfiguration)) return false;
    for (auto t : c.video_codecs) if (static_cast<unsigned>(t) > 3) return false;
    for (auto t : c.display_roles) if (static_cast<unsigned>(t) > 2) return false;
    for (const auto& f : c.audio_formats) if (!valid_audio(f)) return false;
    for (auto s : c.audio_streams) {
        if (s != AudioStream::General && s != AudioStream::Main && s != AudioStream::Alternate && s != AudioStream::MainHigh &&
            s != AudioStream::Buffered && s != AudioStream::AuxiliaryOutput && s != AudioStream::AuxiliaryInput) return false;
    }
    return total <= max;
}
}

std::optional<AudioFormat> audio_format_from_wire(std::uint64_t bit) {
    unsigned n = 0;
    if (!bit || (bit & (bit - 1))) return {};
    while ((bit >>= 1) != 0) ++n;
    if (n < 2 || n > 32) return {};
    if (n <= 17) {
        constexpr std::uint32_t rates[] = {8000, 16000, 24000, 32000, 44100, 44100, 48000, 48000};
        return AudioFormat{AudioCodec::Pcm, rates[(n - 2) / 2], static_cast<std::uint8_t>((n == 12 || n == 13 || n == 16 || n == 17) ? 24 : 16), static_cast<std::uint8_t>((n % 2) + 1)};
    }
    if (n <= 21) return AudioFormat{AudioCodec::Alac, n < 20 ? 44100u : 48000u, static_cast<std::uint8_t>((n % 2) ? 24 : 16), 2};
    if (n <= 23) return AudioFormat{AudioCodec::AacLc, n == 22 ? 44100u : 48000u, 16, 2};
    if (n <= 25) return AudioFormat{AudioCodec::AacEld, n == 24 ? 44100u : 48000u, 16, 2};
    if (n <= 27) return AudioFormat{AudioCodec::AacEld, n == 26 ? 16000u : 24000u, 16, 1};
    if (n <= 30) return AudioFormat{AudioCodec::Opus, n == 28 ? 16000u : (n == 29 ? 24000u : 48000u), 16, 1};
    return AudioFormat{AudioCodec::AacEld, n == 31 ? 44100u : 48000u, 16, 1};
}
bool validate_control_command(const ControlCommand& c, std::size_t max) { return valid_control(c, max); }
bool validate_service_message(const ServiceMessage& m, std::size_t max) { return detail::valid_service(m, max); }
std::optional<std::uint64_t> wire_audio_format(const AudioFormat& f) {
    for (unsigned bit = 2; bit <= 32; ++bit) {
        const auto value = std::uint64_t{1} << bit;
        if (audio_format_from_wire(value) == f) return value;
    }
    return {};
}

struct WiredCarPlayOutput::Impl {
    explicit Impl(Limits value) : limits(value) {}
    mutable std::mutex mutex;
    Limits limits;
    Epoch epoch{};
    RequestId next_id{1};
    Phase phase{Phase::Idle};
    Status last_error;
    struct Pending { WorkItem work; bool taken{}; };
    struct Inbound { IncomingControl event; std::optional<ControlReply> reply; };
    std::map<RequestId, Pending> pending;
    std::deque<Completion> completions;
    std::map<RequestId, Inbound> incoming;
    std::deque<IncomingEvent> events;
    std::deque<MediaPacket> media, microphones;
    std::size_t media_bytes{}, microphone_bytes{};
    std::map<StreamId, std::variant<ScreenConfiguration, AudioConfiguration>> streams;
    std::optional<PeerInfo> peer;
    std::optional<MediaClock> clock;
    std::uint64_t rejected{}, transmitted{}, received_mic{};

    Ticket reject(Error e, std::string detail = {}) { ++rejected; return {error(e, std::move(detail)), epoch, 0}; }
    void clear_session() {
        peer.reset(); clock.reset(); streams.clear(); media.clear(); microphones.clear();
        media_bytes = microphone_bytes = 0; incoming.clear(); events.clear();
    }
    void finish_all(Status reason) {
        for (auto& [id, p] : pending) completions.push_back({epoch, id, reason, {}});
        pending.clear();
    }
    void fail(Status reason) {
        last_error = reason; phase = Phase::Failed; finish_all(reason); clear_session();
    }
    void expire_pending(TimePoint now) {
        for (auto it = pending.begin(); it != pending.end();) {
            if (now < it->second.work.deadline) { ++it; continue; }
            const auto& op = it->second.work.operation;
            if (std::holds_alternative<StartSession>(op) || std::holds_alternative<StopSession>(op) || (it->second.taken && mutation(op))) {
                fail(error(Error::Timeout, "state change timed out; backend must close transport")); return;
            }
            completions.push_back({epoch, it->first, error(Error::Timeout), {}}); it = pending.erase(it);
        }
    }
    bool mutating() const {
        for (const auto& [id, p] : pending) if (mutation(p.work.operation)) return true;
        return false;
    }
    Status validate(const Operation& op) const {
        if (!connected(phase) || !peer) return error(Error::InvalidState);
        const auto& cap = peer->negotiated;
        if (mutation(op) && mutating()) return error(Error::InvalidState, "stream/pairing mutation already pending");
        if (auto p = std::get_if<ScreenConfiguration>(&op)) {
            if (!p->id || p->display_uuid.empty() || !bounded(p->display_uuid) || !p->width || !p->height || !p->fps || p->latency_ms > 60000 || p->codec_configuration.size() > limits.max_payload_bytes) return error(Error::InvalidArgument);
            if (streams.contains(p->id)) return error(Error::InvalidState);
            if (streams.size() >= limits.max_streams) return error(Error::Backpressure);
            if (!contains(cap.video_codecs, p->codec) || !contains(cap.display_roles, p->role)) return error(Error::NotSupported);
            auto d = std::find_if(peer->displays.begin(), peer->displays.end(), [&](const auto& v) { return v.uuid == p->display_uuid; });
            if (d == peer->displays.end() || p->width > d->width || p->height > d->height || p->fps > d->max_fps) return error(Error::NotSupported);
        } else if (auto p = std::get_if<AudioConfiguration>(&op)) {
            if (!p->id || !valid_audio(p->format) || (!p->playback && !p->microphone) || p->latency_ms > 60000 || static_cast<unsigned>(p->use) > 5) return error(Error::InvalidArgument);
            if (streams.contains(p->id)) return error(Error::InvalidState);
            if (streams.size() >= limits.max_streams) return error(Error::Backpressure);
            if (!contains(cap.audio_formats, p->format) || !contains(cap.audio_streams, p->stream) || (p->microphone && !cap.microphone)) return error(Error::NotSupported);
        } else if (auto p = std::get_if<TeardownStream>(&op)) {
            if (!streams.contains(p->id)) return error(Error::NotFound);
        } else if (std::holds_alternative<Record>(op)) {
            if (streams.empty() || phase == Phase::Streaming) return error(Error::InvalidState);
        } else if (auto p = std::get_if<AssertModes>(&op)) {
            if (!valid_modes(p->state)) return error(Error::InvalidArgument);
            if (!contains(cap.commands, CommandType::ModesChanged)) return error(Error::NotSupported);
        } else if (auto p = std::get_if<SendControl>(&op)) {
            if (!valid_control(p->command, limits.max_payload_bytes)) return error(Error::InvalidArgument);
            if (describe(p->command.type)->direction == Direction::AccessoryToDevice) return error(Error::InvalidArgument, "vehicle-origin command cannot be sent by device");
            if (!contains(cap.commands, p->command.type)) return error(Error::NotSupported);
        } else if (auto p = std::get_if<SendIap2>(&op)) {
            if (!describe(p->message.type) || p->message.parameters.size() > limits.max_payload_bytes) return error(Error::InvalidArgument);
            if (!contains(cap.csm_messages, p->message.type)) return error(Error::NotSupported);
        } else if (auto p = std::get_if<SendService>(&op)) {
            if (!detail::valid_service(p->message, limits.max_payload_bytes)) return error(Error::InvalidArgument);
            if (!contains(cap.services, p->message.service)) return error(Error::NotSupported);
        } else if (auto p = std::get_if<PairingRequest>(&op)) {
            if (p->peer_id.empty() || !bounded(p->peer_id) || static_cast<unsigned>(p->action) > 3) return error(Error::InvalidArgument);
            if (!cap.pairing_management) return error(Error::NotSupported);
        }
        return {};
    }
    Ticket enqueue(Operation op, Milliseconds timeout) {
        expire_pending(Clock::now());
        if (timeout.count() <= 0 || timeout > Milliseconds{300000}) return reject(Error::InvalidArgument);
        const bool stop = std::holds_alternative<StopSession>(op);
        if (pending.size() + completions.size() >= limits.pending_requests + (stop ? 1u : 0u)) return reject(Error::Backpressure);
        if (auto p = std::get_if<StartSession>(&op)) {
            if (phase != Phase::Idle && phase != Phase::Closed && phase != Phase::Failed) return reject(Error::InvalidState);
            const auto& u = p->usb;
            if (u.udc.empty() || u.serial.empty() || u.manufacturer.empty() || u.product.empty() || !u.vendor_id || !u.product_id ||
                !bounded(u.udc) || !bounded(u.serial) || !bounded(u.manufacturer) || !bounded(u.product) || !bounded(p->pairing_store) || static_cast<unsigned>(u.role) > 2) return reject(Error::InvalidArgument);
            clear_session(); ++epoch; phase = Phase::Starting; last_error = {};
        } else if (stop) {
            if (phase == Phase::Idle || phase == Phase::Closed || phase == Phase::Stopping) return reject(Error::InvalidState);
            finish_all(error(Error::Cancelled)); clear_session(); phase = Phase::Stopping;
        } else {
            auto s = validate(op); if (!s) return reject(s.code, std::move(s.detail));
        }
        auto id = next_id++;
        pending.emplace(id, Pending{WorkItem{epoch, id, std::move(op), Clock::now() + timeout}, false});
        return {{}, epoch, id};
    }
    Status packet_valid(const MediaPacket& p, bool mic) const {
        if (p.epoch != epoch) return error(Error::StaleSession);
        if (phase != Phase::Streaming) return error(Error::InvalidState);
        if (p.timebase != MediaPacket::Timebase::StreamClock && p.timebase != MediaPacket::Timebase::MonotonicMicroseconds) return error(Error::InvalidArgument);
        if (p.timebase == MediaPacket::Timebase::MonotonicMicroseconds && !peer->negotiated.monotonic_media_timestamps) return error(Error::NotSupported);
        if (p.data.empty() || p.data.size() > limits.max_payload_bytes) return error(Error::InvalidArgument);
        auto stream = streams.find(p.stream_id);
        if (stream == streams.end()) return error(Error::NotFound);
        for (const auto& [id, r] : pending) {
            if (auto t = std::get_if<TeardownStream>(&r.work.operation); t && t->id == p.stream_id) return error(Error::InvalidState);
        }
        if (auto a = std::get_if<AudioConfiguration>(&stream->second)) {
            if (mic ? !a->microphone : !a->playback) return error(Error::NotSupported);
            if (a->format.codec == AudioCodec::Pcm && p.data.size() % (a->format.channels * (a->format.bits / 8)) != 0) return error(Error::InvalidArgument);
        } else if (mic) return error(Error::InvalidArgument);
        return {};
    }
    void remove_stream(StreamId id) {
        streams.erase(id);
        std::erase_if(media, [&](const auto& p) { if (p.stream_id != id) return false; media_bytes -= p.data.size(); return true; });
        std::erase_if(microphones, [&](const auto& p) { if (p.stream_id != id) return false; microphone_bytes -= p.data.size(); return true; });
        if (streams.empty()) phase = Phase::Ready;
    }
    void remove_event(RequestId id) {
        std::erase_if(events, [id](const auto& e) { auto p = std::get_if<IncomingControl>(&e.payload); return p && p->id == id; });
    }
};

WiredCarPlayOutput::WiredCarPlayOutput(Limits l) {
    if (!l.pending_requests || l.pending_requests > 4096 || !l.inbound_requests || l.inbound_requests > 4096 ||
        !l.events || l.events > 4096 || !l.media_packets || l.media_packets > 4096 || !l.media_bytes || l.media_bytes > 64 * 1024 * 1024 ||
        !l.microphone_packets || l.microphone_packets > 4096 || !l.microphone_bytes || l.microphone_bytes > 16 * 1024 * 1024 ||
        !l.max_payload_bytes || l.max_payload_bytes > 16 * 1024 * 1024 || !l.max_streams || l.max_streams > 64) throw std::invalid_argument("invalid WiredCarPlay queue limits");
    impl_ = std::make_unique<Impl>(l);
}
WiredCarPlayOutput::~WiredCarPlayOutput() = default;
Ticket WiredCarPlayOutput::start(StartSession c, Milliseconds t) { return submit(std::move(c), t); }
Ticket WiredCarPlayOutput::shutdown(Milliseconds t) { return submit(StopSession{}, t); }
Ticket WiredCarPlayOutput::submit(Operation op, Milliseconds t) {
    std::lock_guard lock(impl_->mutex); return impl_->enqueue(std::move(op), t);
}
Status WiredCarPlayOutput::cancel(Epoch e, RequestId id) {
    std::lock_guard lock(impl_->mutex); auto& i = *impl_;
    if (e != i.epoch) return error(Error::StaleSession);
    auto it = i.pending.find(id); if (it == i.pending.end()) return error(Error::NotFound);
    if (std::holds_alternative<StopSession>(it->second.work.operation) || (it->second.taken && (mutation(it->second.work.operation) || std::holds_alternative<StartSession>(it->second.work.operation)))) {
        i.fail(error(Error::Cancelled, "in-flight state change cancelled; backend must close transport")); return {};
    }
    bool lifecycle = std::holds_alternative<StartSession>(it->second.work.operation) || std::holds_alternative<StopSession>(it->second.work.operation);
    i.completions.push_back({e, id, error(Error::Cancelled), {}}); i.pending.erase(it);
    if (lifecycle) { i.clear_session(); i.phase = Phase::Closed; }
    return {};
}
std::optional<Completion> WiredCarPlayOutput::pop_completion() {
    std::lock_guard lock(impl_->mutex); auto& q = impl_->completions;
    if (q.empty()) return {};
    auto result = std::move(q.front()); q.pop_front(); return result;
}
Snapshot WiredCarPlayOutput::snapshot() const {
    std::lock_guard lock(impl_->mutex); const auto& i = *impl_;
    return {i.epoch, i.phase, i.last_error, i.pending.size(), i.completions.size(), i.incoming.size(), i.events.size(),
        i.media.size(), i.media_bytes, i.microphones.size(), i.microphone_bytes, i.streams.size(), i.rejected, i.transmitted, i.received_mic};
}
std::optional<Completion> WiredCarPlayOutput::pop_completion(Epoch e, RequestId id) {
    std::lock_guard lock(impl_->mutex);
    auto& q = impl_->completions;
    auto it = std::find_if(q.begin(), q.end(), [&](const auto& c) { return c.epoch == e && c.id == id; });
    if (it == q.end()) return {};
    auto result = std::move(*it); q.erase(it); return result;
}
Status WiredCarPlayOutput::discard_media(Epoch e) {
    std::lock_guard lock(impl_->mutex);
    if (e != impl_->epoch) return error(Error::StaleSession);
    impl_->media.clear(); impl_->microphones.clear(); impl_->media_bytes = impl_->microphone_bytes = 0;
    return {};
}
std::optional<PeerInfo> WiredCarPlayOutput::info_cached() const { std::lock_guard lock(impl_->mutex); return impl_->peer; }
std::optional<MediaClock> WiredCarPlayOutput::media_clock() const { std::lock_guard lock(impl_->mutex); return impl_->clock; }
std::optional<WorkItem> WiredCarPlayOutput::take_work() {
    std::lock_guard lock(impl_->mutex);
    impl_->expire_pending(Clock::now());
    for (auto& [id, p] : impl_->pending) if (!p.taken) { p.taken = true; return p.work; }
    return {};
}
Status WiredCarPlayOutput::complete(Epoch e, RequestId id, Status result, Bytes response, std::optional<PeerInfo> peer) {
    std::lock_guard lock(impl_->mutex); auto& i = *impl_;
    if (e != i.epoch) return error(Error::StaleSession);
    if (response.size() > i.limits.max_payload_bytes || !bounded(result.detail)) return error(Error::InvalidArgument);
    i.expire_pending(Clock::now());
    auto it = i.pending.find(id); if (it == i.pending.end()) return error(Error::NotFound);
    if (!it->second.taken) return error(Error::InvalidState, "backend has not taken this operation");
    const auto& op = it->second.work.operation;
    if (std::holds_alternative<StartSession>(op)) {
        if (result) {
            if (!peer || !valid_peer(*peer, i.limits.max_payload_bytes)) return error(Error::InvalidArgument, "successful bootstrap requires bounded negotiated peer information");
            i.peer = std::move(peer); i.phase = Phase::Ready;
        } else { i.last_error = result; i.phase = Phase::Failed; i.clear_session(); }
    } else {
        if (peer) return error(Error::InvalidArgument);
        if (std::holds_alternative<StopSession>(op)) {
            i.clear_session(); i.phase = result ? Phase::Closed : Phase::Failed;
        } else if (result) {
            if (auto p = std::get_if<ScreenConfiguration>(&op)) i.streams.emplace(p->id, *p);
            else if (auto p = std::get_if<AudioConfiguration>(&op)) i.streams.emplace(p->id, *p);
            else if (auto p = std::get_if<TeardownStream>(&op)) i.remove_stream(p->id);
            else if (std::holds_alternative<Record>(op)) i.phase = Phase::Streaming;
            else if (auto p = std::get_if<AssertModes>(&op)) i.peer->modes = p->state;
            else if (auto p = std::get_if<SendControl>(&op); p && p->command.type == CommandType::ModesChanged) i.peer->modes = std::get<ModeState>(p->command.payload);
        }
    }
    if (!result) i.last_error = result;
    i.completions.push_back({e, id, std::move(result), std::move(response)}); i.pending.erase(it); return {};
}
Status WiredCarPlayOutput::report_progress(Epoch e, Phase phase) {
    std::lock_guard lock(impl_->mutex); auto& i = *impl_;
    if (e != i.epoch) return error(Error::StaleSession);
    bool starting = false;
    for (const auto& [id, p] : i.pending) if (p.taken && std::holds_alternative<StartSession>(p.work.operation)) starting = true;
    if (!starting || phase < Phase::Starting || phase >= Phase::Ready) return error(Error::InvalidState);
    i.phase = phase; return {}; // Bootstrap retries may legitimately revisit earlier phases.
}
Status WiredCarPlayOutput::disconnect(Epoch e, Status reason) {
    std::lock_guard lock(impl_->mutex); auto& i = *impl_;
    if (e != i.epoch) return error(Error::StaleSession);
    if (reason || !bounded(reason.detail)) return error(Error::InvalidArgument);
    i.fail(std::move(reason)); return {};
}
Status WiredCarPlayOutput::update_clock(Epoch e, MediaClock c) {
    std::lock_guard lock(impl_->mutex); auto& i = *impl_;
    if (e != i.epoch) return error(Error::StaleSession);
    if (!connected(i.phase)) return error(Error::InvalidState);
    if (!c.sample_rate || c.sample_rate > 384000 || (i.clock && c.measured_at < i.clock->measured_at)) return error(Error::InvalidArgument);
    i.clock = c; return {};
}
Status WiredCarPlayOutput::push_media(MediaPacket p) {
    std::lock_guard lock(impl_->mutex); auto& i = *impl_;
    auto status = i.packet_valid(p, false); if (!status) { ++i.rejected; return status; }
    if (i.media.size() >= i.limits.media_packets || p.data.size() > i.limits.media_bytes - i.media_bytes) { ++i.rejected; return error(Error::Backpressure); }
    i.media_bytes += p.data.size(); i.media.push_back(std::move(p)); return {};
}
std::optional<MediaPacket> WiredCarPlayOutput::take_media() {
    std::lock_guard lock(impl_->mutex); auto& i = *impl_;
    i.expire_pending(Clock::now());
    if (i.media.empty()) return {};
    auto p = std::move(i.media.front()); i.media.pop_front(); i.media_bytes -= p.data.size(); ++i.transmitted; return p;
}
Status WiredCarPlayOutput::receive_microphone(MediaPacket p) {
    std::lock_guard lock(impl_->mutex); auto& i = *impl_;
    auto status = i.packet_valid(p, true); if (!status) { ++i.rejected; return status; }
    if (i.microphones.size() >= i.limits.microphone_packets || p.data.size() > i.limits.microphone_bytes - i.microphone_bytes) { ++i.rejected; return error(Error::Backpressure); }
    i.microphone_bytes += p.data.size(); ++i.received_mic; i.microphones.push_back(std::move(p)); return {};
}
std::optional<MediaPacket> WiredCarPlayOutput::pop_microphone() {
    std::lock_guard lock(impl_->mutex); auto& i = *impl_;
    if (i.microphones.empty()) return {};
    auto p = std::move(i.microphones.front()); i.microphones.pop_front(); i.microphone_bytes -= p.data.size(); return p;
}
Ticket WiredCarPlayOutput::receive_control(Epoch e, ControlCommand c, Milliseconds timeout) {
    std::lock_guard lock(impl_->mutex); auto& i = *impl_;
    if (e != i.epoch) return i.reject(Error::StaleSession);
    if (!connected(i.phase) || !i.peer) return i.reject(Error::InvalidState);
    if (!valid_control(c, i.limits.max_payload_bytes) || timeout.count() <= 0 || timeout > Milliseconds{300000}) return i.reject(Error::InvalidArgument);
    if (describe(c.type)->direction == Direction::DeviceToAccessory) return i.reject(Error::InvalidArgument);
    if (!contains(i.peer->negotiated.commands, c.type)) return i.reject(Error::NotSupported);
    if (i.incoming.size() >= i.limits.inbound_requests || i.events.size() >= i.limits.events) return i.reject(Error::Backpressure);
    const auto id = i.next_id++;
    IncomingControl event{e, id, std::move(c), Clock::now() + timeout};
    i.events.push_back({e, event}); i.incoming.emplace(id, Impl::Inbound{std::move(event), {}});
    return {{}, e, id};
}
Status WiredCarPlayOutput::receive_iap2(Epoch e, Iap2Message message) {
    std::lock_guard lock(impl_->mutex); auto& i = *impl_;
    if (e != i.epoch) return error(Error::StaleSession);
    if (!connected(i.phase) || !i.peer) return error(Error::InvalidState);
    if (!describe(message.type) || message.parameters.size() > i.limits.max_payload_bytes) return error(Error::InvalidArgument);
    if (!contains(i.peer->negotiated.csm_messages, message.type)) return error(Error::NotSupported);
    if (i.events.size() >= i.limits.events) return error(Error::Backpressure);
    i.events.push_back({e, std::move(message)}); return {};
}
Status WiredCarPlayOutput::receive_service(Epoch e, ServiceMessage message) {
    std::lock_guard lock(impl_->mutex); auto& i = *impl_;
    if (e != i.epoch) return error(Error::StaleSession);
    if (!connected(i.phase) || !i.peer) return error(Error::InvalidState);
    if (!detail::valid_service(message, i.limits.max_payload_bytes)) return error(Error::InvalidArgument);
    if (!contains(i.peer->negotiated.services, message.service)) return error(Error::NotSupported);
    if (i.events.size() >= i.limits.events) return error(Error::Backpressure);
    i.events.push_back({e, std::move(message)}); return {};
}
std::optional<IncomingEvent> WiredCarPlayOutput::pop_event() {
    std::lock_guard lock(impl_->mutex); auto& q = impl_->events;
    if (q.empty()) return {};
    auto e = std::move(q.front()); q.pop_front(); return e;
}
Status WiredCarPlayOutput::respond(Epoch e, RequestId id, Status status, Bytes response) {
    std::lock_guard lock(impl_->mutex); auto& i = *impl_;
    if (e != i.epoch) return error(Error::StaleSession);
    if (response.size() > i.limits.max_payload_bytes || !bounded(status.detail)) return error(Error::InvalidArgument);
    auto it = i.incoming.find(id); if (it == i.incoming.end()) return error(Error::NotFound);
    if (it->second.reply) return error(Error::InvalidState);
    if (Clock::now() >= it->second.event.deadline) return error(Error::Timeout);
    it->second.reply = ControlReply{e, id, std::move(status), std::move(response)};
    i.remove_event(id); return {};
}
std::optional<ControlReply> WiredCarPlayOutput::take_reply(TimePoint now) {
    std::lock_guard lock(impl_->mutex); auto& i = *impl_;
    for (auto it = i.incoming.begin(); it != i.incoming.end(); ++it) {
        if (!it->second.reply && now >= it->second.event.deadline) it->second.reply = ControlReply{i.epoch, it->first, error(Error::Timeout, "incoming command not handled before deadline"), {}};
        if (it->second.reply) {
            auto reply = std::move(it->second.reply); i.remove_event(it->first); i.incoming.erase(it); return reply;
        }
    }
    return {};
}
void WiredCarPlayOutput::expire(TimePoint now) {
    std::lock_guard lock(impl_->mutex); impl_->expire_pending(now);
}
} // namespace zero2w::wired
