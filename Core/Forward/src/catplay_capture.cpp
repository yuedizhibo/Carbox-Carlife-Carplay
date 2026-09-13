#include "forward/catplay_capture.hpp"
#include "wirelesscarplay/catplay_media_ext.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace zero2w::forward {
namespace cp = mvp::catplay_media;
namespace {
std::optional<w::Service> service(cp::ext::DType t) {
    using D = cp::ext::DType; using S = w::Service;
    switch (t) {
    case D::Metadata: return S::NowPlaying; case D::ArtworkChunk: return S::Artwork;
    case D::LyricsChunk: return S::Lyrics; case D::MediaLibrary: return S::MediaLibrary;
    case D::Navigation: return S::Navigation; case D::Vehicle: return S::VehicleStatus;
    case D::Telephony: return S::CallState; case D::ContactsChunk: return S::Contacts;
    case D::CallLogChunk: return S::Recents; case D::Ducking: return {}; // Scalar policy, not a protocol command.
    case D::SafeArea: return S::ViewArea; case D::AuxPlane: return S::DisplayPanels;
    case D::Calibration: case D::DayNight: return S::Appearance;
    case D::FrameRate: return S::DisplayPanels;
    case D::FileTransfer: return S::FileTransfer;
    case D::Ota: case D::Activation: case D::ContentEncryption: case D::MultiSession: return S::Diagnostics;
    case D::AssistiveTouch: case D::VoiceOverState: case D::HidModeState: case D::ProximityState: case D::Capability: return S::UiContext;
    }
    return {};
}
}
struct CatPlayForwardCapture::Impl {
    std::shared_ptr<WirelessEndpoint> endpoint;
    std::uint32_t fps;
    bool hello{};
    std::uint64_t session{}, generation{};
    struct Video { std::uint32_t role{}, config{}, width{}, height{}; std::uint64_t sequence{}; bool wait_key{true}; };
    struct Audio { w::AudioConfiguration configuration; std::uint64_t sequence{}; };
    std::map<std::uint32_t, Video> video;
    std::map<std::uint32_t, Audio> audio;
    std::atomic<std::uint64_t> records{}, dropped{}, unsupported{};
    Impl(std::shared_ptr<WirelessEndpoint> e, std::uint32_t f) : endpoint(std::move(e)), fps(f) {}
    void reset() { endpoint->end(); hello = false; session = generation = 0; video.clear(); audio.clear(); }
    void handle(const cp::Header& h, std::span<const std::uint8_t> bytes) {
        if (!cp::ext::valid_declared_ext(h) || !cp::ext::valid_static_ext(h, bytes)) { ++dropped; return; }
        ++records;
        if (h.type == cp::Type::ServerHello) { if (hello) { ++dropped; return; } hello = true; return; }
        if (!hello) { ++dropped; return; }
        if (h.type == cp::Type::SessionBegin) {
            if (session == h.session_id) return;
            video.clear(); audio.clear(); session = h.session_id; generation = endpoint->begin(session); return;
        }
        if (!session || session != h.session_id) { if (h.type != cp::Type::Heartbeat) ++dropped; return; }
        if (h.type == cp::Type::SessionEnd) { endpoint->end(); session = generation = 0; video.clear(); audio.clear(); return; }
        if (h.type == cp::Type::Heartbeat) return;
        if (h.type == cp::Type::VideoStart) {
            if (audio.contains(h.stream_id)) { ++dropped; return; }
            auto it = video.find(h.stream_id);
            if (it != video.end() && it->second.role == h.p0) return;
            if (video.size() + audio.size() >= 8 && it == video.end()) { ++dropped; return; }
            endpoint->remove_stream(generation, h.stream_id); video[h.stream_id] = Video{h.p0}; return;
        }
        if (h.type == cp::Type::VideoConfig) {
            auto it = video.find(h.stream_id); if (it == video.end()) { ++dropped; return; }
            auto& v = it->second;
            if (h.p0 != 1) { endpoint->remove_stream(generation, h.stream_id); v.config = 0; ++unsupported; return; }
            if (v.config == h.p3) return;
            endpoint->remove_stream(generation, h.stream_id);
            w::ScreenConfiguration s{h.stream_id, {}, v.role == 0 ? w::DisplayRole::Main : w::DisplayRole::Alternate,
                w::VideoCodec::H264AnnexB, h.p1, h.p2, fps, 100, w::Bytes(bytes.begin(), bytes.end())};
            auto status = endpoint->set_stream(generation, std::move(s));
            if (!status) { ++dropped; return; }
            v.config = h.p3; v.width = h.p1; v.height = h.p2; v.sequence = 0; v.wait_key = true; return;
        }
        if (h.type == cp::Type::AudioStart) {
            if (video.contains(h.stream_id)) { ++dropped; return; }
            if (!audio.contains(h.stream_id) && video.size() + audio.size() >= 8) { ++dropped; return; }
            auto f = cp::decode_audio_format(bytes.data(), bytes.size(), h.p2);
            const auto unsupported_audio = [&] { endpoint->remove_stream(generation, h.stream_id); audio.erase(h.stream_id); ++unsupported; };
            const auto bits = f.valid_bits ? f.valid_bits : 16;
            const auto container = f.total_bits ? f.total_bits : bits;
            if ((f.codec == 2 && bits != 16) || (f.codec == 3 && bits != 24)) { unsupported_audio(); return; }
            w::AudioCodec codec;
            switch (f.codec) {
            case 0: case 1: case 2: case 3: codec = w::AudioCodec::Pcm; break;
            case 4: codec = w::AudioCodec::Alac; break; case 5: codec = w::AudioCodec::AacLc; break;
            case 6: codec = w::AudioCodec::AacEld; break; case 7: codec = w::AudioCodec::Opus; break;
            default: unsupported_audio(); return;
            }
            // Never reinterpret float, padded samples, big-endian data or extra channels as packed PCM.
            if (codec == w::AudioCodec::Pcm && (container != bits || (f.format_flags && f.format_flags != 22))) { unsupported_audio(); return; }
            w::AudioFormat format{codec, h.p1, static_cast<std::uint8_t>(bits), static_cast<std::uint8_t>(h.p2)};
            if (!w::wire_audio_format(format)) { unsupported_audio(); return; }
            w::AudioConfiguration a{h.stream_id, static_cast<w::AudioStream>(bytes[0]), static_cast<w::AudioUse>(h.p0), format, 100, true, false};
            // bytes[2] is only an input request indication; the legacy engine records SILENCE.
            // Do not advertise it as real car microphone support.
            if (audio.contains(h.stream_id)) {
                const auto& old = audio.at(h.stream_id).configuration;
                if (old.stream == a.stream && old.use == a.use && old.format == a.format) return;
                endpoint->remove_stream(generation, h.stream_id);
            }
            if (endpoint->set_stream(generation, a)) audio[h.stream_id] = Audio{a}; else ++dropped;
            return;
        }
        if (h.type == cp::Type::VideoEnd || h.type == cp::Type::AudioEnd) {
            const auto erased = h.type == cp::Type::VideoEnd ? video.erase(h.stream_id) : audio.erase(h.stream_id);
            if (!erased) { ++dropped; return; }
            endpoint->remove_stream(generation, h.stream_id); return;
        }
        if (h.type == cp::Type::VideoFrame || h.type == cp::Type::AudioChunk) {
            bool key = (h.flags & cp::kFlagKeyframe) != 0;
            if (h.type == cp::Type::VideoFrame) {
                auto it = video.find(h.stream_id);
                if (it == video.end() || !it->second.config || it->second.config != h.p3 || it->second.width != h.p1 || it->second.height != h.p2 || h.sequence <= it->second.sequence) { ++dropped; return; }
                auto& v = it->second;
                if ((h.flags & cp::kFlagDiscontinuity) || (v.sequence && h.sequence != v.sequence + 1)) v.wait_key = true;
                v.sequence = h.sequence;
                if (v.wait_key && !key) { ++dropped; return; }
            } else {
                auto it = audio.find(h.stream_id);
                if (it == audio.end() || h.sequence <= it->second.sequence) { ++dropped; return; }
                const auto& a = it->second.configuration;
                if (h.p1 != a.format.sample_rate || h.p2 != a.format.channels || h.p0 != static_cast<unsigned>(a.use) ||
                    ((h.flags & cp::kFlagOpaquePayload) != 0) != (a.format.codec != w::AudioCodec::Pcm) ||
                    (a.format.codec == w::AudioCodec::Pcm && bytes.size() != std::size_t(h.p3) * a.format.channels * (a.format.bits / 8))) { ++dropped; return; }
                it->second.sequence = h.sequence;
            }
            w::MediaPacket p{generation, h.stream_id, h.pts, key, w::Bytes(bytes.begin(), bytes.end()), w::MediaPacket::Timebase::MonotonicMicroseconds};
            auto s = endpoint->push_media(generation, std::move(p));
            if (!s) { ++dropped; if (h.type == cp::Type::VideoFrame) video.at(h.stream_id).wait_key = true; }
            else if (h.type == cp::Type::VideoFrame) video.at(h.stream_id).wait_key = false;
            return;
        }
        if (h.type == cp::Type::AudioGain) {
            const auto volume = h.p1 == 0 ? -144.0 : std::max(-144.0, 20.0 * std::log10(double(h.p1) / 1000000.0));
            auto t = endpoint->request_output(generation, w::SendControl{{h.p1 == 1000000 ? w::CommandType::UnduckAudio : w::CommandType::DuckAudio, w::DuckAudio{double(h.p0), volume}}});
            if (!t.status) ++dropped;
            return;
        }
        if (auto s = service(static_cast<cp::ext::DType>(h.type))) {
            auto head = cp::encode_header(h); w::Bytes blob(head.begin(), head.end()); blob.insert(blob.end(), bytes.begin(), bytes.end());
            auto t = endpoint->request_output(generation, w::SendService{{*s, "zero2w.cpmf.v1", std::move(blob)}});
            if (!t.status) ++dropped;
        } else ++unsupported;
    }
};
CatPlayForwardCapture::CatPlayForwardCapture(std::shared_ptr<WirelessEndpoint> e, std::uint32_t fps) {
    if (!e || !fps || fps > 240) throw std::invalid_argument("capture requires endpoint and actual configured frame rate");
    impl_ = std::make_unique<Impl>(std::move(e), fps);
}
CatPlayForwardCapture::~CatPlayForwardCapture() = default;
void CatPlayForwardCapture::on_catplay_record(const cp::Header& h, std::span<const std::uint8_t> p) noexcept {
    try {
        // Legacy CPMF has no execution-result channel. Reclaim results but do not
        // claim they were delivered back to the phone; unsupported results are counted.
        while (auto c = impl_->endpoint->pop_phone_completion()) if (!c->status) ++impl_->unsupported;
        impl_->handle(h, p);
    } catch (...) { ++impl_->dropped; impl_->reset(); }
}
void CatPlayForwardCapture::on_catplay_disconnect() noexcept { impl_->reset(); }
CaptureSnapshot CatPlayForwardCapture::snapshot() const { return {impl_->records.load(), impl_->dropped.load(), impl_->unsupported.load()}; }
} // namespace zero2w::forward
