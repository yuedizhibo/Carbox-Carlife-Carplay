#include "forward/carplay_forward.hpp"
#include "forward/catplay_capture.hpp"
#include "forward/direct_connection.hpp"
#include "wirelesscarplay/catplay_media_client.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>
#if defined(__unix__)
#include <sys/socket.h>
#include <unistd.h>
#endif
namespace w = zero2w::wired;
namespace f = zero2w::forward;
namespace cp = mvp::catplay_media;
namespace mvp {
struct CatPlayApiTestAccess {
    static bool deliver(CatPlayMediaClient& c, const catplay_media::Header& h, std::span<const std::uint8_t> p) { return c.handle(h, p); }
    static void attach(CatPlayMediaClient& c, int fd) {
        c.fd_ = fd; c.connected_ = true; c.connecting_ = false; c.hello_ = false;
        c.connected_at_ = c.last_record_ = std::chrono::steady_clock::now(); c.store_.transport_connected();
    }
};
}
namespace {
int checks{};
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " + #__VA_ARGS__); } while (false)
w::StartSession config() { return {{"test-only", "s", "m", "p", 1, 2, w::UsbRole::Negotiated}, {}}; }
w::PeerInfo peer() {
    w::PeerInfo p; p.device_id = "test-car"; p.name = "CarPlay";
    p.displays = {{"car-main", 1280, 720, 200, 100, 60, 0, {}}, {"car-alt", 640, 480, 100, 100, 30, 0, {}}};
    for (auto c : w::command_catalog) p.negotiated.commands.push_back(c.type);
    for (auto c : w::csm_catalog) p.negotiated.csm_messages.push_back(c.type);
    p.negotiated.services = {w::Service::NowPlaying};
    p.negotiated.video_codecs = {w::VideoCodec::H264AnnexB}; p.negotiated.display_roles = {w::DisplayRole::Main, w::DisplayRole::Alternate};
    p.negotiated.audio_streams = {w::AudioStream::Main};
    p.negotiated.audio_formats = {{}, {w::AudioCodec::AacLc, 48000, 16, 2}};
    p.negotiated.microphone = true; p.negotiated.monotonic_media_timestamps = true; return p;
}
w::ScreenConfiguration screen() { return {10, {}, w::DisplayRole::Main, w::VideoCodec::H264AnnexB, 1280, 720, 30, 100, {0, 0, 1, 0x67, 0, 0, 1, 0x68}}; }
w::AudioConfiguration audio() { return {20, w::AudioStream::Main, w::AudioUse::Telephony, {}, 100, true, true}; }
struct Rig {
    std::unique_ptr<mvp::SessionCore> core = std::make_unique<mvp::SessionCore>();
    w::WiredCarPlayOutput out;
    std::shared_ptr<f::WirelessEndpoint> input = std::make_shared<f::WirelessEndpoint>();
    f::CarPlayForwarder forward{*core, out, input};
    std::uint64_t g{}, epoch{}; w::Ticket startup;
    explicit Rig(w::Limits limits = {}, bool reverse = true) : out(limits) {
        CHECK(core->upsert_source("catplay-real", mvp::InputSourceKind::External, true));
        g = input->begin(91, reverse ? peer().negotiated : w::Capabilities{});
        startup = out.start(config()); CHECK(startup.status); epoch = startup.epoch;
        CHECK(out.take_work()); CHECK(out.complete(epoch, startup.id, {}, {}, peer()));
    }
    void backend() { while (auto op = out.take_work()) CHECK(out.complete(op->epoch, op->id, {})); }
    void pump(int n = 12) { for (int k = 0; k < n; ++k) { forward.tick(); backend(); } forward.tick(); }
    void streams(bool mic = true) { CHECK(input->set_stream(g, screen())); auto a = audio(); a.microphone = mic; CHECK(input->set_stream(g, a)); pump(); CHECK(forward.snapshot().phase == f::Phase::Forwarding); }
    ~Rig() { forward.stop(); for (int n = 0; n < 12; ++n) { forward.tick(); backend(); } }
};
void lifecycle_and_bytes() {
    Rig r; r.streams();
    auto vehicle = r.input->vehicle_info(); CHECK(vehicle && vehicle->output_epoch == r.epoch && vehicle->peer.displays.front().uuid == "car-main");
    CHECK(r.out.pop_completion(r.epoch, r.startup.id)); // Forward never steals startup owner's completion.
    CHECK(r.forward.snapshot().active_streams == 2);
    CHECK(r.input->push_media(r.g, {r.g, 10, 123456, true, {0, 0, 1, 0x65, 9, 8}}));
    CHECK(r.input->push_media(r.g, {r.g, 20, 777, false, {1, 2, 3, 4}}));
    r.forward.tick();
    auto video = r.out.take_media(); CHECK(video && video->stream_id != 10 && video->epoch == r.epoch && video->timestamp == 123456 && video->data == w::Bytes({0, 0, 1, 0x65, 9, 8}));
    auto audio_packet = r.out.take_media(); CHECK(audio_packet && audio_packet->timestamp == 777 && audio_packet->data == w::Bytes({1, 2, 3, 4}));
    CHECK(r.out.receive_microphone({r.epoch, audio_packet->stream_id, 800, false, {5, 6, 7, 8}})); r.forward.tick();
    auto mic = r.input->take_microphone(r.g); CHECK(mic && mic->stream_id == 20 && mic->epoch == r.g && mic->timestamp == 800 && mic->data == w::Bytes({5, 6, 7, 8}));
    auto changed = screen(); changed.width = 640; changed.height = 480; changed.codec_configuration.push_back(9);
    CHECK(r.input->set_stream(r.g, changed)); r.pump(); CHECK(r.forward.snapshot().phase == f::Phase::Forwarding);
    CHECK(r.input->push_media(r.g, {r.g, 10, 123457, false, {0, 0, 1, 0x41}})); r.forward.tick(); CHECK(!r.out.take_media());
    CHECK(r.input->push_media(r.g, {r.g, 10, 123458, true, {0, 0, 1, 0x65}})); r.forward.tick(); CHECK(r.out.take_media());
    CHECK(r.input->push_media(r.g, {r.g, 10, 123459, true, {0, 0, 1, 0x65}})); r.forward.tick();
    CHECK(r.core->upsert_source("other", mvp::InputSourceKind::External, true));
    CHECK(r.core->set_selection(mvp::SelectionMode::Manual, "other")); r.forward.tick();
    CHECK(!r.out.take_media() && !r.input->take_microphone(r.g)); r.pump(); CHECK(r.forward.snapshot().phase == f::Phase::Inactive);
    CHECK(r.out.snapshot().active_streams == 0);
    CHECK(r.core->set_selection(mvp::SelectionMode::Manual, "catplay-real")); r.pump(); CHECK(r.forward.snapshot().phase == f::Phase::Forwarding);
    r.forward.stop(); r.pump(); CHECK(r.forward.snapshot().phase == f::Phase::Inactive);
    r.forward.start(); r.pump(); CHECK(r.forward.snapshot().phase == f::Phase::Forwarding);
    r.input->end(); r.pump(); CHECK(!r.out.take_media() && r.out.snapshot().active_streams == 0);
    CHECK(r.input->push_media(r.g, {r.g, 10, 1, true, {1}}).code == w::Error::StaleSession);
}
void control_ack_and_metadata() {
    Rig r; r.streams();
    auto t = r.input->request_output(r.g, w::SendControl{{w::CommandType::DuckAudio, w::DuckAudio{100, -12}}}); CHECK(t.status);
    r.forward.tick(); CHECK(!r.input->pop_phone_completion());
    auto work = r.out.take_work(); CHECK(work && std::get<w::SendControl>(work->operation).command.type == w::CommandType::DuckAudio);
    CHECK(r.out.complete(r.epoch, work->id, {w::Error::PeerRejected, "vehicle said no"}, {99})); r.forward.tick();
    auto result = r.input->pop_phone_completion(); CHECK(result && result->id == t.id && result->status.code == w::Error::PeerRejected && result->response == w::Bytes({99}));
    w::ControlCommand hid{w::CommandType::HidSendReport, w::HidReport{"original-hid-uuid", {0, 255, 1}, 55}};
    auto car = r.out.receive_control(r.epoch, hid); CHECK(car.status); r.forward.tick(); CHECK(!r.out.take_reply());
    auto request = r.input->take_reverse(); CHECK(request && request->session == 91 && request->generation == r.g);
    auto report = std::get<w::HidReport>(std::get<w::ControlCommand>(request->payload).payload);
    CHECK(report.uuid == "original-hid-uuid" && report.report == w::Bytes({0, 255, 1}) && report.ntp_timestamp == 55);
    CHECK(r.input->complete_reverse(r.g, request->id, {}, {17, 18})); r.forward.tick();
    auto reply = r.out.take_reply(); CHECK(reply && reply->id == car.id && reply->status && reply->response == w::Bytes({17, 18}));
    for (auto command : {w::ControlCommand{w::CommandType::RequestSiri, w::SiriRequest{w::SiriAction::Prewarm}},
        w::ControlCommand{w::CommandType::ForceKeyFrame, w::Empty{}}, w::ControlCommand{w::CommandType::ChangeModes, w::BinaryPlist{{1, 2, 3}}},
        w::ControlCommand{w::CommandType::SetNightMode, w::Toggle{true}}, w::ControlCommand{w::CommandType::SetLimitedUI, w::Toggle{true}},
        w::ControlCommand{w::CommandType::RequestUI, w::UiRequest{"maps:"}}, w::ControlCommand{w::CommandType::IApSendMessage, w::Bytes{5, 4}}}) {
        auto c = r.out.receive_control(r.epoch, command); CHECK(c.status); r.forward.tick();
        auto req = r.input->take_reverse(); CHECK(req && std::get<w::ControlCommand>(req->payload).type == command.type);
        CHECK(r.input->complete_reverse(r.g, req->id, {})); r.forward.tick(); CHECK(r.out.take_reply()->status);
    }
    w::Iap2Message iap{w::CsmType::NowPlayingUpdate, 7, {0, 5, 255, 3}};
    CHECK(r.input->request_output(r.g, w::SendIap2{iap}).status); r.forward.tick(); work = r.out.take_work();
    CHECK(work && std::get<w::SendIap2>(work->operation).message.parameters == iap.parameters); CHECK(r.out.complete(r.epoch, work->id, {})); r.forward.tick(); CHECK(r.input->pop_phone_completion());
    CHECK(r.out.receive_iap2(r.epoch, iap)); r.forward.tick(); request = r.input->take_reverse(); CHECK(request && std::get<w::Iap2Message>(request->payload).parameters == iap.parameters);
    CHECK(r.input->complete_reverse(r.g, request->id, {})); r.forward.tick();
    w::NowPlaying playing; playing.item.title = "unchanged title";
    CHECK(r.input->request_output(r.g, w::SendService{{w::Service::NowPlaying, "native/v1", playing}}).status); r.forward.tick(); work = r.out.take_work();
    CHECK(work && std::get<w::NowPlaying>(std::get<w::SendService>(work->operation).message.payload).item.title == playing.item.title);
    CHECK(r.out.complete(r.epoch, work->id, {})); r.forward.tick(); CHECK(r.input->pop_phone_completion());
}
void bounds_failures_and_reconnect() {
    w::Limits limits; limits.media_packets = 1; limits.media_bytes = 16;
    Rig r(limits, false); r.streams(false);
    auto siri = r.out.receive_control(r.epoch, {w::CommandType::RequestSiri, w::SiriRequest{}}); CHECK(siri.status); r.forward.tick();
    auto refused = r.out.take_reply(); CHECK(refused && refused->status.code == w::Error::NotSupported); CHECK(!r.input->take_reverse());
    for (int n = 0; n < 3; ++n) CHECK(r.input->push_media(r.g, {r.g, 10, std::uint64_t(n + 1), true, {0, 0, 1, 0x65}}));
    r.forward.tick(); CHECK(r.out.snapshot().media_packets == 1);
    for (int n = 0; n < 3; ++n) { auto p = r.out.take_media(); CHECK(p && p->timestamp == std::uint64_t(n + 1)); r.forward.tick(); }
    CHECK(!r.out.take_media());
    auto old_epoch = r.epoch; CHECK(r.out.disconnect(old_epoch)); r.forward.tick(); CHECK(r.forward.snapshot().phase == f::Phase::WaitingOutput);
    CHECK(!r.input->vehicle_info());
    auto start = r.out.start(config()); CHECK(start.status); CHECK(r.out.take_work()); auto no_clock = peer(); no_clock.negotiated.monotonic_media_timestamps = false; CHECK(r.out.complete(start.epoch, start.id, {}, {}, no_clock)); r.epoch = start.epoch; r.pump(); CHECK(r.forward.snapshot().phase == f::Phase::Forwarding);
    CHECK(r.input->push_media(r.g, {r.g, 10, 999, true, {0, 0, 1, 0x65}, w::MediaPacket::Timebase::MonotonicMicroseconds})); r.forward.tick(); CHECK(!r.out.take_media()); CHECK(r.forward.snapshot().last_error.code == w::Error::NotSupported);
    CHECK(r.out.complete(old_epoch, 123, {}).code == w::Error::StaleSession);
    auto changed = screen(); changed.width = 4000; CHECK(r.input->set_stream(r.g, changed)); r.pump(); CHECK(r.forward.snapshot().phase == f::Phase::Failed);
    CHECK(r.forward.snapshot().last_error.code == w::Error::NotSupported);
    auto endpoint = std::make_shared<f::WirelessEndpoint>(1, 4); auto g = endpoint->begin(1, peer().negotiated); CHECK(endpoint->set_stream(g, screen()));
    CHECK(endpoint->push_media(g, {g, 10, 1, true, {1, 2, 3, 4}})); CHECK(endpoint->push_media(g, {g, 10, 2, true, {1}}).code == w::Error::Backpressure);
    CHECK(endpoint->request_output(g, w::StartSession{}).status.code == w::Error::InvalidArgument);
    auto req = endpoint->post_reverse(g, w::ControlCommand{w::CommandType::RequestSiri, w::SiriRequest{}}, w::Clock::now() + w::Milliseconds{100}); CHECK(req.status);
    CHECK(endpoint->complete_reverse(g, req.id, {}).code == w::Error::InvalidState);
    endpoint->expire(w::Clock::now() + w::Milliseconds{200}); auto done = endpoint->pop_reverse_completion(); CHECK(done && done->status.code == w::Error::Timeout);
    CHECK(endpoint->complete_reverse(g, req.id, {}).code == w::Error::NotFound);
    auto old = g; endpoint->end(); g = endpoint->begin(2); CHECK(g != old && !endpoint->take_media(g));
}
void capture_socket() {
#if defined(__unix__)
    auto core = std::make_unique<mvp::SessionCore>(); auto store = std::make_unique<mvp::RealMediaStore>();
    store->set_selected(true);
    auto input = std::make_shared<f::WirelessEndpoint>(); auto capture = std::make_shared<f::CatPlayForwardCapture>(input, 30);
    auto client = std::make_unique<mvp::CatPlayMediaClient>(*core, *store, "/test/unused-forward.sock");
    int pair[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0);
    mvp::CatPlayApiTestAccess::attach(*client, pair[0]); client->set_record_observer(capture);
    auto send = [&](cp::Header h, w::Bytes data = {}) {
        h.payload_bytes = static_cast<std::uint32_t>(data.size()); auto header = cp::encode_header(h);
        CHECK(::send(pair[1], header.data(), 17, MSG_NOSIGNAL) == 17); client->tick();
        CHECK(::send(pair[1], header.data() + 17, header.size() - 17, MSG_NOSIGNAL) == ssize_t(header.size() - 17));
        if (!data.empty()) CHECK(::send(pair[1], data.data(), data.size(), MSG_NOSIGNAL) == ssize_t(data.size()));
        client->tick();
    };
    send({cp::Type::ServerHello, 0, 0, 0, 0, 0, 0, cp::kMaxWirePayloadBytes, 1000, 7, 1});
    send({cp::Type::SessionBegin, 0, 0, 901}); auto g = input->state().generation; CHECK(input->state().connected);
    send({cp::Type::VideoStart, 0, 1, 901});
    send({cp::Type::VideoConfig, 0, 1, 901, 0, 0, 0, 1, 640, 480, 1}, {0, 0, 1, 0x67, 0, 0, 1, 0x68});
    send({cp::Type::VideoFrame, cp::kFlagKeyframe, 1, 901, 1, 1001, 0, 1, 640, 480, 1}, {0, 0, 1, 0x65, 0x99});
    auto frame = input->take_media(g); CHECK(frame && frame->timestamp == 1001 && frame->timebase == w::MediaPacket::Timebase::MonotonicMicroseconds && frame->data == w::Bytes({0, 0, 1, 0x65, 0x99}));
    w::Bytes descriptor(16); cp::AudioFormat format{5, 16, 16, 1, 0, 4, true}; cp::encode_audio_format(descriptor.data(), format); descriptor[0] = 100;
    send({cp::Type::AudioStart, 0, 2, 901, 0, 0, 0, 2, 48000, 2, 1}, descriptor);
    send({cp::Type::AudioChunk, cp::kFlagOpaquePayload, 2, 901, 1, 2002, 0, 2, 48000, 2, 1024}, {1, 2, 3, 4, 5});
    auto encoded = input->take_media(g); CHECK(encoded && encoded->data == w::Bytes({1, 2, 3, 4, 5}) && encoded->timestamp == 2002);
    CHECK(client->connected()); // Opaque frame passed framing guard, independent of preview decision.
    CHECK(!input->state().reverse_capabilities.microphone && input->state().reverse_capabilities.commands.empty());
    send({cp::Type::VideoFrame, 0, 1, 901, 3, 1003, 0, 1, 640, 480, 1}, {0, 0, 1, 0x41}); CHECK(!input->take_media(g));
    send({cp::Type::VideoFrame, cp::kFlagKeyframe, 1, 901, 4, 1004, 0, 1, 640, 480, 1}, {0, 0, 1, 0x65}); CHECK(input->take_media(g));
    send({cp::Type::VideoFrame, cp::kFlagKeyframe, 1, 900, 5, 1005, 0, 1, 640, 480, 1}, {0, 0, 1, 0x65}); CHECK(!input->take_media(g));
    w::WiredCarPlayOutput out; auto startup = out.start(config()); CHECK(out.take_work()); CHECK(out.complete(startup.epoch, startup.id, {}, {}, peer()));
    f::CarPlayForwarder forward(*core, out, input);
    for (int n = 0; n < 10; ++n) { forward.tick(); while (auto work = out.take_work()) CHECK(out.complete(work->epoch, work->id, {})); }
    send({cp::Type::VideoFrame, cp::kFlagKeyframe, 1, 901, 6, 1006, 0, 1, 640, 480, 1}, {0, 0, 1, 0x65, 7}); forward.tick();
    auto sent = out.take_media(); CHECK(sent && sent->data == w::Bytes({0, 0, 1, 0x65, 7}) && sent->timestamp == 1006 && sent->timebase == w::MediaPacket::Timebase::MonotonicMicroseconds);
    ::close(pair[1]); client->tick(); CHECK(!input->state().connected); forward.tick();
    CHECK(!out.take_media()); client->set_record_observer(nullptr); client->stop();
    CHECK(capture->snapshot().dropped >= 2);
#endif
}
void composition() {
    auto core = std::make_unique<mvp::SessionCore>(); auto store = std::make_unique<mvp::RealMediaStore>();
    auto client = std::make_unique<mvp::CatPlayMediaClient>(*core, *store, "/test/not-started.sock");
    w::WiredCarPlayOutput out;
    f::CarPlayDirectConnection connection(*core, *client, out, 30);
    store->transport_connected(); store->set_selected(true);
    auto feed = [&](cp::Header h, w::Bytes data = {}) { h.payload_bytes = static_cast<std::uint32_t>(data.size()); CHECK(mvp::CatPlayApiTestAccess::deliver(*client, h, data)); };
    feed({cp::Type::ServerHello, 0, 0, 0, 0, 0, 0, cp::kMaxWirePayloadBytes, 1000, 7, 1});
    feed({cp::Type::SessionBegin, 0, 0, 71}); feed({cp::Type::VideoStart, 0, 4, 71});
    feed({cp::Type::VideoConfig, 0, 4, 71, 0, 0, 0, 1, 640, 480, 1}, {0, 0, 1, 0x67, 0, 0, 1, 0x68});
    connection.tick(); CHECK(connection.snapshot().phase == f::Phase::WaitingOutput);
    auto start = out.start(config()); CHECK(out.take_work()); CHECK(out.complete(start.epoch, start.id, {}, {}, peer()));
    for (int n = 0; n < 8; ++n) { connection.tick(); while (auto work = out.take_work()) CHECK(out.complete(work->epoch, work->id, {})); }
    CHECK(connection.snapshot().phase == f::Phase::Forwarding);
    feed({cp::Type::VideoFrame, cp::kFlagKeyframe, 4, 71, 1, 7001, 0, 1, 640, 480, 1}, {0, 0, 1, 0x65, 42}); connection.tick();
    auto packet = out.take_media(); CHECK(packet && packet->data.back() == 42 && packet->timestamp == 7001);
    CHECK(connection.capture_snapshot().records == 5);
    connection.stop();
    for (int n = 0; n < 8; ++n) { connection.tick(); while (auto work = out.take_work()) CHECK(out.complete(work->epoch, work->id, {})); }
    CHECK(connection.snapshot().phase == f::Phase::Inactive && out.snapshot().active_streams == 0);
}
}
int main() {
    try { lifecycle_and_bytes(); control_ack_and_metadata(); bounds_failures_and_reconnect(); capture_socket(); composition(); std::cout << "Forward checks passed: " << checks << '\n'; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
