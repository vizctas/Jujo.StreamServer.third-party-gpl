/**
 * @file src/windows/win_display_device_general.cpp
 * @brief Definitions for the leftover (general) methods in WinDisplayDevice.
 */
// standard includes
#include <algorithm>
#include <set>
#include <utility>

// class header include
#include "display_device/windows/win_display_device.h"

// local includes
#include "display_device/logging.h"
#include "display_device/windows/win_api_utils.h"

namespace display_device {
  WinDisplayDevice::WinDisplayDevice(std::shared_ptr<WinApiLayerInterface> w_api):
      m_w_api {std::move(w_api)} {
    if (!m_w_api) {
      throw std::logic_error {"Nullptr provided for WinApiLayerInterface in WinDisplayDevice!"};
    }
  }

  bool WinDisplayDevice::isApiAccessAvailable() const {
    // Unless something is really broken on Windows, this call should never fail under normal circumstances - the configuration is 100% correct, since it was
    // provided by Windows.
    const UINT32 flags {SDC_VALIDATE | SDC_USE_DATABASE_CURRENT};
    const LONG result {m_w_api->setDisplayConfig({}, {}, flags)};

    DD_LOG(debug) << "WinDisplayDevice::isApiAccessAvailable result: " << m_w_api->getErrorString(result);
    return result == ERROR_SUCCESS;
  }

  EnumeratedDeviceList WinDisplayDevice::enumAvailableDevices(DeviceEnumerationDetail detail) const {
    const auto display_data {m_w_api->queryDisplayConfig(QueryType::All)};
    if (!display_data) {
      // Error already logged
      return {};
    }

    EnumeratedDeviceList available_devices;
    const auto source_data {win_utils::collectSourceDataForMatchingPaths(*m_w_api, display_data->m_paths)};
    if (source_data.empty()) {
      // Error already logged
      return {};
    }

    for (const auto &[device_id, data] : source_data) {
      // In case we have no active source, we will take the first available source id
      const auto source_id_index {data.m_active_source.value_or(data.m_source_id_to_path_index.begin()->first)};
      const auto &best_path {display_data->m_paths.at(data.m_source_id_to_path_index.at(source_id_index))};
      const auto monitor_device_path {m_w_api->getMonitorDevicePath(best_path)};
      const auto friendly_name {m_w_api->getFriendlyName(best_path)};
      const bool is_active {win_utils::isActive(best_path)};
      const auto source_mode {is_active ? win_utils::getSourceMode(win_utils::getSourceIndex(best_path, display_data->m_modes), display_data->m_modes) : nullptr};
      const auto display_name {is_active ? m_w_api->getDisplayName(best_path) : std::string {}};  // Inactive devices can have multiple display names, so it's just meaningless use any
      const auto edid {EdidData::parse(m_w_api->getEdid(best_path))};
      std::vector<Rational> supported_refresh_rates;
      if (detail == DeviceEnumerationDetail::Full) {
        std::set<std::pair<unsigned int, unsigned int>> seen_rates;

        for (const auto &[_, path_index] : data.m_source_id_to_path_index) {
          if (path_index >= display_data->m_paths.size()) {
            continue;
          }
          const auto &path_for_modes {display_data->m_paths.at(path_index)};
          if (!win_utils::isActive(path_for_modes) && !win_utils::isAvailable(path_for_modes)) {
            continue;
          }
          for (const auto &mode : m_w_api->getSupportedDisplayModes(path_for_modes)) {
            const auto &refresh {mode.m_refresh_rate};
            if (refresh.m_denominator == 0) {
              continue;
            }
            const auto inserted {seen_rates.emplace(refresh.m_numerator, refresh.m_denominator).second};
            if (inserted) {
              supported_refresh_rates.push_back(refresh);
            }
          }
        }

        std::sort(
          supported_refresh_rates.begin(),
          supported_refresh_rates.end(),
          [](const Rational &lhs, const Rational &rhs) {
            if (lhs.m_denominator == 0 || rhs.m_denominator == 0) {
              return lhs.m_numerator < rhs.m_numerator;
            }
            const long double lhs_value = static_cast<long double>(lhs.m_numerator) / static_cast<long double>(lhs.m_denominator);
            const long double rhs_value = static_cast<long double>(rhs.m_numerator) / static_cast<long double>(rhs.m_denominator);
            if (lhs_value == rhs_value) {
              if (lhs.m_numerator == rhs.m_numerator) {
                return lhs.m_denominator < rhs.m_denominator;
              }
              return lhs.m_numerator < rhs.m_numerator;
            }
            return lhs_value < rhs_value;
          }
        );
      }

      if (is_active && !source_mode) {
        DD_LOG(warning) << "Device " << device_id << " is missing source mode!";
      }

      if (source_mode) {
        const Rational refresh_rate {best_path.targetInfo.refreshRate.Denominator > 0 ? Rational {best_path.targetInfo.refreshRate.Numerator, best_path.targetInfo.refreshRate.Denominator} : Rational {0, 1}};
        FloatingPoint resolution_scale {Rational {1, 1}};
        if (detail == DeviceEnumerationDetail::Full && is_active) {
          resolution_scale = m_w_api->getDisplayScale(display_name, *source_mode).value_or(Rational {0, 1});
        }
        std::optional<HdrState> hdr_state;
        if (detail == DeviceEnumerationDetail::Full) {
          hdr_state = m_w_api->getHdrState(best_path);
        }
        const EnumeratedDevice::Info info {
          {source_mode->width, source_mode->height},
          resolution_scale,
          refresh_rate,
          win_utils::isPrimary(*source_mode),
          {static_cast<int>(source_mode->position.x), static_cast<int>(source_mode->position.y)},
          hdr_state
        };

        EnumeratedDevice device;
        device.m_device_id = device_id;
        device.m_monitor_device_path = monitor_device_path;
        device.m_display_name = display_name;
        device.m_friendly_name = friendly_name;
        device.m_edid = edid;
        device.m_info = info;
        device.m_supported_refresh_rates = std::move(supported_refresh_rates);
        available_devices.push_back(std::move(device));
      } else {
        EnumeratedDevice device;
        device.m_device_id = device_id;
        device.m_monitor_device_path = monitor_device_path;
        device.m_display_name = display_name;
        device.m_friendly_name = friendly_name;
        device.m_edid = edid;
        device.m_supported_refresh_rates = std::move(supported_refresh_rates);
        available_devices.push_back(std::move(device));
      }
    }

    return available_devices;
  }

  std::string WinDisplayDevice::getDisplayName(const std::string &device_id) const {
    if (device_id.empty()) {
      // Valid return, no error
      return {};
    }

    const auto display_data {m_w_api->queryDisplayConfig(QueryType::Active)};
    if (!display_data) {
      // Error already logged
      return {};
    }

    const auto path {win_utils::getActivePath(*m_w_api, device_id, display_data->m_paths)};
    if (!path) {
      // Debug level, because inactive device is valid case for this function
      DD_LOG(debug) << "Failed to find device for " << device_id << "!";
      return {};
    }

    const auto display_name {m_w_api->getDisplayName(*path)};
    if (display_name.empty()) {
      // Theoretically possible due to some race condition in the OS...
      DD_LOG(error) << "Device " << device_id << " has no display name assigned.";
    }

    return display_name;
  }

  bool WinDisplayDevice::restoreMonitorSettings() {
    // Apply the settings currently stored by Windows (registry/database).
    // Strictly use the current database without introducing virtual-mode semantics
    // and with empty path/mode arrays, matching:
    //   SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_USE_DATABASE_CURRENT)
    const UINT32 flags {SDC_APPLY | SDC_USE_DATABASE_CURRENT};
    const LONG result {m_w_api->setDisplayConfig({}, {}, flags)};
    if (result != ERROR_SUCCESS) {
      DD_LOG(error) << m_w_api->getErrorString(result) << " failed to restore monitor settings from database!";
      return false;
    }
    return true;
  }

  bool WinDisplayDevice::setDisplayOrigin(const std::string &device_id, const display_device::Point &origin) {
    if (device_id.empty()) {
      DD_LOG(error) << "Display id is empty; cannot move display.";
      return false;
    }

    auto display_data {m_w_api->queryDisplayConfig(QueryType::Active)};
    if (!display_data) {
      return false;
    }

    auto *path = win_utils::getActivePath(*m_w_api, device_id, display_data->m_paths);
    if (!path) {
      DD_LOG(error) << "Failed to find path for device " << device_id << "!";
      return false;
    }

    const auto source_index = win_utils::getSourceIndex(*path, display_data->m_modes);
    if (!source_index) {
      DD_LOG(error) << "Device " << device_id << " is missing a source mode!";
      return false;
    }

    auto *source_mode = win_utils::getSourceMode(source_index, display_data->m_modes);
    if (!source_mode) {
      DD_LOG(error) << "Source mode lookup failed for device " << device_id << "!";
      return false;
    }

    source_mode->position.x = static_cast<LONG>(origin.m_x);
    source_mode->position.y = static_cast<LONG>(origin.m_y);

    const UINT32 flags {SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE | SDC_VIRTUAL_MODE_AWARE};
    const LONG result {m_w_api->setDisplayConfig(display_data->m_paths, display_data->m_modes, flags)};
    if (result != ERROR_SUCCESS) {
      DD_LOG(error) << m_w_api->getErrorString(result) << " failed to move device " << device_id << " to new origin!";
      return false;
    }

    return true;
  }
}  // namespace display_device
