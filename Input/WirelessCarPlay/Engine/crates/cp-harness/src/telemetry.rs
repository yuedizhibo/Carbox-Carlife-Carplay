//! Versioned, bounded protocol telemetry shared by the protocol engine,
//! `cp-native`, and the separate C++ core process.
//!
//! Values describe observed protocol state only. A field is `unsupported`,
//! `not-offered`, or `not-negotiated` until the engine has evidence otherwise;
//! no UI layer may translate that into success.

use std::collections::VecDeque;

use serde::{Deserialize, Serialize};

/// Compatibility version of the line-JSON state/event contract.
pub const TELEMETRY_SCHEMA_VERSION: u16 = 1;
pub const TELEMETRY_TEXT_CAP: usize = 160;
pub const ARTWORK_BYTES_CAP: usize = 64 * 1024;
pub const MEDIA_FRAME_CACHE_CAP: usize = 4;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "kebab-case")]
pub enum OfferState {
    Unsupported,
    NotOffered,
    #[default]
    NotNegotiated,
    Negotiating,
    Active,
    Failed,
}

impl OfferState {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Unsupported => "unsupported",
            Self::NotOffered => "not-offered",
            Self::NotNegotiated => "not-negotiated",
            Self::Negotiating => "negotiating",
            Self::Active => "active",
            Self::Failed => "failed",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct FeatureMatrix {
    pub bluetooth: OfferState,
    pub wifi_bonjour: OfferState,
    pub iap2: OfferState,
    pub mfi: OfferState,
    pub hap: OfferState,
    pub rtsp: OfferState,
    pub wired_usb: OfferState,
    pub main_screen: OfferState,
    pub second_screen: OfferState,
    pub instrument_screen: OfferState,
    pub media_audio: OfferState,
    pub call_audio: OfferState,
    pub siri: OfferState,
    pub prompt_audio: OfferState,
    pub microphone: OfferState,
    pub hid: OfferState,
    pub now_playing: OfferState,
    pub lyrics: OfferState,
    pub gps: OfferState,
    pub vehicle: OfferState,
    pub navigation: OfferState,
    pub phone: OfferState,
}

impl Default for FeatureMatrix {
    fn default() -> Self {
        Self {
            bluetooth: OfferState::Unsupported,
            wifi_bonjour: OfferState::NotNegotiated,
            iap2: OfferState::NotNegotiated,
            mfi: OfferState::NotNegotiated,
            hap: OfferState::NotNegotiated,
            rtsp: OfferState::NotNegotiated,
            wired_usb: OfferState::NotNegotiated,
            main_screen: OfferState::NotNegotiated,
            second_screen: OfferState::NotOffered,
            instrument_screen: OfferState::NotOffered,
            media_audio: OfferState::NotNegotiated,
            call_audio: OfferState::NotOffered,
            siri: OfferState::NotOffered,
            prompt_audio: OfferState::NotOffered,
            microphone: OfferState::NotOffered,
            hid: OfferState::NotNegotiated,
            now_playing: OfferState::NotOffered,
            lyrics: OfferState::NotOffered,
            gps: OfferState::NotOffered,
            vehicle: OfferState::NotOffered,
            navigation: OfferState::NotOffered,
            phone: OfferState::NotOffered,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct ScreenTelemetry {
    pub state: OfferState,
    pub codec: String,
    pub width: u16,
    pub height: u16,
    pub fps: u16,
    pub latency_ms: u32,
    pub dropped_frames: u64,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct NowPlaying {
    pub title: String,
    pub artist: String,
    pub album: String,
    /// Base64 is intentionally not embedded in status. The core retrieves a
    /// bounded artwork object through the binary media path when available.
    pub artwork_bytes: usize,
    pub lyrics: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CarPlayTelemetry {
    pub schema_version: u16,
    pub session_id: u64,
    /// Last accepted CatPlay lifecycle string; empty before the engine reports.
    pub lifecycle: String,
    pub wireless_listener_ready: bool,
    pub wired_endpoint_ready: bool,
    pub iphone_connected: bool,
    pub vehicle_connected: bool,
    pub bridge_active: bool,
    pub connected: bool,
    pub features: FeatureMatrix,
    pub main_screen: ScreenTelemetry,
    pub second_screen: ScreenTelemetry,
    pub instrument_screen: ScreenTelemetry,
    pub audio_focus: String,
    pub now_playing: NowPlaying,
    pub reconnects: u32,
    pub last_error: String,
}

impl Default for CarPlayTelemetry {
    fn default() -> Self {
        Self {
            schema_version: TELEMETRY_SCHEMA_VERSION,
            session_id: 0,
            lifecycle: "initial".to_string(),
            wireless_listener_ready: false,
            wired_endpoint_ready: false,
            iphone_connected: false,
            vehicle_connected: false,
            bridge_active: false,
            connected: false,
            features: FeatureMatrix::default(),
            main_screen: ScreenTelemetry::default(),
            second_screen: ScreenTelemetry::default(),
            instrument_screen: ScreenTelemetry::default(),
            audio_focus: "none".to_string(),
            now_playing: NowPlaying::default(),
            reconnects: 0,
            last_error: String::new(),
        }
    }
}

/// Clamp an untrusted text field at a UTF-8 boundary.
pub fn bounded_text(value: &str) -> String {
    if value.len() <= TELEMETRY_TEXT_CAP {
        return value.to_string();
    }
    let mut end = TELEMETRY_TEXT_CAP;
    while end > 0 && !value.is_char_boundary(end) {
        end -= 1;
    }
    value[..end].to_string()
}

impl CarPlayTelemetry {
    pub fn set_error(&mut self, value: &str) {
        self.last_error = bounded_text(value);
    }

    pub fn set_now_playing(&mut self, title: &str, artist: &str, album: &str, lyrics: &str, artwork_bytes: usize) {
        self.now_playing = NowPlaying {
            title: bounded_text(title),
            artist: bounded_text(artist),
            album: bounded_text(album),
            lyrics: bounded_text(lyrics),
            artwork_bytes: artwork_bytes.min(ARTWORK_BYTES_CAP),
        };
    }
}

/// Descriptor only: encoded media stays on a separate binary socket/HTTP
/// stream. Slow clients receive a future keyframe, never stall the phone.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct MediaFrame {
    pub sequence: u64,
    pub keyframe: bool,
    pub bytes: usize,
}

#[derive(Debug, Default)]
pub struct MediaFrameCache {
    frames: VecDeque<MediaFrame>,
    dropped: u64,
}

impl MediaFrameCache {
    pub fn push(&mut self, frame: MediaFrame) {
        if frame.bytes > ARTWORK_BYTES_CAP * 16 {
            self.dropped = self.dropped.saturating_add(1);
            return;
        }
        if self.frames.len() == MEDIA_FRAME_CACHE_CAP {
            self.frames.pop_front();
            self.dropped = self.dropped.saturating_add(1);
        }
        self.frames.push_back(frame);
    }

    /// A slow preview client must restart from a keyframe. No matching frame
    /// means it waits; it never asks the protocol ingest path to retain more.
    pub fn resync_from_keyframe(&self, after: u64) -> Option<MediaFrame> {
        self.frames.iter().copied().find(|frame| frame.sequence > after && frame.keyframe)
    }

    pub fn len(&self) -> usize {
        self.frames.len()
    }
    pub fn is_empty(&self) -> bool {
        self.frames.is_empty()
    }
    pub fn dropped(&self) -> u64 {
        self.dropped
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_are_truthful_about_unoffered_screens() {
        let state = CarPlayTelemetry::default();
        assert_eq!(state.schema_version, TELEMETRY_SCHEMA_VERSION);
        assert_eq!(state.features.second_screen, OfferState::NotOffered);
        assert_eq!(state.features.instrument_screen, OfferState::NotOffered);
        assert!(!state.connected);
    }

    #[test]
    fn metadata_and_errors_are_bounded_and_artwork_is_not_inlined() {
        let mut state = CarPlayTelemetry::default();
        state.set_now_playing(&"t".repeat(400), "artist", "album", &"l".repeat(400), ARTWORK_BYTES_CAP * 2);
        state.set_error(&"e".repeat(400));
        assert_eq!(state.now_playing.title.len(), TELEMETRY_TEXT_CAP);
        assert_eq!(state.now_playing.lyrics.len(), TELEMETRY_TEXT_CAP);
        assert_eq!(state.now_playing.artwork_bytes, ARTWORK_BYTES_CAP);
        assert_eq!(state.last_error.len(), TELEMETRY_TEXT_CAP);
        let json = serde_json::to_string(&state).expect("json");
        assert!(!json.contains("base64"));
    }

    #[test]
    fn slow_preview_drops_and_resyncs_only_on_a_keyframe() {
        let mut cache = MediaFrameCache::default();
        for sequence in 1..=6 {
            cache.push(MediaFrame { sequence, keyframe: sequence == 5, bytes: 100 });
        }
        assert_eq!(cache.len(), MEDIA_FRAME_CACHE_CAP);
        assert_eq!(cache.dropped(), 2);
        assert_eq!(cache.resync_from_keyframe(1), Some(MediaFrame { sequence: 5, keyframe: true, bytes: 100 }));
        assert_eq!(cache.resync_from_keyframe(5), None);
    }
}
