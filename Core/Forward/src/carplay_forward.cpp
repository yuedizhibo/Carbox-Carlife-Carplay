#include "forward/carplay_forward.hpp"
#include <algorithm>
#include <stdexcept>

namespace zero2w::forward {
namespace {
w::StreamId id_of(const Stream& s) { return std::visit([](const auto& v) { return v.id; }, s); }
bool same(const Stream& a, const Stream& b) {
    if (a.index() != b.index()) return false;
    if (auto x = std::get_if<w::ScreenConfiguration>(&a)) {
        const auto& y = std::get<w::ScreenConfiguration>(b);
        return x->id == y.id && x->display_uuid == y.display_uuid && x->role == y.role && x->codec == y.codec && x->width == y.width && x->height == y.height && x->fps == y.fps && x->latency_ms == y.latency_ms && x->codec_configuration == y.codec_configuration;
    }
    const auto& x = std::get<w::AudioConfiguration>(a); const auto& y = std::get<w::AudioConfiguration>(b);
    return x.id == y.id && x.stream == y.stream && x.use == y.use && x.format == y.format && x.latency_ms == y.latency_ms && x.playback == y.playback && x.microphone == y.microphone;
}
}
struct CarPlayForwarder::Impl {
    mvp::SessionCore& core;
    w::WiredCarPlayOutput& out;
    std::shared_ptr<WirelessEndpoint> input;
    std::string source;
    ForwardSnapshot stats;
    struct Active { Stream source; w::StreamId output_id{}; bool waiting_keyframe{true}; };
    std::map<w::StreamId, Active> active;
    enum class Job { Setup, Teardown, Record, Phone };
    struct Pending { w::Ticket ticket; Job job; w::StreamId source_id{}; std::optional<Active> stream; std::uint64_t phone_id{}, generation{}; };
    std::map<std::uint64_t, Pending> pending;
    std::map<std::uint64_t, w::RequestId> reverse;
    w::StreamId next_stream{0xF0000000};
    std::uint64_t vehicle_epoch{};
    std::optional<w::MediaPacket> held_media, held_mic;
    bool bound{}, draining{}, failed{}, enabled{true};

    Impl(mvp::SessionCore& c, w::WiredCarPlayOutput& o, std::shared_ptr<WirelessEndpoint> i, std::string s)
        : core(c), out(o), input(std::move(i)), source(std::move(s)) {}
    void error(w::Status s) { if (!s) stats.last_error = std::move(s); }
    bool state_pending() const { for (const auto& [id, p] : pending) if (p.job != Job::Phone) return true; return false; }
    void drain() {
        if (draining) return;
        draining = true; failed = false; held_media.reset(); held_mic.reset();
        out.discard_media(stats.output_epoch);
        if (input->state().generation == stats.generation) { input->discard_media(); input->cancel_requests({w::Error::Cancelled, "forward selection/session changed"}); }
        for (const auto& [id, p] : pending) { out.cancel(p.ticket.epoch, p.ticket.id); out.pop_completion(p.ticket.epoch, p.ticket.id); }
        pending.clear();
        for (const auto& [id, car_id] : reverse) if (car_id) out.respond(stats.output_epoch, car_id, {w::Error::Cancelled, "forward detached"});
        reverse.clear(); stats.phase = Phase::Draining;
    }
    void collect() {
        for (auto it = pending.begin(); it != pending.end();) {
            auto done = out.pop_completion(it->second.ticket.epoch, it->first);
            if (!done) { ++it; continue; }
            auto p = std::move(it->second); it = pending.erase(it);
            if (p.job == Job::Phone) input->complete_phone(p.generation, p.phone_id, done->status, std::move(done->response));
            else if (!done->status) { error(done->status); failed = true; }
            else if (p.job == Job::Setup) active[p.source_id] = std::move(*p.stream);
            else if (p.job == Job::Teardown) active.erase(p.source_id);
        }
        while (auto done = input->pop_reverse_completion()) {
            auto it = reverse.find(done->id); if (it == reverse.end()) continue;
            if (done->epoch == stats.generation && it->second) out.respond(stats.output_epoch, it->second, done->status, std::move(done->response));
            error(done->status); reverse.erase(it);
        }
    }
    bool teardown(w::StreamId source_id) {
        auto t = out.submit(w::TeardownStream{active.at(source_id).output_id});
        if (!t.status) { error(t.status); if (t.status.code != w::Error::Backpressure && t.status.code != w::Error::InvalidState) failed = true; return false; }
        pending.emplace(t.id, Pending{t, Job::Teardown, source_id, {}, 0, stats.generation}); return true;
    }
    bool setup(const Stream& s, const InputState& in) {
        Active a{s, ++next_stream, true};
        auto t = std::visit([&](const auto& original) -> w::Ticket {
            auto config = original; config.id = a.output_id;
            using T = std::decay_t<decltype(config)>;
            if constexpr (std::is_same_v<T, w::ScreenConfiguration>) {
                if (config.display_uuid.empty()) {
                    auto peer = out.info_cached(); const std::size_t n = config.role == w::DisplayRole::Main ? 0 : 1;
                    if (!peer || n >= peer->displays.size()) return {{w::Error::NotSupported, "output display unavailable"}, stats.output_epoch, 0};
                    config.display_uuid = peer->displays[n].uuid;
                }
            } else if (config.microphone && !in.reverse_capabilities.microphone) return {{w::Error::NotSupported, "wireless input has no real microphone return backend"}, stats.output_epoch, 0};
            return out.submit(std::move(config));
        }, s);
        if (!t.status) { error(t.status); if (t.status.code != w::Error::Backpressure && t.status.code != w::Error::InvalidState) failed = true; return false; }
        pending.emplace(t.id, Pending{t, Job::Setup, id_of(s), std::move(a), 0, stats.generation}); return true;
    }
    void events(bool usable) {
        for (int n = 0; n < 16; ++n) {
            auto e = out.pop_event(); if (!e) break;
            auto c = std::get_if<w::IncomingControl>(&e->payload);
            if (!usable || e->epoch != stats.output_epoch) {
                if (c) out.respond(e->epoch, c->id, {w::Error::Disconnected, "no selected wireless CarPlay session"});
                continue;
            }
            ReversePayload payload = std::visit([](const auto& p) -> ReversePayload {
                using T = std::decay_t<decltype(p)>;
                if constexpr (std::is_same_v<T, w::IncomingControl>) return p.command;
                else return p;
            }, e->payload);
            auto t = input->post_reverse(stats.generation, std::move(payload), c ? c->deadline : w::Clock::now() + w::Milliseconds{3000});
            if (!t.status) { error(t.status); if (c) out.respond(stats.output_epoch, c->id, t.status); }
            else { reverse.emplace(t.id, c ? c->id : 0); ++stats.reverse_commands; }
        }
    }
    void phone_requests() {
        for (int n = 0; n < 16 && pending.size() < 32; ++n) {
            auto p = input->take_phone_request(); if (!p) break;
            if (p->generation != stats.generation) { input->complete_phone(p->generation, p->id, {w::Error::StaleSession, {}}); continue; }
            auto remaining = std::chrono::duration_cast<w::Milliseconds>(p->deadline - w::Clock::now());
            if (remaining.count() <= 0) { input->complete_phone(p->generation, p->id, {w::Error::Timeout, {}}); continue; }
            auto t = out.submit(std::move(p->operation), remaining);
            if (!t.status) { input->complete_phone(p->generation, p->id, t.status); error(t.status); }
            else pending.emplace(t.id, Pending{t, Job::Phone, 0, {}, p->id, p->generation});
        }
    }
    void media() {
        for (int n = 0; n < 32; ++n) {
            if (!held_media) held_media = input->take_media(stats.generation);
            if (!held_media) break;
            auto it = active.find(held_media->stream_id);
            if (it == active.end() || held_media->epoch != stats.generation) { ++stats.dropped_packets; held_media.reset(); continue; }
            auto& a = it->second;
            const bool video = std::holds_alternative<w::ScreenConfiguration>(a.source);
            if (video && a.waiting_keyframe && !held_media->key_frame) { ++stats.dropped_packets; held_media.reset(); continue; }
            auto packet = *held_media; packet.epoch = stats.output_epoch; packet.stream_id = a.output_id;
            auto s = out.push_media(std::move(packet));
            if (s.code == w::Error::Backpressure) break; // Retain exactly one packet and stop draining input.
            if (!s) { error(s); ++stats.dropped_packets; }
            else { if (video) a.waiting_keyframe = false; ++stats.forwarded_packets; stats.forwarded_bytes += held_media->data.size(); }
            held_media.reset();
        }
        for (int n = 0; n < 32; ++n) {
            if (!held_mic) held_mic = out.pop_microphone();
            if (!held_mic) break;
            if (held_mic->epoch != stats.output_epoch) { held_mic.reset(); continue; }
            auto it = std::find_if(active.begin(), active.end(), [&](const auto& p) { return p.second.output_id == held_mic->stream_id; });
            if (it == active.end()) { held_mic.reset(); ++stats.dropped_packets; continue; }
            auto p = *held_mic; p.stream_id = it->first; p.epoch = stats.generation;
            auto s = input->post_microphone(stats.generation, std::move(p));
            if (s.code == w::Error::Backpressure) break;
            if (s) ++stats.microphone_packets; else { error(s); ++stats.dropped_packets; }
            held_mic.reset();
        }
    }
    void tick() {
        input->expire(w::Clock::now()); out.expire(); collect();
        auto in = input->state(); auto os = out.snapshot(); auto cs = core.snapshot();
        const bool selected = enabled && std::string_view(cs.active.data()) == source && in.connected;
        const bool output_live = os.phase == w::Phase::Ready || os.phase == w::Phase::Streaming;
        if (output_live && vehicle_epoch != os.epoch) {
            if (auto info = out.info_cached()) { input->update_vehicle(VehicleContext{os.epoch, std::move(*info)}); vehicle_epoch = os.epoch; }
        } else if (!output_live && vehicle_epoch) { input->update_vehicle({}); vehicle_epoch = 0; }
        if (bound && (!selected || in.generation != stats.generation || os.epoch != stats.output_epoch || !output_live)) drain();
        if (draining) {
            events(false);
            if (os.epoch != stats.output_epoch || !output_live) { active.clear(); pending.clear(); }
            if (!active.empty()) {
                if (failed) { out.disconnect(stats.output_epoch, {w::Error::TransportFailure, "forward teardown failed"}); active.clear(); }
                else if (!state_pending()) teardown(active.begin()->first);
                return;
            }
            if (state_pending()) return;
            bound = draining = failed = false; stats.phase = Phase::Inactive;
        }
        if (!selected) { input->discard_media(); input->cancel_requests({w::Error::Cancelled, "wireless source is not selected"}); events(false); stats.phase = Phase::Inactive; return; }
        if (!output_live) { input->discard_media(); events(false); stats.phase = Phase::WaitingOutput; return; }
        if (!bound) {
            if (os.active_streams) { stats.phase = Phase::WaitingOutput; error({w::Error::InvalidState, "output streams owned by another consumer"}); return; }
            bound = true; stats.generation = in.generation; stats.session = in.session; stats.output_epoch = os.epoch; stats.last_error = {};
        }
        if (failed) { stats.phase = Phase::Failed; events(false); input->discard_media(); out.discard_media(stats.output_epoch); return; }
        events(true);
        if (state_pending()) { stats.phase = Phase::Configuring; return; }
        for (const auto& [id, a] : active) {
            auto it = std::find_if(in.streams.begin(), in.streams.end(), [id](const auto& s) { return id_of(s) == id; });
            if (it == in.streams.end() || !same(a.source, *it)) {
                held_media.reset(); held_mic.reset(); out.discard_media(stats.output_epoch);
                stats.phase = Phase::Configuring; teardown(id); return;
            }
        }
        for (const auto& s : in.streams) if (!active.contains(id_of(s))) { stats.phase = Phase::Configuring; setup(s, in); return; }
        if (!active.empty() && os.phase == w::Phase::Ready) {
            auto t = out.submit(w::Record{});
            if (t.status) pending.emplace(t.id, Pending{t, Job::Record, 0, {}, 0, stats.generation}); else error(t.status);
            stats.phase = Phase::Configuring; return;
        }
        stats.phase = active.empty() ? Phase::Configuring : Phase::Forwarding;
        phone_requests();
        if (stats.phase == Phase::Forwarding) media();
    }
};
CarPlayForwarder::CarPlayForwarder(mvp::SessionCore& c, w::WiredCarPlayOutput& o, std::shared_ptr<WirelessEndpoint> i, std::string source) {
    if (!i || source.empty() || source.size() >= 32) throw std::invalid_argument("invalid Forward input/source");
    impl_ = std::make_unique<Impl>(c, o, std::move(i), std::move(source));
}
CarPlayForwarder::~CarPlayForwarder() {
    stop();
    // A caller should pump stop() to completion. Destruction cannot wait for I/O;
    // invalidate an incompletely drained link so no orphaned stream keeps playing.
    if (!impl_->active.empty() || !impl_->pending.empty()) impl_->out.disconnect(impl_->stats.output_epoch, {w::Error::Cancelled, "Forward destroyed before stream teardown completed"});
}
void CarPlayForwarder::tick() { impl_->tick(); }
void CarPlayForwarder::start() { impl_->enabled = true; }
void CarPlayForwarder::stop() { impl_->enabled = false; if (impl_->bound || !impl_->active.empty()) impl_->drain(); }
ForwardSnapshot CarPlayForwarder::snapshot() const { auto s = impl_->stats; s.active_streams = impl_->active.size(); s.pending_controls = impl_->pending.size() + impl_->reverse.size(); return s; }
} // namespace zero2w::forward
