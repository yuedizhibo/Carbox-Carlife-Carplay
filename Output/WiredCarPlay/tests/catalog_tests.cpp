// Deterministic catalog tests. Explicit checks only: no assert(), so every
// check stays enabled in Release builds.
#include "wired_carplay/catalog.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace zero2w::wired;

namespace {
int checks = 0;
std::vector<std::string> failures;

void check(bool condition, const char* expression, int line) {
  ++checks;
  if (!condition) {
    failures.push_back("line " + std::to_string(line) + ": " + expression);
  }
}

#define CHECK(x) check((x), #x, __LINE__)

std::uint16_t id_of(CsmType type) { return static_cast<std::uint16_t>(type); }
} // namespace

int main() {
  // --- command catalog -----------------------------------------------------
  CHECK(command_catalog.size() == 13u);

  int device_to_accessory = 0;
  int accessory_to_device = 0;
  int bidirectional = 0;
  int protocol_defined = 0;
  for (const CommandDescriptor& descriptor : command_catalog) {
    switch (descriptor.direction) {
      case Direction::DeviceToAccessory: ++device_to_accessory; break;
      case Direction::AccessoryToDevice: ++accessory_to_device; break;
      case Direction::Bidirectional: ++bidirectional; break;
      case Direction::ProtocolDefined: ++protocol_defined; break;
    }
    CHECK(!descriptor.wire_name.empty());
  }
  CHECK(device_to_accessory == 5);
  CHECK(accessory_to_device == 7);
  CHECK(bidirectional == 1);
  CHECK(protocol_defined == 0);

  // Roundtrip: every catalog entry is reachable by its own CommandType.
  for (const CommandDescriptor& descriptor : command_catalog) {
    const CommandDescriptor* found = describe(descriptor.type);
    CHECK(found != nullptr);
    if (found == nullptr) continue;
    CHECK(found == &descriptor);
    CHECK(found->type == descriptor.type);
    CHECK(found->wire_name == descriptor.wire_name);
    CHECK(found->direction == descriptor.direction);
  }

  // Every CommandType value is present exactly once.
  for (std::uint8_t raw = 0; raw < 13; ++raw) {
    const CommandType type = static_cast<CommandType>(raw);
    int occurrences = 0;
    for (const CommandDescriptor& descriptor : command_catalog) {
      if (descriptor.type == type) ++occurrences;
    }
    CHECK(occurrences == 1);
    CHECK(describe(type) != nullptr);
  }

  // Documented wire names and directions (case sensitive, iAPSendMessage is not
  // iApSendMessage).
  const CommandDescriptor* duck = describe(CommandType::DuckAudio);
  CHECK(duck != nullptr && duck->wire_name == "duckAudio");
  CHECK(duck != nullptr && duck->direction == Direction::DeviceToAccessory);
  const CommandDescriptor* modes_changed = describe(CommandType::ModesChanged);
  CHECK(modes_changed != nullptr && modes_changed->wire_name == "modesChanged");
  CHECK(modes_changed != nullptr && modes_changed->direction == Direction::DeviceToAccessory);
  const CommandDescriptor* set_limited_ui = describe(CommandType::SetLimitedUI);
  CHECK(set_limited_ui != nullptr && set_limited_ui->wire_name == "setLimitedUI");
  CHECK(set_limited_ui != nullptr && set_limited_ui->direction == Direction::AccessoryToDevice);
  const CommandDescriptor* iap = describe(CommandType::IApSendMessage);
  CHECK(iap != nullptr && iap->wire_name == "iAPSendMessage");
  CHECK(iap != nullptr && iap->direction == Direction::Bidirectional);

  // Invalid enum values resolve to nullptr.
  CHECK(describe(static_cast<CommandType>(13)) == nullptr);
  CHECK(describe(static_cast<CommandType>(200)) == nullptr);
  CHECK(describe(static_cast<CommandType>(-1)) == nullptr);
  CHECK(describe(CommandType::DuckAudio) != nullptr);

  // --- declared extensions -------------------------------------------------
  CHECK(declared_command_extensions.size() == 11u);
  CHECK(declared_command_extensions[0] == "updateVehicleInformation");
  CHECK(declared_command_extensions[1] == "flushAudio");
  CHECK(declared_command_extensions[2] == "performHapticFeedback");
  CHECK(declared_command_extensions[3] == "hidSetReport");
  CHECK(declared_command_extensions[4] == "updateDisplayPanels");
  CHECK(declared_command_extensions[5] == "updateVocoderInfo");
  CHECK(declared_command_extensions[6] == "updateViewArea");
  CHECK(declared_command_extensions[7] == "changeUIContext");
  CHECK(declared_command_extensions[8] == "uiAppearanceUpdate");
  CHECK(declared_command_extensions[9] == "mapAppearanceUpdate");
  CHECK(declared_command_extensions[10] == "changeMapZoomLevel");
  for (const std::string_view extension : declared_command_extensions) {
    CHECK(!extension.empty());
    for (const CommandDescriptor& descriptor : command_catalog) {
      CHECK(descriptor.wire_name != extension);
    }
  }

  // --- CSM catalog ---------------------------------------------------------
  CHECK(csm_catalog.size() == 59u);

  // Every entry is named and carries a family.
  for (const CsmDescriptor& descriptor : csm_catalog) {
    CHECK(!descriptor.name.empty());
    CHECK(!descriptor.family.empty());
  }

  // All packet IDs are unique.
  {
    std::vector<std::uint16_t> ids;
    ids.reserve(csm_catalog.size());
    for (const CsmDescriptor& descriptor : csm_catalog) {
      ids.push_back(id_of(descriptor.type));
    }
    bool unique = true;
    for (std::size_t i = 0; i < ids.size(); ++i) {
      for (std::size_t j = i + 1; j < ids.size(); ++j) {
        if (ids[i] == ids[j]) unique = false;
      }
    }
    CHECK(unique);
  }

  // Roundtrip: every entry is reachable by its own CsmType, and the descriptor
  // name matches the CsmType enumerator it was declared with.
  for (const CsmDescriptor& descriptor : csm_catalog) {
    const CsmDescriptor* found = describe(descriptor.type);
    CHECK(found != nullptr);
    if (found == nullptr) continue;
    CHECK(found == &descriptor);
    CHECK(found->name == descriptor.name);
    CHECK(found->family == descriptor.family);
  }

  CHECK(describe(static_cast<CsmType>(0x0001)) == nullptr);
  CHECK(describe(static_cast<CsmType>(0x4E0F)) == nullptr);
  CHECK(describe(static_cast<CsmType>(0xFFFF)) == nullptr);

  // Authentication family: 0xAA00..0xAA06.
  const CsmDescriptor* auth = describe(CsmType::RequestAuthenticationCertificate);
  CHECK(auth != nullptr && auth->name == "RequestAuthenticationCertificate");
  CHECK(auth != nullptr && auth->family == "auth" && id_of(auth->type) == 0xAA00);
  CHECK(id_of(CsmType::AuthenticationCertificate) == 0xAA01);
  CHECK(id_of(CsmType::RequestAuthenticationChallengeResponse) == 0xAA02);
  CHECK(id_of(CsmType::AuthenticationResponse) == 0xAA03);
  CHECK(id_of(CsmType::AuthenticationFailed) == 0xAA04);
  CHECK(id_of(CsmType::AuthenticationSucceeded) == 0xAA05);
  CHECK(id_of(CsmType::AccessoryAuthenticationSerialNumber) == 0xAA06);

  // Identification family: 0x1D00..0x1D06 with no 0x1D04 message.
  const CsmDescriptor* start_ident = describe(CsmType::StartIdentification);
  CHECK(start_ident != nullptr && start_ident->name == "StartIdentification");
  CHECK(start_ident != nullptr && start_ident->family == "ident" && id_of(start_ident->type) == 0x1D00);
  CHECK(id_of(CsmType::IdentificationInformation) == 0x1D01);
  CHECK(id_of(CsmType::IdentificationAccepted) == 0x1D02);
  CHECK(id_of(CsmType::IdentificationRejected) == 0x1D03);
  CHECK(id_of(CsmType::CancelIdentification) == 0x1D05);
  CHECK(id_of(CsmType::IdentificationInformationUpdate) == 0x1D06);
  CHECK(describe(static_cast<CsmType>(0x1D04)) == nullptr);

  // CarPlay family: 0x4300..0x4303.
  const CsmDescriptor* availability = describe(CsmType::CarPlayAvailability);
  CHECK(availability != nullptr && availability->name == "CarPlayAvailability");
  CHECK(availability != nullptr && availability->family == "carplay_modern");
  CHECK(id_of(CsmType::CarPlayAvailability) == 0x4300);
  CHECK(id_of(CsmType::CarPlayStartSession) == 0x4301);
  CHECK(id_of(CsmType::AvailableDigitalCarKeys) == 0x4302);
  CHECK(id_of(CsmType::MatchedDigitalCarKeys) == 0x4303);

  // Now playing family: 0x5000..0x5003.
  const CsmDescriptor* now_playing = describe(CsmType::StartNowPlayingUpdates);
  CHECK(now_playing != nullptr && now_playing->name == "StartNowPlayingUpdates");
  CHECK(now_playing != nullptr && now_playing->family == "now_playing");
  CHECK(id_of(CsmType::StartNowPlayingUpdates) == 0x5000);
  CHECK(id_of(CsmType::NowPlayingUpdate) == 0x5001);
  CHECK(id_of(CsmType::StopNowPlayingUpdates) == 0x5002);
  CHECK(id_of(CsmType::SetNowPlayingInformation) == 0x5003);

  // Comms and comms_lists families: 0x4154..0x4159, 0x415A..0x4161, 0x4170..0x4172.
  const CsmDescriptor* comms = describe(CsmType::StartCommunicationsUpdates);
  CHECK(comms != nullptr && comms->name == "StartCommunicationsUpdates");
  CHECK(comms != nullptr && comms->family == "comms");
  CHECK(id_of(CsmType::StartCallStateUpdates) == 0x4154);
  CHECK(id_of(CsmType::CallStateUpdate) == 0x4155);
  CHECK(id_of(CsmType::StopCallStateUpdates) == 0x4156);
  CHECK(id_of(CsmType::StartCommunicationsUpdates) == 0x4157);
  CHECK(id_of(CsmType::CommunicationsUpdate) == 0x4158);
  CHECK(id_of(CsmType::StopCommunicationsUpdates) == 0x4159);
  CHECK(id_of(CsmType::InitiateCall) == 0x415A);
  CHECK(id_of(CsmType::AcceptCall) == 0x415B);
  CHECK(id_of(CsmType::EndCall) == 0x415C);
  CHECK(id_of(CsmType::SwapCalls) == 0x415D);
  CHECK(id_of(CsmType::MergeCalls) == 0x415E);
  CHECK(id_of(CsmType::HoldStatusUpdate) == 0x415F);
  CHECK(id_of(CsmType::MuteStatusUpdate) == 0x4160);
  CHECK(id_of(CsmType::SendDTMF) == 0x4161);
  const CsmDescriptor* lists = describe(CsmType::StartListUpdates);
  CHECK(lists != nullptr && lists->family == "comms_lists");
  CHECK(id_of(CsmType::StartListUpdates) == 0x4170);
  CHECK(id_of(CsmType::ListUpdate) == 0x4171);
  CHECK(id_of(CsmType::StopListUpdates) == 0x4172);

  // Device notifications family: 0x4E09..0x4E0E.
  const CsmDescriptor* notifications = describe(CsmType::DeviceInformationUpdate);
  CHECK(notifications != nullptr && notifications->family == "device_notifications");
  CHECK(id_of(CsmType::DeviceInformationUpdate) == 0x4E09);
  CHECK(id_of(CsmType::DeviceLanguageUpdate) == 0x4E0A);
  CHECK(id_of(CsmType::DeviceTimeUpdate) == 0x4E0B);
  CHECK(id_of(CsmType::DeviceUUIDUpdate) == 0x4E0C);
  CHECK(id_of(CsmType::WirelessCarPlayUpdate) == 0x4E0D);
  CHECK(id_of(CsmType::DeviceTransportIdentifierNotification) == 0x4E0E);

  // External accessory protocol family: 0xEA00, 0xEA01, 0xEA03 (no 0xEA02).
  const CsmDescriptor* eap = describe(CsmType::StartExternalAccessoryProtocolSession);
  CHECK(eap != nullptr && eap->family == "eap");
  CHECK(id_of(CsmType::StartExternalAccessoryProtocolSession) == 0xEA00);
  CHECK(id_of(CsmType::StopExternalAccessoryProtocolSession) == 0xEA01);
  CHECK(id_of(CsmType::StatusExternalAccessoryProtocolSession) == 0xEA03);
  CHECK(describe(static_cast<CsmType>(0xEA02)) == nullptr);

  // GPS family: 0xFFFA, 0xFFF0, 0xFFFB, 0xFFFC.
  const CsmDescriptor* gps = describe(CsmType::StartLocationInformation);
  CHECK(gps != nullptr && gps->name == "StartLocationInformation");
  CHECK(gps != nullptr && gps->family == "gps");
  CHECK(id_of(CsmType::StartLocationInformation) == 0xFFFA);
  CHECK(id_of(CsmType::GPRMCDataStatusValuesNotification) == 0xFFF0);
  CHECK(id_of(CsmType::LocationInformation) == 0xFFFB);
  CHECK(id_of(CsmType::StopLocationInformation) == 0xFFFC);

  // Power family: 0xAE00..0xAE03.
  const CsmDescriptor* power = describe(CsmType::StartPowerUpdates);
  CHECK(power != nullptr && power->family == "power");
  CHECK(id_of(CsmType::StartPowerUpdates) == 0xAE00);
  CHECK(id_of(CsmType::PowerUpdate) == 0xAE01);
  CHECK(id_of(CsmType::StopPowerUpdates) == 0xAE02);
  CHECK(id_of(CsmType::PowerSourceUpdate) == 0xAE03);

  // Wi-Fi family: 0x5700..0x5703.
  const CsmDescriptor* wifi = describe(CsmType::RequestWiFiInformation);
  CHECK(wifi != nullptr && wifi->family == "wifi");
  CHECK(id_of(CsmType::RequestWiFiInformation) == 0x5700);
  CHECK(id_of(CsmType::WiFiInformation) == 0x5701);
  CHECK(id_of(CsmType::RequestAccessoryWiFiConfigurationInformation) == 0x5702);
  CHECK(id_of(CsmType::AccessoryWiFiConfigurationInformation) == 0x5703);

  // --- report --------------------------------------------------------------
  if (!failures.empty()) {
    std::cerr << "catalog checks failed: " << failures.size() << " of " << checks << '\n';
    for (const std::string& failure : failures) std::cerr << "  " << failure << '\n';
    std::cout << "catalog checks executed: " << checks << " failed: " << failures.size() << '\n';
    return 1;
  }
  std::cout << "catalog checks executed: " << checks << " failed: 0\n";
  return 0;
}
