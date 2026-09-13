#pragma once
#include "core/session_core.hpp"
#include <atomic>
#include <string_view>
#include <thread>
namespace mvp {
// Replaceable seam only; no Apple protocol, MFi authentication, or iPhone connectivity is present.
class WirelessCarPlayAdapter:public InputAdapter{public:virtual bool running()const=0;};
class LocalDesktopBackend final:public InputAdapter{public:explicit LocalDesktopBackend(SessionCore&);~LocalDesktopBackend()override;std::string_view id()const override{return "local-desktop";}InputSourceKind kind()const override{return InputSourceKind::LocalDesktop;}void start()override;void stop()override;void on_control(const ControlEvent&)override;void refresh();uint64_t received_controls()const{return controls_;}private:SessionCore& core_;std::atomic<uint64_t> controls_{0};bool started_{};};
class SyntheticWirelessCarPlayBackend final:public WirelessCarPlayAdapter{public:explicit SyntheticWirelessCarPlayBackend(SessionCore&);~SyntheticWirelessCarPlayBackend()override;std::string_view id()const override{return "synthetic-wireless";}InputSourceKind kind()const override{return InputSourceKind::Synthetic;}void start()override;void stop()override;void on_control(const ControlEvent&)override;bool running()const override{return running_;}uint64_t received_controls()const{return controls_;}private:void run();SessionCore& core_;std::atomic<bool> running_{false};std::thread thread_;std::atomic<uint64_t> controls_{0};};
} // namespace mvp
