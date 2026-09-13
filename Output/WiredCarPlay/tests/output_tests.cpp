#include "wired_carplay/output.hpp"
#include <atomic>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
using namespace zero2w::wired;
namespace {
int checks{};
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " + #__VA_ARGS__); } while (false)
StartSession config() { return {{"test-only-udc", "test-serial", "test-manufacturer", "test-product", 1, 2, UsbRole::Negotiated}, "test-only-store"}; }
PeerInfo peer() {
    PeerInfo p;
    p.device_id = "test-car"; p.name = "CarPlay";
    p.displays.push_back({"display", 1280, 720, 200, 100, 60, 0, {}});
    for (auto c : command_catalog) p.negotiated.commands.push_back(c.type);
    for (auto c : csm_catalog) p.negotiated.csm_messages.push_back(c.type);
    p.negotiated.video_codecs = {VideoCodec::H264AnnexB};
    p.negotiated.audio_formats = {AudioFormat{}};
    p.negotiated.audio_streams = {AudioStream::Main};
    p.negotiated.display_roles = {DisplayRole::Main};
    p.negotiated.microphone = true;
    p.negotiated.services = {Service::NowPlaying};
    return p;
}
Epoch ready(WiredCarPlayOutput& o, PeerInfo p = peer()) {
    auto t = o.start(config()); CHECK(t.status); CHECK(t.id != 0);
    CHECK(o.snapshot().phase == Phase::Starting);
    CHECK(!o.pop_completion());
    auto w = o.take_work(); CHECK(w && w->id == t.id);
    CHECK(o.complete(t.epoch, t.id, {}, {}, std::move(p)));
    auto done = o.pop_completion(); CHECK(done && done->status);
    CHECK(o.snapshot().phase == Phase::Ready); return t.epoch;
}
void execute(WiredCarPlayOutput& o, Operation op) {
    auto t = o.submit(std::move(op)); CHECK(t.status);
    auto w = o.take_work(); CHECK(w && w->id == t.id);
    CHECK(o.complete(t.epoch, t.id, {}));
    auto done = o.pop_completion(); CHECK(done && done->id == t.id && done->status);
}
ScreenConfiguration screen() { return {1, "display", DisplayRole::Main, VideoCodec::H264AnnexB, 1280, 720, 30, 100, {}}; }
AudioConfiguration audio() { return {2, AudioStream::Main, AudioUse::Telephony, {}, 100, true, true}; }
ControlCommand payload(CommandType t) {
    switch (t) {
    case CommandType::DuckAudio: case CommandType::UnduckAudio: return {t, DuckAudio{20, -12}};
    case CommandType::DisableBluetooth: return {t, BluetoothDevice{"01:23:45:67:89:ab"}};
    case CommandType::ChangeModes: return {t, BinaryPlist{{1, 2, 3}}};
    case CommandType::ModesChanged: return {t, ModeState{}};
    case CommandType::ForceKeyFrame: return {t, Empty{}};
    case CommandType::HidSendReport: return {t, HidReport{"hid", {1, 2}, 42}};
    case CommandType::HidSetInputMode: return {t, SetHidInputMode{"hid", HidInputMode::DialPad}};
    case CommandType::RequestSiri: return {t, SiriRequest{SiriAction::ButtonDown}};
    case CommandType::RequestUI: return {t, UiRequest{"maps:"}};
    case CommandType::SetNightMode: case CommandType::SetLimitedUI: return {t, Toggle{true}};
    case CommandType::IApSendMessage: return {t, Bytes{1, 2, 3}};
    }
    throw std::runtime_error("unknown command");
}
void lifecycle() {
    WiredCarPlayOutput o;
    CHECK(o.snapshot().phase == Phase::Idle);
    CHECK(o.submit(Record{}).status.code == Error::InvalidState);
    CHECK(o.start({}).status.code == Error::InvalidArgument);
    CHECK(o.start(config(), Milliseconds{0}).status.code == Error::InvalidArgument);
    auto t = o.start(config()); CHECK(t.status);
    CHECK(o.start(config()).status.code == Error::InvalidState);
    CHECK(o.complete(t.epoch, t.id, {}, {}, peer()).code == Error::InvalidState);
    CHECK(o.take_work()); CHECK(!o.take_work());
    CHECK(o.report_progress(t.epoch, Phase::Authenticating));
    CHECK(o.report_progress(t.epoch, Phase::Ready).code == Error::InvalidState);
    CHECK(o.complete(t.epoch, t.id, {}).code == Error::InvalidArgument);
    CHECK(!o.info_cached());
    CHECK(o.complete(t.epoch, t.id, {}, {}, peer()));
    CHECK(o.complete(t.epoch, t.id, {}).code == Error::NotFound);
    CHECK(o.pop_completion());
    auto info = o.info_cached(); CHECK(info && info->device_id == "test-car");
    info->device_id = "mutated"; CHECK(o.info_cached()->device_id == "test-car");
    auto stop = o.shutdown(); CHECK(stop.status); CHECK(o.snapshot().phase == Phase::Stopping);
    CHECK(!o.info_cached()); CHECK(o.take_work()); CHECK(o.complete(stop.epoch, stop.id, {}));
    CHECK(o.snapshot().phase == Phase::Closed); CHECK(o.pop_completion());
    auto e2 = ready(o); CHECK(e2 > t.epoch);
    CHECK(o.disconnect(t.epoch).code == Error::StaleSession);
    CHECK(o.complete(t.epoch, t.id, {}).code == Error::StaleSession);
    CHECK(o.update_clock(t.epoch, {}).code == Error::StaleSession);
    CHECK(o.disconnect(e2, {}).code == Error::InvalidArgument);
    CHECK(o.disconnect(e2)); CHECK(!o.info_cached());
    CHECK(o.snapshot().phase == Phase::Failed);
}
void directions_and_messages() {
    WiredCarPlayOutput o; auto e = ready(o);
    for (auto c : command_catalog) {
        const auto cmd = payload(c.type);
        if (c.direction != Direction::AccessoryToDevice) execute(o, SendControl{cmd});
        else CHECK(o.submit(SendControl{cmd}).status.code == Error::InvalidArgument);
        auto t = o.receive_control(e, cmd);
        if (c.direction == Direction::DeviceToAccessory) CHECK(t.status.code == Error::InvalidArgument);
        else {
            CHECK(t.status); auto event = o.pop_event(); CHECK(event && event->epoch == e);
            CHECK(std::get<IncomingControl>(event->payload).command.type == c.type);
            CHECK(o.respond(e, t.id, {}, {9, 8}));
            CHECK(o.respond(e, t.id, {}).code == Error::InvalidState);
            auto reply = o.take_reply(); CHECK(reply && reply->id == t.id && reply->response == Bytes({9, 8}));
            CHECK(!o.take_reply()); CHECK(o.respond(e, t.id, {}).code == Error::NotFound);
        }
    }
    for (auto c : csm_catalog) {
        Iap2Message m{c.type, 19, {0, 255, 2, 0}};
        execute(o, SendIap2{m}); CHECK(o.receive_iap2(e, m));
        auto event = o.pop_event(); CHECK(event);
        auto& got = std::get<Iap2Message>(event->payload); CHECK(got.type == m.type && got.session_id == 19 && got.parameters == m.parameters);
    }
    CHECK(o.receive_iap2(e, {static_cast<CsmType>(0xFFFF), 1, {}}).code == Error::InvalidArgument);
    CHECK(o.receive_control(e, {CommandType::RequestSiri, Empty{}}).status.code == Error::InvalidArgument);
    CHECK(o.submit(SendControl{{CommandType::DuckAudio, DuckAudio{1, std::numeric_limits<double>::quiet_NaN()}}}).status.code == Error::InvalidArgument);
    CHECK(o.submit(SendControl{{CommandType::DisableBluetooth, BluetoothDevice{"bad"}}}).status.code == Error::InvalidArgument);
    CHECK(o.receive_control(e, {CommandType::HidSendReport, HidReport{"", {1}, {}}}).status.code == Error::InvalidArgument);
    ServiceMessage svc{Service::NowPlaying, "test/v1", Bytes{0, 1, 255}};
    execute(o, SendService{svc}); CHECK(o.receive_service(e, svc));
    CHECK(std::get<Bytes>(std::get<ServiceMessage>(o.pop_event()->payload).payload) == std::get<Bytes>(svc.payload));
    svc.service = Service::Lyrics; CHECK(o.submit(SendService{svc}).status.code == Error::NotSupported);
    CHECK(o.receive_service(e, svc).code == Error::NotSupported);
    CHECK(o.submit(PairingRequest{PairingAction::ForgetPeer, "car"}).status.code == Error::NotSupported);
    execute(o, AssertModes{}); execute(o, DrainTeardown{});
}
void media() {
    Limits l; l.media_packets = 2; l.media_bytes = 8; l.microphone_packets = 1; l.microphone_bytes = 4;
    WiredCarPlayOutput o(l); auto e = ready(o);
    CHECK(o.submit(Record{}).status.code == Error::InvalidState);
    auto s = screen(); s.width = 4000; CHECK(o.submit(s).status.code == Error::NotSupported);
    s = screen(); s.codec = VideoCodec::HevcAnnexB; CHECK(o.submit(s).status.code == Error::NotSupported);
    s = screen(); auto t = o.submit(s); CHECK(t.status);
    CHECK(o.snapshot().active_streams == 0); CHECK(o.submit(audio()).status.code == Error::InvalidState);
    CHECK(o.take_work()); CHECK(o.complete(e, t.id, {Error::PeerRejected, "test"})); CHECK(o.pop_completion());
    CHECK(o.snapshot().active_streams == 0);
    execute(o, screen()); execute(o, audio());
    CHECK(o.submit(screen()).status.code == Error::InvalidState);
    CHECK(o.push_media({e, 1, 42, true, {1, 2}}).code == Error::InvalidState);
    execute(o, Record{}); CHECK(o.snapshot().phase == Phase::Streaming);
    CHECK(o.submit(Record{}).status.code == Error::InvalidState);
    CHECK(o.push_media({e - 1, 1, 42, true, {1}}).code == Error::StaleSession);
    CHECK(o.push_media({e, 77, 42, true, {1}}).code == Error::NotFound);
    CHECK(o.push_media({e, 2, 42, false, {1, 2}}).code == Error::InvalidArgument); // stereo PCM frame = 4 bytes
    CHECK(o.push_media({e, 1, 42, true, {1, 2, 3, 4}}));
    CHECK(o.push_media({e, 2, 48, false, {5, 6, 7, 8}}));
    CHECK(o.push_media({e, 1, 43, false, {1}}).code == Error::Backpressure);
    CHECK(o.snapshot().media_bytes == 8);
    auto frame = o.take_media(); CHECK(frame && frame->key_frame && frame->timestamp == 42 && frame->data == Bytes({1, 2, 3, 4}));
    CHECK(o.receive_microphone({e, 1, 0, false, {0, 0, 0, 0}}).code == Error::InvalidArgument);
    CHECK(o.receive_microphone({e, 2, 96, false, {1, 2, 3, 4}}));
    CHECK(o.receive_microphone({e, 2, 100, false, {1, 2, 3, 4}}).code == Error::Backpressure);
    auto mic = o.pop_microphone(); CHECK(mic && mic->timestamp == 96 && mic->data.size() == 4);
    CHECK(o.snapshot().microphone_bytes == 0);
    auto now = Clock::now(); CHECK(o.update_clock(e, {123, 48, 48000, now}));
    CHECK(o.update_clock(e, {124, 96, 48000, now - Milliseconds{1}}).code == Error::InvalidArgument);
    CHECK(o.media_clock()->ntp_timestamp == 123);
    auto teardown = o.submit(TeardownStream{2}); CHECK(teardown.status);
    CHECK(o.push_media({e, 2, 52, false, {1, 2, 3, 4}}).code == Error::InvalidState);
    CHECK(o.take_work()); CHECK(o.complete(e, teardown.id, {})); CHECK(o.pop_completion());
    CHECK(o.snapshot().media_bytes == 0); CHECK(!o.take_media());
    execute(o, TeardownStream{1}); CHECK(o.snapshot().phase == Phase::Ready);
    execute(o, screen()); execute(o, Record{});
    CHECK(o.push_media({e, 1, 1, true, {1}})); CHECK(o.disconnect(e));
    CHECK(!o.take_media() && !o.pop_microphone() && !o.media_clock());
    CHECK(o.snapshot().active_streams == 0 && o.snapshot().media_bytes == 0);
}
void bounded_requests() {
    Limits l; l.pending_requests = 2; l.inbound_requests = 1; l.events = 1;
    WiredCarPlayOutput o(l); auto e = ready(o);
    auto a = o.submit(SendControl{payload(CommandType::DuckAudio)});
    auto b = o.submit(SendControl{payload(CommandType::UnduckAudio)}); CHECK(a.status && b.status);
    CHECK(o.submit(DrainTeardown{}).status.code == Error::Backpressure);
    CHECK(o.cancel(e, a.id)); CHECK(o.snapshot().completed_requests == 1);
    CHECK(o.submit(DrainTeardown{}).status.code == Error::Backpressure); // completion memory also bounded
    CHECK(o.pop_completion());
    auto c = o.submit(DrainTeardown{}); CHECK(c.status);
    auto stop = o.shutdown(); CHECK(stop.status); // reserved shutdown slot even when full
    CHECK(o.snapshot().pending_requests == 1 && o.snapshot().completed_requests == 2);
    CHECK(o.take_work()->id == stop.id); CHECK(o.complete(e, stop.id, {}));
    while (o.pop_completion()) {}
    e = ready(o);
    auto in = o.receive_control(e, payload(CommandType::RequestSiri)); CHECK(in.status);
    CHECK(o.receive_control(e, payload(CommandType::RequestSiri)).status.code == Error::Backpressure);
    CHECK(o.receive_iap2(e, {CsmType::NowPlayingUpdate, 1, {}}).code == Error::Backpressure);
    CHECK(o.pop_event()); // draining event doesn't discard obligation to respond
    CHECK(o.receive_control(e, payload(CommandType::RequestSiri)).status.code == Error::Backpressure);
    auto timed = o.take_reply(Clock::now() + Milliseconds{4000}); CHECK(timed && timed->status.code == Error::Timeout);
    CHECK(o.respond(e, in.id, {}).code == Error::NotFound);
    CHECK(o.receive_control(e, payload(CommandType::RequestSiri)).status);
    CHECK(o.disconnect(e)); CHECK(!o.pop_event() && !o.take_reply());
}
void timeouts_and_capabilities() {
    WiredCarPlayOutput o; auto e = ready(o);
    auto t = o.submit(SendControl{payload(CommandType::DuckAudio)}); CHECK(t.status);
    CHECK(o.take_work()); o.expire(Clock::now() + Milliseconds{4000});
    auto done = o.pop_completion(); CHECK(done && done->status.code == Error::Timeout);
    CHECK(o.snapshot().phase == Phase::Ready);
    CHECK(o.complete(e, t.id, {}).code == Error::NotFound);
    t = o.submit(screen()); CHECK(t.status); CHECK(o.take_work());
    o.expire(Clock::now() + Milliseconds{4000}); CHECK(o.snapshot().phase == Phase::Failed);
    CHECK(o.pop_completion()->status.code == Error::Timeout);
    auto p = peer(); p.negotiated = {};
    e = ready(o, p);
    CHECK(o.submit(screen()).status.code == Error::NotSupported);
    CHECK(o.submit(audio()).status.code == Error::NotSupported);
    CHECK(o.submit(SendControl{payload(CommandType::DuckAudio)}).status.code == Error::NotSupported);
    CHECK(o.receive_control(e, payload(CommandType::RequestSiri)).status.code == Error::NotSupported);
    CHECK(o.submit(SendIap2{{CsmType::NowPlayingUpdate, 1, {}}}).status.code == Error::NotSupported);
    CHECK(o.receive_iap2(e, {CsmType::NowPlayingUpdate, 1, {}}).code == Error::NotSupported);
    CHECK(o.disconnect(e));
    auto start = o.start(config()); CHECK(start.status); CHECK(o.cancel(start.epoch, start.id));
    CHECK(o.snapshot().phase == Phase::Closed); CHECK(o.pop_completion());
    start = o.start(config()); CHECK(start.status); CHECK(o.take_work());
    CHECK(o.complete(start.epoch, start.id, {Error::AuthenticationFailed, "test failure"}));
    CHECK(o.snapshot().last_error.code == Error::AuthenticationFailed); CHECK(o.pop_completion());
    bool threw = false;
    try { Limits invalid; invalid.events = 0; WiredCarPlayOutput bad(invalid); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}
void concurrent_access() {
    WiredCarPlayOutput o; auto e = ready(o); execute(o, screen()); execute(o, Record{});
    std::atomic<bool> done{}; std::atomic<int> accepted{}, consumed{};
    std::thread producer([&] { for (int n = 0; n < 1000; ++n) if (o.push_media({e, 1, static_cast<std::uint64_t>(n), false, {1, 2, 3}})) ++accepted; done = true; });
    std::thread consumer([&] { while (!done || o.snapshot().media_packets) { if (o.take_media()) ++consumed; std::this_thread::yield(); } });
    producer.join(); consumer.join(); CHECK(accepted == consumed); CHECK(o.snapshot().media_bytes == 0);
}
void metadata_and_formats() {
    for (unsigned bit = 2; bit <= 32; ++bit) {
        auto mask = std::uint64_t{1} << bit;
        auto format = audio_format_from_wire(mask); CHECK(format);
        CHECK(wire_audio_format(*format) == mask);
    }
    CHECK(!audio_format_from_wire(0)); CHECK(!audio_format_from_wire(12)); CHECK(!audio_format_from_wire(std::uint64_t{1} << 33));
    CHECK(wire_audio_format(AudioFormat{}) == (std::uint64_t{1} << 15));
    CHECK(wire_audio_format({AudioCodec::AacEld, 48000, 16, 1}) == (std::uint64_t{1} << 32));
    CHECK(!wire_audio_format({AudioCodec::Pcm, 16000, 24, 1}));
    WiredCarPlayOutput o; auto p = peer(); p.negotiated.services.clear(); p.negotiated.pairing_management = true;
    for (unsigned n = 0; n <= static_cast<unsigned>(Service::WifiConfiguration); ++n) p.negotiated.services.push_back(static_cast<Service>(n));
    auto e = ready(o, p);
    for (auto s : p.negotiated.services) {
        execute(o, SendService{{s, "test-only/opaque-v1", Bytes{0, 255, 1}}});
        CHECK(o.receive_service(e, {s, "test-only/opaque-v1", Bytes{1}})); CHECK(o.pop_event());
    }
    execute(o, PairingRequest{PairingAction::PairVerify, "test-peer"});
    std::vector<ServiceMessage> typed{
        {Service::NowPlaying, "zero2w/v1", NowPlaying{}},
        {Service::Artwork, "zero2w/v1", Artwork{"image", "image/png", 1, 1, {1}}},
        {Service::Lyrics, "zero2w/v1", Lyrics{"track", "zh", {{0, 100, "hello"}, {100, 200, "world"}}}},
        {Service::Navigation, "zero2w/v1", Navigation{}},
        {Service::Contacts, "zero2w/v1", ContactDirectory{}},
        {Service::Favorites, "zero2w/v1", ContactDirectory{}},
        {Service::Recents, "zero2w/v1", RecentCalls{}},
        {Service::MediaLibrary, "zero2w/v1", MediaLibrary{}},
        {Service::PlaybackQueue, "zero2w/v1", MediaLibrary{}},
        {Service::CallState, "zero2w/v1", CallState{}},
        {Service::Communications, "zero2w/v1", Communications{}},
        {Service::VehicleInformation, "zero2w/v1", VehicleInformation{}},
        {Service::VehicleStatus, "zero2w/v1", VehicleStatus{}},
        {Service::Location, "zero2w/v1", Location{}},
        {Service::Power, "zero2w/v1", Power{}},
        {Service::DeviceNotifications, "zero2w/v1", DeviceNotification{}},
        {Service::ExternalAccessory, "zero2w/v1", ExternalAccessoryData{"test.protocol", 1, {1, 2}}},
        {Service::FileTransfer, "zero2w/v1", FileChunk{"transfer", "test/octet", 0, 2, true, {1, 2}}},
        {Service::DisplayPanels, "zero2w/v1", DisplayPanels{{{"main", 0, 0, 1280, 720}}}},
        {Service::Vocoder, "zero2w/v1", Vocoder{"aac-eld", 16000, 64000}}
    };
    for (const auto& s : typed) {
        auto t = o.submit(SendService{s}); CHECK(t.status);
        auto w = o.take_work(); CHECK(w && std::get<SendService>(w->operation).message.payload.index() == s.payload.index());
        CHECK(o.complete(e, t.id, {})); CHECK(o.pop_completion());
        CHECK(o.receive_service(e, s)); auto event = o.pop_event(); CHECK(event);
        CHECK(std::get<ServiceMessage>(event->payload).payload.index() == s.payload.index());
    }
    CHECK(o.submit(SendService{{Service::Lyrics, "zero2w/v1", Power{}}}).status.code == Error::InvalidArgument);
    CHECK(o.receive_service(e, {Service::Power, "zero2w/v1", Power{101}}).code == Error::InvalidArgument);
    CHECK(o.receive_service(e, {Service::Lyrics, "zero2w/v1", Lyrics{"t", "zh", {{4, 2, "bad"}}}}).code == Error::InvalidArgument);
    Location bad; bad.latitude = std::numeric_limits<double>::quiet_NaN();
    CHECK(o.receive_service(e, {Service::Location, "zero2w/v1", bad}).code == Error::InvalidArgument);
    FileChunk bad_file{"x", "x", 9, 10, true, {1, 2}};
    CHECK(o.receive_service(e, {Service::FileTransfer, "zero2w/v1", bad_file}).code == Error::InvalidArgument);
    ContactDirectory excessive; excessive.entries.resize(257);
    CHECK(o.submit(SendService{{Service::Contacts, "zero2w/v1", excessive}}).status.code == Error::InvalidArgument);
}
}
int main() {
    try {
        lifecycle(); directions_and_messages(); media(); bounded_requests(); timeouts_and_capabilities(); concurrent_access(); metadata_and_formats();
        std::cout << "Output contract checks passed: " << checks << '\n'; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
