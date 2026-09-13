#pragma once

// Read-only reference catalog for the standalone Wired CarPlay Output library.
//
// Every entry below is transcribed by hand from the read-only CatPlay reference
// tree:
//   Reference/CatPlaySource/carplay/catplay_carplay/src/msg/commands/command_types.rs
//   Reference/CatPlaySource/core/catplay_csm/src/msg/*.rs
//
// Nothing here is generated at build time and no reference code is imported.
// The catalog exists so callers can look up wire names, directions and CSM
// packet IDs without depending on the reference implementation. Types and
// serializers are intentionally out of scope.

#include <array>
#include <cstdint>
#include <string_view>

namespace zero2w::wired {

// Direction of a wire command, stated from the point of view of this Output
// library. Device is the phone-emulation implementation we provide; Accessory is
// the vehicle (head unit) on the other end of the wire.
enum class Direction {
  DeviceToAccessory,
  AccessoryToDevice,
  Bidirectional,
  // CSM direction must be validated by the eventual protocol backend.
  ProtocolDefined,
};

enum class CommandType {
  DuckAudio,
  UnduckAudio,
  DisableBluetooth,
  ChangeModes,
  ModesChanged,
  ForceKeyFrame,
  HidSendReport,
  HidSetInputMode,
  RequestSiri,
  RequestUI,
  SetNightMode,
  SetLimitedUI,
  IApSendMessage,
};

struct CommandDescriptor {
  CommandType type;
  std::string_view wire_name;
  Direction direction;
};

// The 13 implemented wire commands declared by CommandType in command_types.rs,
// in the same order as the reference enum. Wire names are case sensitive.
inline constexpr std::array command_catalog{
    CommandDescriptor{CommandType::DuckAudio, "duckAudio", Direction::DeviceToAccessory},
    CommandDescriptor{CommandType::UnduckAudio, "unduckAudio", Direction::DeviceToAccessory},
    CommandDescriptor{CommandType::DisableBluetooth, "disableBluetooth", Direction::DeviceToAccessory},
    CommandDescriptor{CommandType::ChangeModes, "changeModes", Direction::AccessoryToDevice},
    CommandDescriptor{CommandType::ModesChanged, "modesChanged", Direction::DeviceToAccessory},
    CommandDescriptor{CommandType::ForceKeyFrame, "forceKeyFrame", Direction::AccessoryToDevice},
    CommandDescriptor{CommandType::HidSendReport, "hidSendReport", Direction::AccessoryToDevice},
    CommandDescriptor{CommandType::HidSetInputMode, "hidSetInputMode", Direction::DeviceToAccessory},
    CommandDescriptor{CommandType::RequestSiri, "requestSiri", Direction::AccessoryToDevice},
    CommandDescriptor{CommandType::RequestUI, "requestUI", Direction::AccessoryToDevice},
    CommandDescriptor{CommandType::SetNightMode, "setNightMode", Direction::AccessoryToDevice},
    CommandDescriptor{CommandType::SetLimitedUI, "setLimitedUI", Direction::AccessoryToDevice},
    CommandDescriptor{CommandType::IApSendMessage, "iAPSendMessage", Direction::Bidirectional},
};

// Returns the descriptor for an implemented command, or nullptr when the value
// is not present in the catalog, including out-of-range casts.
inline constexpr const CommandDescriptor* describe(CommandType type) {
  for (const CommandDescriptor& descriptor : command_catalog) {
    if (descriptor.type == type) return &descriptor;
  }
  return nullptr;
}

// Every named CSM packet message declared with a hexadecimal packet ID in
// core/catplay_csm/src/msg/*.rs. Structs without a packet ID are helper
// structures, not messages, and are deliberately absent. Spelling is preserved
// exactly as in the reference.
enum class CsmType : std::uint16_t {
  RequestAuthenticationCertificate = 0xAA00,
  AuthenticationCertificate = 0xAA01,
  RequestAuthenticationChallengeResponse = 0xAA02,
  AuthenticationResponse = 0xAA03,
  AuthenticationFailed = 0xAA04,
  AuthenticationSucceeded = 0xAA05,
  AccessoryAuthenticationSerialNumber = 0xAA06,

  CarPlayAvailability = 0x4300,
  CarPlayStartSession = 0x4301,
  AvailableDigitalCarKeys = 0x4302,
  MatchedDigitalCarKeys = 0x4303,

  StartIdentification = 0x1D00,
  IdentificationInformation = 0x1D01,
  IdentificationAccepted = 0x1D02,
  IdentificationRejected = 0x1D03,
  CancelIdentification = 0x1D05,
  IdentificationInformationUpdate = 0x1D06,

  StartNowPlayingUpdates = 0x5000,
  NowPlayingUpdate = 0x5001,
  StopNowPlayingUpdates = 0x5002,
  SetNowPlayingInformation = 0x5003,

  StartCallStateUpdates = 0x4154,
  CallStateUpdate = 0x4155,
  StopCallStateUpdates = 0x4156,
  StartCommunicationsUpdates = 0x4157,
  CommunicationsUpdate = 0x4158,
  StopCommunicationsUpdates = 0x4159,
  InitiateCall = 0x415A,
  AcceptCall = 0x415B,
  EndCall = 0x415C,
  SwapCalls = 0x415D,
  MergeCalls = 0x415E,
  HoldStatusUpdate = 0x415F,
  MuteStatusUpdate = 0x4160,
  SendDTMF = 0x4161,

  StartListUpdates = 0x4170,
  ListUpdate = 0x4171,
  StopListUpdates = 0x4172,

  DeviceInformationUpdate = 0x4E09,
  DeviceLanguageUpdate = 0x4E0A,
  DeviceTimeUpdate = 0x4E0B,
  DeviceUUIDUpdate = 0x4E0C,
  WirelessCarPlayUpdate = 0x4E0D,
  DeviceTransportIdentifierNotification = 0x4E0E,

  StartExternalAccessoryProtocolSession = 0xEA00,
  StopExternalAccessoryProtocolSession = 0xEA01,
  StatusExternalAccessoryProtocolSession = 0xEA03,

  StartLocationInformation = 0xFFFA,
  GPRMCDataStatusValuesNotification = 0xFFF0,
  LocationInformation = 0xFFFB,
  StopLocationInformation = 0xFFFC,

  StartPowerUpdates = 0xAE00,
  PowerUpdate = 0xAE01,
  StopPowerUpdates = 0xAE02,
  PowerSourceUpdate = 0xAE03,

  RequestWiFiInformation = 0x5700,
  WiFiInformation = 0x5701,
  RequestAccessoryWiFiConfigurationInformation = 0x5702,
  AccessoryWiFiConfigurationInformation = 0x5703,
};

struct CsmDescriptor {
  CsmType type;
  std::string_view name;
  // Source filename without the .rs extension.
  std::string_view family;
};

// All identified CSM messages, grouped by source file in the same order as
// core/catplay_csm/src/msg/mod.rs declares them.
inline constexpr std::array csm_catalog{
    CsmDescriptor{CsmType::RequestAuthenticationCertificate, "RequestAuthenticationCertificate", "auth"},
    CsmDescriptor{CsmType::AuthenticationCertificate, "AuthenticationCertificate", "auth"},
    CsmDescriptor{CsmType::RequestAuthenticationChallengeResponse, "RequestAuthenticationChallengeResponse", "auth"},
    CsmDescriptor{CsmType::AuthenticationResponse, "AuthenticationResponse", "auth"},
    CsmDescriptor{CsmType::AuthenticationFailed, "AuthenticationFailed", "auth"},
    CsmDescriptor{CsmType::AuthenticationSucceeded, "AuthenticationSucceeded", "auth"},
    CsmDescriptor{CsmType::AccessoryAuthenticationSerialNumber, "AccessoryAuthenticationSerialNumber", "auth"},

    CsmDescriptor{CsmType::CarPlayAvailability, "CarPlayAvailability", "carplay_modern"},
    CsmDescriptor{CsmType::CarPlayStartSession, "CarPlayStartSession", "carplay_modern"},
    CsmDescriptor{CsmType::AvailableDigitalCarKeys, "AvailableDigitalCarKeys", "carplay_modern"},
    CsmDescriptor{CsmType::MatchedDigitalCarKeys, "MatchedDigitalCarKeys", "carplay_modern"},

    CsmDescriptor{CsmType::StartIdentification, "StartIdentification", "ident"},
    CsmDescriptor{CsmType::IdentificationInformation, "IdentificationInformation", "ident"},
    CsmDescriptor{CsmType::IdentificationAccepted, "IdentificationAccepted", "ident"},
    CsmDescriptor{CsmType::IdentificationRejected, "IdentificationRejected", "ident"},
    CsmDescriptor{CsmType::CancelIdentification, "CancelIdentification", "ident"},
    CsmDescriptor{CsmType::IdentificationInformationUpdate, "IdentificationInformationUpdate", "ident"},

    CsmDescriptor{CsmType::StartNowPlayingUpdates, "StartNowPlayingUpdates", "now_playing"},
    CsmDescriptor{CsmType::NowPlayingUpdate, "NowPlayingUpdate", "now_playing"},
    CsmDescriptor{CsmType::StopNowPlayingUpdates, "StopNowPlayingUpdates", "now_playing"},
    CsmDescriptor{CsmType::SetNowPlayingInformation, "SetNowPlayingInformation", "now_playing"},

    CsmDescriptor{CsmType::StartCallStateUpdates, "StartCallStateUpdates", "comms"},
    CsmDescriptor{CsmType::CallStateUpdate, "CallStateUpdate", "comms"},
    CsmDescriptor{CsmType::StopCallStateUpdates, "StopCallStateUpdates", "comms"},
    CsmDescriptor{CsmType::StartCommunicationsUpdates, "StartCommunicationsUpdates", "comms"},
    CsmDescriptor{CsmType::CommunicationsUpdate, "CommunicationsUpdate", "comms"},
    CsmDescriptor{CsmType::StopCommunicationsUpdates, "StopCommunicationsUpdates", "comms"},
    CsmDescriptor{CsmType::InitiateCall, "InitiateCall", "comms"},
    CsmDescriptor{CsmType::AcceptCall, "AcceptCall", "comms"},
    CsmDescriptor{CsmType::EndCall, "EndCall", "comms"},
    CsmDescriptor{CsmType::SwapCalls, "SwapCalls", "comms"},
    CsmDescriptor{CsmType::MergeCalls, "MergeCalls", "comms"},
    CsmDescriptor{CsmType::HoldStatusUpdate, "HoldStatusUpdate", "comms"},
    CsmDescriptor{CsmType::MuteStatusUpdate, "MuteStatusUpdate", "comms"},
    CsmDescriptor{CsmType::SendDTMF, "SendDTMF", "comms"},

    CsmDescriptor{CsmType::StartListUpdates, "StartListUpdates", "comms_lists"},
    CsmDescriptor{CsmType::ListUpdate, "ListUpdate", "comms_lists"},
    CsmDescriptor{CsmType::StopListUpdates, "StopListUpdates", "comms_lists"},

    CsmDescriptor{CsmType::DeviceInformationUpdate, "DeviceInformationUpdate", "device_notifications"},
    CsmDescriptor{CsmType::DeviceLanguageUpdate, "DeviceLanguageUpdate", "device_notifications"},
    CsmDescriptor{CsmType::DeviceTimeUpdate, "DeviceTimeUpdate", "device_notifications"},
    CsmDescriptor{CsmType::DeviceUUIDUpdate, "DeviceUUIDUpdate", "device_notifications"},
    CsmDescriptor{CsmType::WirelessCarPlayUpdate, "WirelessCarPlayUpdate", "device_notifications"},
    CsmDescriptor{CsmType::DeviceTransportIdentifierNotification, "DeviceTransportIdentifierNotification", "device_notifications"},

    CsmDescriptor{CsmType::StartExternalAccessoryProtocolSession, "StartExternalAccessoryProtocolSession", "eap"},
    CsmDescriptor{CsmType::StopExternalAccessoryProtocolSession, "StopExternalAccessoryProtocolSession", "eap"},
    CsmDescriptor{CsmType::StatusExternalAccessoryProtocolSession, "StatusExternalAccessoryProtocolSession", "eap"},

    CsmDescriptor{CsmType::StartLocationInformation, "StartLocationInformation", "gps"},
    CsmDescriptor{CsmType::GPRMCDataStatusValuesNotification, "GPRMCDataStatusValuesNotification", "gps"},
    CsmDescriptor{CsmType::LocationInformation, "LocationInformation", "gps"},
    CsmDescriptor{CsmType::StopLocationInformation, "StopLocationInformation", "gps"},

    CsmDescriptor{CsmType::StartPowerUpdates, "StartPowerUpdates", "power"},
    CsmDescriptor{CsmType::PowerUpdate, "PowerUpdate", "power"},
    CsmDescriptor{CsmType::StopPowerUpdates, "StopPowerUpdates", "power"},
    CsmDescriptor{CsmType::PowerSourceUpdate, "PowerSourceUpdate", "power"},

    CsmDescriptor{CsmType::RequestWiFiInformation, "RequestWiFiInformation", "wifi"},
    CsmDescriptor{CsmType::WiFiInformation, "WiFiInformation", "wifi"},
    CsmDescriptor{CsmType::RequestAccessoryWiFiConfigurationInformation, "RequestAccessoryWiFiConfigurationInformation", "wifi"},
    CsmDescriptor{CsmType::AccessoryWiFiConfigurationInformation, "AccessoryWiFiConfigurationInformation", "wifi"},
};

// Returns the descriptor for an identified CSM message, or nullptr when the
// value is not present in the catalog.
inline constexpr const CsmDescriptor* describe(CsmType type) {
  for (const CsmDescriptor& descriptor : csm_catalog) {
    if (descriptor.type == type) return &descriptor;
  }
  return nullptr;
}

// Wire command names that command_types.rs mentions only as trailing comments.
// They are declared intentions, not CommandType values and not usable wire
// implementations; they are recorded here so the gap stays visible.
inline constexpr std::array<std::string_view, 11> declared_command_extensions{
    "updateVehicleInformation",
    "flushAudio",
    "performHapticFeedback",
    "hidSetReport",
    "updateDisplayPanels",
    "updateVocoderInfo",
    "updateViewArea",
    "changeUIContext",
    "uiAppearanceUpdate",
    "mapAppearanceUpdate",
    "changeMapZoomLevel",
};

} // namespace zero2w::wired
