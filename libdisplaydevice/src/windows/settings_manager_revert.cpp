/**
 * @file src/windows/settings_manager_revert.cpp
 * @brief Definitions for the methods for reverting settings in SettingsManager.
 */
// class header include
#include "display_device/windows/settings_manager.h"

// system includes
#include <boost/scope/scope_exit.hpp>

// local includes
#include "display_device/logging.h"
#include "display_device/windows/json.h"
#include "display_device/windows/settings_utils.h"

namespace display_device {
  namespace {
    /**
     * @brief Function that does nothing.
     */
    void noopFn() {
    }
  }  // namespace

  SettingsManager::RevertResult SettingsManager::revertSettings() {
    const auto api_access {m_dd_api->isApiAccessAvailable()};
    DD_LOG(info) << "Trying to revert applied display device settings. API is available: " << toJson(api_access);
    if (!api_access) {
      return RevertResult::ApiTemporarilyUnavailable;
    }

    // Try to revert using cached modified state first for reliability.
    const auto &cached_state {m_persistence_state->getState()};
    const auto current_topology {m_dd_api->getCurrentTopology()};
    if (!m_dd_api->isTopologyValid(current_topology)) {
      DD_LOG(error) << "Topology prior to revert is invalid:\n"
                    << toJson(current_topology);
      return RevertResult::TopologyIsInvalid;
    }

    bool system_settings_touched {false};
    if (cached_state && cached_state->m_modified.hasModifications()) {
      if (const auto result = revertModifiedSettings(current_topology, system_settings_touched); result != RevertResult::Ok) {
        DD_LOG(warning) << "Failed to revert using cached modified state (code: " << static_cast<int>(result) << "). Falling back to Windows database restore.";
      } else {
        if (m_audio_context_api->isCaptured()) {
          m_audio_context_api->release();
        }
        return RevertResult::Ok;
      }
    }

    // Fallback: restore current per-user monitor configuration from Windows DB.
    if (!m_dd_api->restoreMonitorSettings()) {
      DD_LOG(error) << "Failed to restore monitor settings from Windows database!";
      return RevertResult::SwitchingTopologyFailed;
    }

    if (m_audio_context_api->isCaptured()) {
      m_audio_context_api->release();
    }

    // Clear cached state if present since the system is back to user's baseline.
    static_cast<void>(m_persistence_state->persistState(std::nullopt));
    return RevertResult::Ok;
  }

  SettingsManager::RevertResult SettingsManager::revertModifiedSettings(const ActiveTopology &current_topology, bool &system_settings_touched, bool *switched_topology) {
    const auto &cached_state {m_persistence_state->getState()};
    if (!cached_state) {
      return RevertResult::Ok;
    }

    auto state = *cached_state;

    // 1) Ensure we are back on the initial topology used as baseline.
    if (!m_dd_api->isTopologyTheSame(current_topology, state.m_initial.m_topology)) {
      system_settings_touched = true;
      DD_LOG(info) << "Reverting topology to initial:\n"
                   << toJson(state.m_initial.m_topology);
      if (!m_dd_api->setTopology(state.m_initial.m_topology)) {
        DD_LOG(error) << "Failed switching topology back to initial!";
        return RevertResult::SwitchingTopologyFailed;
      }
      if (switched_topology) {
        *switched_topology = true;
      }
    }

    // 2) Restore primary device if needed.
    if (!state.m_modified.m_original_primary_device.empty()) {
      DD_LOG(info) << "Restoring original primary device: " << toJson(state.m_modified.m_original_primary_device);
      system_settings_touched = true;
      if (!m_dd_api->setAsPrimary(state.m_modified.m_original_primary_device)) {
        DD_LOG(error) << "Failed to restore original primary device!";
        return RevertResult::RevertingPrimaryDeviceFailed;
      }
    }

    // 3) Restore display modes if needed.
    if (!state.m_modified.m_original_modes.empty()) {
      DD_LOG(info) << "Restoring original display modes.";
      if (!m_dd_api->setDisplayModes(state.m_modified.m_original_modes)) {
        DD_LOG(error) << "Failed to restore original display modes!";
        return RevertResult::RevertingDisplayModesFailed;
      }
      system_settings_touched = true;
    }

    // 4) Restore HDR states if needed.
    if (!state.m_modified.m_original_hdr_states.empty()) {
      DD_LOG(info) << "Restoring original HDR states.";
      if (!m_dd_api->setHdrStates(state.m_modified.m_original_hdr_states)) {
        DD_LOG(error) << "Failed to restore original HDR states!";
        return RevertResult::RevertingHdrStatesFailed;
      }
      system_settings_touched = true;
    }

    // Success: clear cached persistent state.
    if (!m_persistence_state->persistState(std::nullopt)) {
      DD_LOG(error) << "Failed to clear persistent state after revert!";
      return RevertResult::PersistenceSaveFailed;
    }

    return RevertResult::Ok;
  }
}  // namespace display_device
