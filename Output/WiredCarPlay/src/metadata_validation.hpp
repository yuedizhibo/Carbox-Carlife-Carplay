#pragma once
#include "wired_carplay/output.hpp"
#include <cmath>
#include <type_traits>
namespace zero2w::wired::detail {
inline bool valid_service(const ServiceMessage& m, std::size_t max) {
    if (m.schema.empty() || m.schema.size() > 1024 || static_cast<unsigned>(m.service) > static_cast<unsigned>(Service::WifiConfiguration)) return false;
    std::size_t size = 0;
    bool valid = true;
    auto text = [&](const std::string& s) { if (s.size() > 1024) valid = false; size += s.size(); };
    auto texts = [&](const auto& v) { if (v.size() > 256) { valid = false; return; } for (const auto& s : v) text(s); };
    auto item = [&](const MediaItem& p) { text(p.id); text(p.title); text(p.artist); text(p.album); text(p.genre); text(p.composer); text(p.artwork_id); };
    auto finite = [](double v) { return std::isfinite(v); };
    const bool shape = std::visit([&](const auto& p) -> bool {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, Bytes>) {
            size = p.size(); return true; // Opaque negotiated schema, validated by its backend.
        } else if constexpr (std::is_same_v<T, NowPlaying>) {
            item(p.item);
            return m.service == Service::NowPlaying && static_cast<unsigned>(p.state) <= 4 && static_cast<unsigned>(p.repeat) <= 2 &&
                finite(p.playback_rate) && p.playback_rate >= 0 && p.playback_rate <= 32 && (!p.queue_count || p.queue_index < p.queue_count);
        } else if constexpr (std::is_same_v<T, Artwork>) {
            text(p.id); text(p.mime_type); size += p.data.size();
            return m.service == Service::Artwork && !p.id.empty() && !p.mime_type.empty() && p.width && p.height && !p.data.empty();
        } else if constexpr (std::is_same_v<T, Lyrics>) {
            text(p.track_id); text(p.language);
            if (p.lines.size() > 256) return false;
            std::uint64_t last{};
            for (const auto& line : p.lines) { text(line.text); if (line.start_ms < last || line.end_ms < line.start_ms) return false; last = line.start_ms; }
            return m.service == Service::Lyrics;
        } else if constexpr (std::is_same_v<T, Navigation>) {
            text(p.route_id); text(p.road); text(p.next_road); text(p.instruction); text(p.maneuver); text(p.destination);
            return m.service == Service::Navigation && finite(p.distance_to_maneuver_m) && p.distance_to_maneuver_m >= 0 && finite(p.remaining_distance_m) && p.remaining_distance_m >= 0;
        } else if constexpr (std::is_same_v<T, ContactDirectory>) {
            text(p.revision); texts(p.removed_ids);
            if (p.entries.size() > 256) return false;
            for (const auto& c : p.entries) { text(c.id); text(c.display_name); texts(c.phone_numbers); texts(c.emails); }
            return m.service == Service::Contacts || m.service == Service::Favorites;
        } else if constexpr (std::is_same_v<T, RecentCalls>) {
            text(p.revision); if (p.entries.size() > 256) return false;
            for (const auto& c : p.entries) { text(c.id); text(c.contact_id); text(c.phone_number); }
            return m.service == Service::Recents;
        } else if constexpr (std::is_same_v<T, MediaLibrary>) {
            text(p.revision); texts(p.removed_ids); if (p.entries.size() > 256) return false;
            for (const auto& c : p.entries) item(c);
            return m.service == Service::MediaLibrary || m.service == Service::PlaybackQueue;
        } else if constexpr (std::is_same_v<T, CallState>) {
            if (p.calls.size() > 16) return false;
            for (const auto& c : p.calls) { text(c.id); text(c.phone_number); text(c.display_name); if (static_cast<unsigned>(c.status) > 5) return false; }
            return m.service == Service::CallState;
        } else if constexpr (std::is_same_v<T, Communications>) {
            text(p.carrier); text(p.service); return m.service == Service::Communications && p.signal_percent <= 100;
        } else if constexpr (std::is_same_v<T, VehicleInformation>) {
            text(p.manufacturer); text(p.model); text(p.vin); text(p.engine_type);
            return m.service == Service::VehicleInformation && p.year <= 9999;
        } else if constexpr (std::is_same_v<T, VehicleStatus>) {
            return m.service == Service::VehicleStatus && (!p.speed_mps || (finite(*p.speed_mps) && *p.speed_mps >= 0)) && (!p.outside_temperature_c || finite(*p.outside_temperature_c));
        } else if constexpr (std::is_same_v<T, Location>) {
            return m.service == Service::Location && finite(p.latitude) && p.latitude >= -90 && p.latitude <= 90 &&
                finite(p.longitude) && p.longitude >= -180 && p.longitude <= 180 && finite(p.altitude_m) &&
                finite(p.speed_mps) && p.speed_mps >= 0 && finite(p.heading_degrees) && p.heading_degrees >= 0 && p.heading_degrees < 360 && finite(p.horizontal_accuracy_m) && p.horizontal_accuracy_m >= 0;
        } else if constexpr (std::is_same_v<T, Power>) {
            return m.service == Service::Power && p.battery_percent <= 100;
        } else if constexpr (std::is_same_v<T, DeviceNotification>) {
            text(p.device_id); text(p.name); text(p.language); text(p.transport_id); return m.service == Service::DeviceNotifications;
        } else if constexpr (std::is_same_v<T, ExternalAccessoryData>) {
            text(p.protocol); size += p.data.size(); return m.service == Service::ExternalAccessory && !p.protocol.empty();
        } else if constexpr (std::is_same_v<T, FileChunk>) {
            text(p.transfer_id); text(p.mime_type); size += p.data.size();
            return (m.service == Service::FileTransfer || m.service == Service::Diagnostics) && !p.transfer_id.empty() &&
                p.offset <= p.total_size && p.data.size() <= p.total_size - p.offset && (!p.last || p.data.size() == p.total_size - p.offset);
        } else if constexpr (std::is_same_v<T, DisplayPanels>) {
            if (p.panels.size() > 16) return false;
            for (const auto& d : p.panels) { text(d.uuid); if (d.uuid.empty() || !d.width || !d.height) return false; }
            return m.service == Service::DisplayPanels || m.service == Service::ViewArea;
        } else if constexpr (std::is_same_v<T, Vocoder>) {
            text(p.codec); return m.service == Service::Vocoder && !p.codec.empty() && p.sample_rate && p.sample_rate <= 384000;
        }
        return false;
    }, m.payload);
    return shape && valid && size <= max;
}
} // namespace zero2w::wired::detail
