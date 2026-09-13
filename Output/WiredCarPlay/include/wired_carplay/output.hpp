#pragma once

#include "wired_carplay/catalog.hpp"
#include "wired_carplay/metadata.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace zero2w::wired {
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Milliseconds = std::chrono::milliseconds;
using Epoch = std::uint64_t;
using RequestId = std::uint64_t;
using StreamId = std::uint32_t;

enum class Error {
    Ok, InvalidArgument, InvalidState, NotSupported, Backpressure, NotFound,
    StaleSession, Timeout, Cancelled, Disconnected, AuthenticationFailed,
    TransportFailure, PeerRejected, ProtocolError
};
struct Status {
    Error code{Error::Ok};
    std::string detail;
    explicit operator bool() const noexcept { return code == Error::Ok; }
};
// A ticket acknowledges local admission ONLY. Read pop_completion() for execution result.
struct Ticket { Status status; Epoch epoch{}; RequestId id{}; };
struct Completion { Epoch epoch{}; RequestId id{}; Status status; Bytes response; };

enum class Phase {
    Idle, Starting, GadgetEnabled, RoleSwitching, AccessoryDetected,
    Iap2Negotiating, Authenticating, Identifying, NcmConfiguring, Discovering,
    Pairing, RtspConnecting, Ready, Streaming, Stopping, Closed, Failed
};
enum class UsbRole { Peripheral, Host, Negotiated };
struct UsbConfiguration {
    std::string udc; // Explicit controller selection; never automatically activate management USB.
    std::string serial;
    std::string manufacturer;
    std::string product;
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    UsbRole role{UsbRole::Negotiated};
};
struct StartSession { UsbConfiguration usb; std::string pairing_store; };
struct StopSession {};

enum class VideoCodec { H264AnnexB, H264Avcc, HevcAnnexB, HevcHvcc };
enum class DisplayRole { Main, Alternate, InstrumentCluster };
// Modern roles/codecs are expressible, not advertised until a backend and peer support them.
struct ScreenConfiguration {
    StreamId id{};
    std::string display_uuid;
    DisplayRole role{DisplayRole::Main};
    VideoCodec codec{VideoCodec::H264AnnexB};
    std::uint32_t width{}, height{}, fps{}, latency_ms{};
    Bytes codec_configuration;
};
enum class AudioStream : std::uint16_t {
    General = 96, Main = 100, Alternate = 101, MainHigh = 102,
    Buffered = 103, AuxiliaryOutput = 106, AuxiliaryInput = 107
};
enum class AudioUse { Default, Alert, Media, Telephony, SpeechRecognition, Compatibility };
enum class AudioCodec { Pcm, Alac, AacLc, AacEld, Opus };
struct AudioFormat {
    AudioCodec codec{AudioCodec::Pcm};
    std::uint32_t sample_rate{48000};
    std::uint8_t bits{16}, channels{2};
    bool operator==(const AudioFormat&) const = default;
};
// Exact single-format bit mapping from the reference streams.rs (bits 2..32).
// A mask with zero/multiple/unknown bits is not a selected stream format.
std::optional<std::uint64_t> wire_audio_format(const AudioFormat& format);
std::optional<AudioFormat> audio_format_from_wire(std::uint64_t bit);
struct AudioConfiguration {
    StreamId id{};
    AudioStream stream{AudioStream::Main};
    AudioUse use{AudioUse::Media};
    AudioFormat format;
    std::uint32_t latency_ms{};
    bool playback{true};       // Output -> car speakers.
    bool microphone{false};   // Car -> Output; never a local ALSA assumption.
};
struct TeardownStream { StreamId id{}; };
struct Record {};
struct DrainTeardown {};

enum class Entity : std::uint8_t { None, Device, Accessory };
enum class SpeechMode : std::int8_t { None = -1, Speaking = 1, Recognizing = 2 };
struct ResourceOwner { Entity current{Entity::Device}, permanent{Entity::Device}; };
struct ModeState {
    ResourceOwner screen, main_audio;
    Entity phone_call{Entity::None}, navigation{Entity::None}, speech{Entity::None};
    SpeechMode speech_mode{SpeechMode::None};
};
struct AssertModes { ModeState state; };
struct Empty {};
struct DuckAudio { double duration_ms{}, volume_db{}; };
struct BluetoothDevice { std::string address; };
struct HidReport { std::string uuid; Bytes report; std::optional<std::uint64_t> ntp_timestamp; };
enum class HidInputMode : std::uint8_t { Default, Character, Scrolling, ScrollingWithCharacters, DialPad };
struct SetHidInputMode { std::string uuid; HidInputMode mode{HidInputMode::Default}; };
enum class SiriAction : std::uint8_t { Prewarm = 1, ButtonDown, ButtonUp };
struct SiriRequest { SiriAction action{SiriAction::Prewarm}; };
struct UiRequest { std::optional<std::string> url; };
struct Toggle { bool enabled{}; };
// Full original plist retained for resource constraints and forward-compatible unknown fields.
// This interface does not parse or synthesize plist. The protocol backend validates it.
struct BinaryPlist { Bytes data; };
using ControlPayload = std::variant<Empty, DuckAudio, BluetoothDevice, ModeState,
    HidReport, SetHidInputMode, SiriRequest, UiRequest, Toggle, BinaryPlist, Bytes>;
struct ControlCommand { CommandType type; ControlPayload payload; };
bool validate_control_command(const ControlCommand&, std::size_t max_payload_bytes = 1024 * 1024);
struct SendControl { ControlCommand command; };
// Owns the full CSM parameter body (including unknown TLVs), not a serialized C++ struct.
// Packet framing, protocol direction and session routing are backend responsibilities.
struct Iap2Message { CsmType type; std::uint16_t session_id{}; Bytes parameters; };
struct SendIap2 { Iap2Message message; };
enum class Service {
    NowPlaying, Artwork, Lyrics, Navigation, Contacts, Recents, Favorites,
    MediaLibrary, PlaybackQueue, CallState, Communications, VehicleInformation,
    VehicleStatus, Location, Power, DeviceNotifications, ExternalAccessory,
    FileTransfer, Diagnostics, DisplayPanels, ViewArea, UiContext, Appearance,
    MapAppearance, MapZoom, Vocoder, Haptics, HidSetReport, FlushAudio,
    DigitalCarKeys, WifiConfiguration
};
// Extension/metadata envelope: schema is versioned by its future backend, never guessed here.
struct ServiceMessage { Service service; std::string schema; ServicePayload payload; };
bool validate_service_message(const ServiceMessage&, std::size_t max_payload_bytes = 1024 * 1024);
struct SendService { ServiceMessage message; };
enum class PairingAction { PairSetup, PairVerify, AuthSetup, ForgetPeer };
struct PairingRequest { PairingAction action; std::string peer_id; };
using Operation = std::variant<StartSession, StopSession, ScreenConfiguration,
    AudioConfiguration, TeardownStream, Record, DrainTeardown, AssertModes,
    SendControl, SendIap2, SendService, PairingRequest>;
struct WorkItem { Epoch epoch{}; RequestId id{}; Operation operation; TimePoint deadline; };

struct DisplayInfo {
    std::string uuid;
    std::uint32_t width{}, height{}, width_mm{}, height_mm{}, max_fps{};
    std::uint32_t input_features{}; // Raw peer flags (knob/touch/touchpad).
    Bytes edid;
};
struct HidDeviceInfo { std::string uuid, display_uuid, name; std::uint16_t vendor{}, product{}, country{}; Bytes descriptor; };
struct Capabilities {
    std::vector<CommandType> commands;
    std::vector<CsmType> csm_messages;
    std::vector<Service> services;
    std::vector<VideoCodec> video_codecs;
    std::vector<AudioFormat> audio_formats;
    std::vector<AudioStream> audio_streams;
    std::vector<DisplayRole> display_roles;
    bool microphone{}, pairing_management{};
    bool monotonic_media_timestamps{}; // Backend can map host CLOCK_MONOTONIC microseconds to the peer clock.
};
struct PeerInfo {
    std::string device_id, name, manufacturer, model, firmware, source_version;
    std::string network_interface, peer_address;
    UsbRole role{UsbRole::Negotiated};
    std::uint64_t feature_bits{}, status_flags{};
    std::vector<std::string> controller_features, extended_features, bluetooth_ids;
    std::vector<DisplayInfo> displays;
    std::vector<HidDeviceInfo> hid_devices;
    bool night_mode{}, limited_ui{}, right_hand_drive{};
    ModeState modes;
    BinaryPlist raw_info; // Preserves latencies/OEM icons/limited UI elements and future fields.
    Capabilities negotiated;
};
struct MediaClock { std::uint64_t ntp_timestamp{}, sample_time{}; std::uint32_t sample_rate{}; TimePoint measured_at{}; };
struct MediaPacket {
    Epoch epoch{};
    StreamId stream_id{};
    std::uint64_t timestamp{}; // Video NTP 64-bit; audio sample counter in negotiated sample rate.
    bool key_frame{};
    Bytes data;
    enum class Timebase { StreamClock, MonotonicMicroseconds } timebase{Timebase::StreamClock};
};
struct IncomingControl { Epoch epoch{}; RequestId id{}; ControlCommand command; TimePoint deadline; };
using IncomingPayload = std::variant<IncomingControl, Iap2Message, ServiceMessage>;
struct IncomingEvent { Epoch epoch{}; IncomingPayload payload; };
struct ControlReply { Epoch epoch{}; RequestId id{}; Status status; Bytes response; };
struct Limits {
    std::size_t pending_requests{32}, inbound_requests{32}, events{64};
    std::size_t media_packets{64}, media_bytes{8 * 1024 * 1024};
    std::size_t microphone_packets{64}, microphone_bytes{256 * 1024};
    std::size_t max_payload_bytes{1024 * 1024}, max_streams{8};
};
struct Snapshot {
    Epoch epoch{};
    Phase phase{Phase::Idle};
    Status last_error;
    std::size_t pending_requests{}, completed_requests{}, incoming_requests{}, events{};
    std::size_t media_packets{}, media_bytes{}, microphone_packets{}, microphone_bytes{}, active_streams{};
    std::uint64_t rejected{}, packets_taken_by_backend{}, received_microphone_packets{};
};

// Backend provider contracts only. There is deliberately no always-success auth or USB driver.
class AuthenticationProvider {
public:
    virtual ~AuthenticationProvider() = default;
    virtual Status certificate(Bytes& certificate) = 0;
    virtual Status sign_challenge(const Bytes& challenge, Bytes& signature) = 0;
    virtual Status verify_certificate(const Bytes& certificate) = 0;
    virtual Status verify_response(const Bytes& certificate, const Bytes& challenge, const Bytes& signature) = 0;
};
class PairingStore {
public:
    virtual ~PairingStore() = default;
    virtual Status load(const std::string& peer_id, Bytes& record) = 0;
    virtual Status save(const std::string& peer_id, const Bytes& record) = 0;
    virtual Status erase(const std::string& peer_id) = 0;
};

// Thread safe, bounded, nonblocking interface broker. No worker thread, sockets, gadget changes,
// codec conversion or Core dependency. One backend consumes take_work/take_media/take_reply.
// Call expire(now) from the backend's timer; destroying this object does NOT stop hardware.
class WiredCarPlayOutput {
public:
    explicit WiredCarPlayOutput(Limits limits = {});
    ~WiredCarPlayOutput();
    WiredCarPlayOutput(const WiredCarPlayOutput&) = delete;
    WiredCarPlayOutput& operator=(const WiredCarPlayOutput&) = delete;

    Ticket start(StartSession config, Milliseconds timeout = Milliseconds{10000});
    Ticket shutdown(Milliseconds timeout = Milliseconds{3000});
    Ticket submit(Operation operation, Milliseconds timeout = Milliseconds{3000});
    Status cancel(Epoch epoch, RequestId id);
    std::optional<Completion> pop_completion();
    std::optional<Completion> pop_completion(Epoch epoch, RequestId id);
    Status discard_media(Epoch epoch); // Selection handoff: purge queued playback AND microphone data.
    Snapshot snapshot() const;
    std::optional<PeerInfo> info_cached() const;
    std::optional<MediaClock> media_clock() const;
    Status push_media(MediaPacket packet);
    std::optional<IncomingEvent> pop_event();
    std::optional<MediaPacket> pop_microphone();
    Status respond(Epoch epoch, RequestId id, Status status, Bytes response = {});

    // Backend port. complete() is the only route from accepted to successfully executed.
    std::optional<WorkItem> take_work();
    Status complete(Epoch epoch, RequestId id, Status result, Bytes response = {},
                    std::optional<PeerInfo> peer = std::nullopt);
    Status report_progress(Epoch epoch, Phase phase);
    Status disconnect(Epoch epoch, Status reason = {Error::Disconnected, {}});
    Status update_clock(Epoch epoch, MediaClock clock);
    std::optional<MediaPacket> take_media();
    Ticket receive_control(Epoch epoch, ControlCommand command, Milliseconds timeout = Milliseconds{3000});
    Status receive_iap2(Epoch epoch, Iap2Message message);
    Status receive_service(Epoch epoch, ServiceMessage message);
    Status receive_microphone(MediaPacket packet);
    std::optional<ControlReply> take_reply(TimePoint now = Clock::now());
    void expire(TimePoint now = Clock::now());

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace zero2w::wired
