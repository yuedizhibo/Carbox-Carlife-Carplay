#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace zero2w::wired {
// Product-side data contracts, NOT CarPlay wire layouts. No conversion/Core dependency.
enum class PlaybackState { Stopped, Playing, Paused, SeekingForward, SeekingBackward };
struct MediaItem {
    std::string id, title, artist, album, genre, composer, artwork_id;
    std::uint64_t duration_ms{};
    std::uint32_t track_number{}, disc_number{};
};
struct NowPlaying {
    MediaItem item;
    PlaybackState state{PlaybackState::Stopped};
    std::uint64_t elapsed_ms{}, queue_index{}, queue_count{};
    double playback_rate{1};
    bool shuffle{};
    enum class Repeat { Off, One, All } repeat{Repeat::Off};
};
struct Artwork {
    std::string id, mime_type;
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> data;
};
struct LyricLine { std::uint64_t start_ms{}, end_ms{}; std::string text; };
struct Lyrics { std::string track_id, language; std::vector<LyricLine> lines; };
struct Navigation {
    std::string route_id, road, next_road, instruction, maneuver, destination;
    double distance_to_maneuver_m{}, remaining_distance_m{};
    std::uint64_t remaining_time_s{};
    bool active{};
};
struct Contact { std::string id, display_name; std::vector<std::string> phone_numbers, emails; };
struct ContactDirectory { std::string revision; bool replace{}; std::vector<Contact> entries; std::vector<std::string> removed_ids; };
struct RecentCall { std::string id, contact_id, phone_number; std::uint64_t unix_time_s{}, duration_s{}; bool incoming{}, missed{}; };
struct RecentCalls { std::string revision; bool replace{}; std::vector<RecentCall> entries; };
struct MediaLibrary { std::string revision; bool replace{}; std::vector<MediaItem> entries; std::vector<std::string> removed_ids; };
enum class CallStatus { Idle, Dialing, Ringing, Active, Held, Ended };
struct Call { std::string id, phone_number, display_name; CallStatus status{CallStatus::Idle}; bool incoming{}, muted{}; };
struct CallState { std::vector<Call> calls; };
struct Communications { std::string carrier, service; std::uint8_t signal_percent{}; bool registered{}, roaming{}; };
struct VehicleInformation { std::string manufacturer, model, vin, engine_type; std::uint32_t year{}; bool right_hand_drive{}; };
struct VehicleStatus { bool parking_brake{}, reverse_gear{}; std::optional<double> speed_mps, outside_temperature_c; };
struct Location {
    double latitude{}, longitude{}, altitude_m{}, speed_mps{}, heading_degrees{}, horizontal_accuracy_m{};
    std::uint64_t unix_time_ms{};
    bool valid{};
};
struct Power { std::uint8_t battery_percent{}; bool charging{}, external_power{}; std::uint32_t available_ma{}; };
struct DeviceNotification { std::string device_id, name, language, transport_id; std::uint64_t unix_time_ms{}; };
struct ExternalAccessoryData { std::string protocol; std::uint16_t session_id{}; std::vector<std::uint8_t> data; };
struct FileChunk { std::string transfer_id, mime_type; std::uint64_t offset{}, total_size{}; bool last{}; std::vector<std::uint8_t> data; };
struct DisplayPanel { std::string uuid; std::int32_t x{}, y{}; std::uint32_t width{}, height{}; };
struct DisplayPanels { std::vector<DisplayPanel> panels; };
struct Vocoder { std::string codec; std::uint32_t sample_rate{}, bitrate{}; };
using ServicePayload = std::variant<std::vector<std::uint8_t>, NowPlaying, Artwork, Lyrics, Navigation,
    ContactDirectory, RecentCalls, MediaLibrary, CallState, Communications, VehicleInformation,
    VehicleStatus, Location, Power, DeviceNotification, ExternalAccessoryData, FileChunk, DisplayPanels, Vocoder>;
} // namespace zero2w::wired
