/**
 * @file src/windows/win_api_layer.cpp
 * @brief Definitions for the WinApiLayer.
 */
// class header include
#include "display_device/windows/win_api_layer.h"
#include "display_device/windows/win_api_recovery.h"

// system includes
#include <boost/algorithm/string.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/uuid/name_generator_sha1.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <sstream>
#include <unordered_set>

// local includes
#include "display_device/logging.h"

// Windows includes after "windows.h"
#include <SetupApi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace {
  thread_local display_device::DisplayRecoveryBehavior g_display_recovery_behavior = display_device::DisplayRecoveryBehavior::Automatic;
}

namespace display_device {
  namespace detail {
    DisplayRecoveryBehavior current_display_recovery_behavior() {
      return g_display_recovery_behavior;
    }

    void set_display_recovery_behavior(DisplayRecoveryBehavior behavior) {
      g_display_recovery_behavior = behavior;
    }
  }  // namespace detail

  DisplayRecoveryBehaviorGuard::DisplayRecoveryBehaviorGuard(DisplayRecoveryBehavior behavior):
      m_previous(detail::current_display_recovery_behavior()) {
    detail::set_display_recovery_behavior(behavior);
  }

  DisplayRecoveryBehaviorGuard::~DisplayRecoveryBehaviorGuard() {
    detail::set_display_recovery_behavior(m_previous);
  }

  namespace {
    constexpr DWORD QUERY_ERROR_COOLDOWN_MS = 250;
    constexpr int MAX_TOPOLOGY_JOG_ATTEMPTS = 5;

    std::string queryTypeToString(QueryType type) {
      switch (type) {
        case QueryType::Active:
          return "Active";
        case QueryType::All:
          return "All";
      }

      return "Unknown";
    }

    std::string hdrStateToString(HdrState state) {
      switch (state) {
        case HdrState::Disabled:
          return "Disabled";
        case HdrState::Enabled:
          return "Enabled";
      }

      return "Unknown";
    }

    std::string pathIdentifier(const DISPLAYCONFIG_PATH_INFO &path) {
      std::ostringstream identifier;
      identifier << "adapter=[" << path.sourceInfo.adapterId.HighPart << ":" << path.sourceInfo.adapterId.LowPart << "]";
      identifier << " source=" << path.sourceInfo.id << " target=" << path.targetInfo.id;
      return identifier.str();
    }

    class ApiCallTimer {
    public:
      ApiCallTimer(const char *name, std::string detail = {})
          : m_name(name), m_detail(std::move(detail)), m_start(std::chrono::steady_clock::now()) {}

      ~ApiCallTimer() {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start);
        DD_LOG(debug) << m_name << (m_detail.empty() ? "" : " (" + m_detail + ")") << " completed in " << elapsed.count() << " ms";
      }

    private:
      const char *m_name;
      std::string m_detail;
      std::chrono::steady_clock::time_point m_start;
    };

    // Forward declaration to allow use before definition within this TU
    std::string toUtf8(const WinApiLayerInterface &w_api, const std::wstring &value);

    std::string normalizeDisplayName(std::string name) {
      boost::algorithm::to_lower(name);
      constexpr std::string_view prefix = R"(\\.\)";
      if (name.rfind(prefix.data(), 0) == 0) {
        name.erase(0, prefix.size());
      }
      const auto suffix_pos = name.find('\\');
      if (suffix_pos != std::string::npos) {
        name.erase(suffix_pos);
      }
      return name;
    }

    std::optional<HMONITOR> monitorFromDisplayName(const std::string &display_name) {
      if (display_name.empty()) {
        return std::nullopt;
      }

      const auto normalized_target = normalizeDisplayName(display_name);
      struct EnumData {
        std::string normalized_target;
        HMONITOR monitor = nullptr;
      };

      EnumData data {normalized_target, nullptr};
      EnumDisplayMonitors(
        nullptr,
        nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
          auto *user_data = reinterpret_cast<EnumData *>(param);
          MONITORINFOEXA info {sizeof(MONITORINFOEXA)};
          if (!GetMonitorInfoA(monitor, &info)) {
            return TRUE;
          }

          std::string monitor_name(info.szDevice);
          if (normalizeDisplayName(std::move(monitor_name)) == user_data->normalized_target) {
            user_data->monitor = monitor;
            return FALSE;
          }

          return TRUE;
        },
        reinterpret_cast<LPARAM>(&data)
      );

      if (data.monitor != nullptr) {
        return data.monitor;
      }

      return std::nullopt;
    }

    /** @brief Dumps the result of @see queryDisplayConfig into a string */
    std::string dumpPath(const DISPLAYCONFIG_PATH_INFO &info) {
      std::ostringstream output;
      std::ios state(nullptr);
      state.copyfmt(output);

      // clang-format off
      output << "sourceInfo:" << std::endl;
      output << "    adapterId: [" << info.sourceInfo.adapterId.HighPart << ", " << info.sourceInfo.adapterId.LowPart << "]" << std::endl;
      output << "    id: " << info.sourceInfo.id << std::endl;
      output << "        cloneGroupId: " << info.sourceInfo.cloneGroupId << std::endl;
      output << "        sourceModeInfoIdx: " << info.sourceInfo.sourceModeInfoIdx << std::endl;
      output << "        modeInfoIdx: " << info.sourceInfo.modeInfoIdx << std::endl;
      output << "    statusFlags: 0x" << std::uppercase << std::setfill('0') << std::setw(8) << std::hex << info.sourceInfo.statusFlags << std::endl;
      output.copyfmt(state);
      output << "targetInfo:" << std::endl;
      output << "    adapterId: [" << info.targetInfo.adapterId.HighPart << ", " << info.targetInfo.adapterId.LowPart << "]" << std::endl;
      output << "    id: " << info.targetInfo.id << std::endl;
      output << "        desktopModeInfoIdx: " << info.targetInfo.desktopModeInfoIdx << std::endl;
      output << "        targetModeInfoIdx: " << info.targetInfo.targetModeInfoIdx << std::endl;
      output << "        modeInfoIdx: " << info.targetInfo.modeInfoIdx << std::endl;
      output << "    outputTechnology:  0x" << std::uppercase << std::setfill('0') << std::setw(8) << std::hex << info.targetInfo.outputTechnology << std::endl;
      output << "    rotation: 0x" << std::uppercase << std::setfill('0') << std::setw(8) << std::hex << info.targetInfo.rotation << std::endl;
      output << "    scaling: 0x" << std::uppercase << std::setfill('0') << std::setw(8) << std::hex << info.targetInfo.scaling << std::endl;
      output.copyfmt(state);
      output << "    refreshRate: " << info.targetInfo.refreshRate.Numerator << "/" << info.targetInfo.refreshRate.Denominator << std::endl;
      output << "    scanLineOrdering: 0x" << std::uppercase << std::setfill('0') << std::setw(8) << std::hex << info.targetInfo.scanLineOrdering << std::endl;
      output << "    targetAvailable: 0x" << std::uppercase << std::setfill('0') << std::setw(8) << std::hex << info.targetInfo.targetAvailable << std::endl;
      output << "    statusFlags: 0x" << std::uppercase << std::setfill('0') << std::setw(8) << std::hex << info.targetInfo.statusFlags << std::endl;
      output << "flags: 0x" << std::uppercase << std::setfill('0') << std::setw(8) << std::hex << info.flags;
      // clang-format on

      return output.str();
    }

    /** @brief Dumps the result of @see queryDisplayConfig into a string */
    std::string dumpMode(const DISPLAYCONFIG_MODE_INFO &info) {
      std::stringstream output;
      std::ios state(nullptr);
      state.copyfmt(output);

      if (info.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        // clang-format off
        output << "width: " << info.sourceMode.width << std::endl;
        output << "height: " << info.sourceMode.height << std::endl;
        output << "pixelFormat: " << info.sourceMode.pixelFormat << std::endl;
        output << "position: [" << info.sourceMode.position.x << ", " << info.sourceMode.position.y << "]";
        // clang-format on
      } else if (info.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
        // clang-format off
        output << "pixelRate: " << info.targetMode.targetVideoSignalInfo.pixelRate << std::endl;
        output << "hSyncFreq: " << info.targetMode.targetVideoSignalInfo.hSyncFreq.Numerator << "/" << info.targetMode.targetVideoSignalInfo.hSyncFreq.Denominator << std::endl;
        output << "vSyncFreq: " << info.targetMode.targetVideoSignalInfo.vSyncFreq.Numerator << "/" << info.targetMode.targetVideoSignalInfo.vSyncFreq.Denominator << std::endl;
        output << "activeSize: [" << info.targetMode.targetVideoSignalInfo.activeSize.cx << ", " << info.targetMode.targetVideoSignalInfo.activeSize.cy << "]" << std::endl;
        output << "totalSize: [" << info.targetMode.targetVideoSignalInfo.totalSize.cx << ", " << info.targetMode.targetVideoSignalInfo.totalSize.cy << "]" << std::endl;
        output << "videoStandard: " << info.targetMode.targetVideoSignalInfo.videoStandard << std::endl;
        output << "scanLineOrdering: " << info.targetMode.targetVideoSignalInfo.scanLineOrdering;
        // clang-format on
      } else if (info.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE) {
        // TODO: One day MinGW will add updated struct definition and the following code can be enabled
        // clang-format off
        // output << "PathSourceSize: [" << info.desktopImageInfo.PathSourceSize.x << ", " << info.desktopImageInfo.PathSourceSize.y << "]" << std::endl;
        // output << "DesktopImageRegion: [" << info.desktopImageInfo.DesktopImageRegion.bottom << ", " << info.desktopImageInfo.DesktopImageRegion.left << ", " << info.desktopImageInfo.DesktopImageRegion.right << ", " << info.desktopImageInfo.DesktopImageRegion.top << "]" << std::endl;
        // output << "DesktopImageClip: [" << info.desktopImageInfo.DesktopImageClip.bottom << ", " << info.desktopImageInfo.DesktopImageClip.left << ", " << info.desktopImageInfo.DesktopImageClip.right << ", " << info.desktopImageInfo.DesktopImageClip.top << "]";
        // clang-format on
        output << "NOT SUPPORTED BY COMPILER YET...";
      } else {
        output << "NOT IMPLEMENTED YET...";
      }

      return output.str();
    }

    /** @brief Dumps the result of @see queryDisplayConfig into a string */
    std::string dumpPathsAndModes(const std::vector<DISPLAYCONFIG_PATH_INFO> &paths, const std::vector<DISPLAYCONFIG_MODE_INFO> &modes) {
      std::ostringstream output;

      output << std::endl
             << "Got " << paths.size() << " path(s):";
      bool path_dumped {false};
      for (auto i {0u}; i < paths.size(); ++i) {
        output << std::endl
               << "----------------------------------------[index: " << i << "]" << std::endl;

        output << dumpPath(paths[i]);
        path_dumped = true;
      }

      if (path_dumped) {
        output << std::endl
               << std::endl;
      }

      output << "Got " << modes.size() << " mode(s):";
      for (auto i {0u}; i < modes.size(); ++i) {
        output << std::endl
               << "----------------------------------------[index: " << i << "]" << std::endl;

        output << dumpMode(modes[i]);
      }

      return output.str();
    }

    /**
     * @see getMonitorDevicePath description for more information as this
     *      function is identical except that it returns wide-string instead
     *      of a normal one.
     */
    std::wstring getMonitorDevicePathWstr(const WinApiLayerInterface &w_api, const DISPLAYCONFIG_PATH_INFO &path) {
      DISPLAYCONFIG_TARGET_DEVICE_NAME target_name = {};
      target_name.header.adapterId = path.targetInfo.adapterId;
      target_name.header.id = path.targetInfo.id;
      target_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
      target_name.header.size = sizeof(target_name);

      LONG result {DisplayConfigGetDeviceInfo(&target_name.header)};
      if (result != ERROR_SUCCESS) {
        DD_LOG(error) << w_api.getErrorString(result) << " failed to get target device name!";
        return {};
      }

      return std::wstring {target_name.monitorDevicePath};
    }

    /**
     * @brief Helper method for dealing with SetupAPI.
     * @returns True if device interface path was retrieved and is non-empty, false otherwise.
     * @see getDeviceId implementation for more context regarding this madness.
     */
    bool getDeviceInterfaceDetail(const WinApiLayerInterface &w_api, HDEVINFO dev_info_handle, SP_DEVICE_INTERFACE_DATA &dev_interface_data, std::wstring &dev_interface_path, SP_DEVINFO_DATA &dev_info_data) {
      DWORD required_size_in_bytes {0};
      if (SetupDiGetDeviceInterfaceDetailW(dev_info_handle, &dev_interface_data, nullptr, 0, &required_size_in_bytes, nullptr)) {
        DD_LOG(error) << "\"SetupDiGetDeviceInterfaceDetailW\" did not fail, what?!";
        return false;
      } else if (required_size_in_bytes <= 0) {
        DD_LOG(warning) << w_api.getErrorString(static_cast<LONG>(GetLastError())) << " \"SetupDiGetDeviceInterfaceDetailW\" failed while getting size.";
        return false;
      }

      std::vector<std::uint8_t> buffer;
      buffer.resize(required_size_in_bytes);

      // This part is just EVIL!
      auto detail_data {reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data())};
      detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

      if (!SetupDiGetDeviceInterfaceDetailW(dev_info_handle, &dev_interface_data, detail_data, required_size_in_bytes, nullptr, &dev_info_data)) {
        DD_LOG(warning) << w_api.getErrorString(static_cast<LONG>(GetLastError())) << " \"SetupDiGetDeviceInterfaceDetailW\" failed.";
        return false;
      }

      dev_interface_path = std::wstring {detail_data->DevicePath};
      return !dev_interface_path.empty();
    }

    /**
     * @brief Helper method for dealing with SetupAPI.
     * @returns True if instance id was retrieved and is non-empty, false otherwise.
     * @see getDeviceId implementation for more context regarding this madness.
     */
    bool getDeviceInstanceId(const WinApiLayerInterface &w_api, HDEVINFO dev_info_handle, SP_DEVINFO_DATA &dev_info_data, std::wstring &instance_id) {
      DWORD required_size_in_characters {0};
      if (SetupDiGetDeviceInstanceIdW(dev_info_handle, &dev_info_data, nullptr, 0, &required_size_in_characters)) {
        DD_LOG(error) << "\"SetupDiGetDeviceInstanceIdW\" did not fail, what?!";
        return false;
      } else if (required_size_in_characters <= 0) {
        DD_LOG(warning) << w_api.getErrorString(static_cast<LONG>(GetLastError())) << " \"SetupDiGetDeviceInstanceIdW\" failed while getting size.";
        return false;
      }

      instance_id.resize(required_size_in_characters);
      if (!SetupDiGetDeviceInstanceIdW(dev_info_handle, &dev_info_data, instance_id.data(), instance_id.size(), nullptr)) {
        DD_LOG(warning) << w_api.getErrorString(static_cast<LONG>(GetLastError())) << " \"SetupDiGetDeviceInstanceIdW\" failed.";
        return false;
      }

      return !instance_id.empty();
    }

    /**
     * @brief Helper method for dealing with SetupAPI.
     * @returns True if EDID was retrieved and is non-empty, false otherwise.
     * @see getDeviceId implementation for more context regarding this madness.
     */
    bool getDeviceEdid(const WinApiLayerInterface &w_api, HDEVINFO dev_info_handle, SP_DEVINFO_DATA &dev_info_data, std::vector<std::byte> &edid) {
      // We could just directly open the registry key as the path is known, but we can also use the this
      HKEY reg_key {SetupDiOpenDevRegKey(dev_info_handle, &dev_info_data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ)};
      if (reg_key == INVALID_HANDLE_VALUE) {
        DD_LOG(warning) << w_api.getErrorString(static_cast<LONG>(GetLastError())) << " \"SetupDiOpenDevRegKey\" failed; proceeding without EDID.";
        edid.clear();
        return true;
      }

      const auto reg_key_cleanup {
        boost::scope::scope_exit([&w_api, &reg_key]() {
          const auto status {RegCloseKey(reg_key)};
          if (status != ERROR_SUCCESS) {
            DD_LOG(error) << w_api.getErrorString(status) << " \"RegCloseKey\" failed.";
          }
        })
      };

      DWORD required_size_in_bytes {0};
      auto status {RegQueryValueExW(reg_key, L"EDID", nullptr, nullptr, nullptr, &required_size_in_bytes)};
      if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
        // Many virtual/temporary devices do not expose EDID. This is expected.
        DD_LOG(verbose) << "EDID registry value not found for this device; proceeding without EDID.";
        edid.clear();
        return true;  // not fatal; allow caller to use instance ID only
      }
      if (status != ERROR_SUCCESS) {
        // Degrade severity and allow fallback to instance ID/path-based identifier.
        DD_LOG(warning) << w_api.getErrorString(status) << " \"RegQueryValueExW\" failed when getting size; proceeding without EDID.";
        edid.clear();
        return true;
      }

      if (required_size_in_bytes == 0) {
        DD_LOG(verbose) << "EDID registry value has zero size; proceeding without EDID.";
        edid.clear();
        return true;
      }

      edid.resize(required_size_in_bytes);

      status = RegQueryValueExW(reg_key, L"EDID", nullptr, nullptr, reinterpret_cast<LPBYTE>(edid.data()), &required_size_in_bytes);
      if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
        DD_LOG(verbose) << "EDID registry value disappeared during read; proceeding without EDID.";
        edid.clear();
        return true;
      }
      if (status != ERROR_SUCCESS) {
        DD_LOG(warning) << w_api.getErrorString(status) << " \"RegQueryValueExW\" failed when getting data; proceeding without EDID.";
        edid.clear();
        return true;
      }

      return !edid.empty();
    }

    /**
     * @brief Get instance ID and EDID via SetupAPI.
     * @param w_api Reference to the WinApiLayer.
     * @param device_path Device path to find device for.
     * @return A tuple of instance ID and EDID, or empty optional if not device was found or error has occurred.
     */
    std::optional<std::tuple<std::wstring, std::vector<std::byte>>> getInstanceIdAndEdid(const WinApiLayerInterface &w_api, const std::wstring &device_path) {
      static const GUID monitor_guid {0xe6f07b5f, 0xee97, 0x4a90, {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7}};

      HDEVINFO dev_info_handle {SetupDiGetClassDevsW(&monitor_guid, nullptr, nullptr, DIGCF_DEVICEINTERFACE)};
      if (dev_info_handle == INVALID_HANDLE_VALUE) {
        DD_LOG(warning) << w_api.getErrorString(static_cast<LONG>(GetLastError()))
                        << " \"SetupDiGetClassDevsW\" failed.";
        return std::nullopt;
      }
      if (dev_info_handle) {
        const auto dev_info_handle_cleanup {
          boost::scope::scope_exit([&dev_info_handle, &w_api]() {
            if (!SetupDiDestroyDeviceInfoList(dev_info_handle)) {
              DD_LOG(error) << w_api.getErrorString(static_cast<LONG>(GetLastError())) << " \"SetupDiDestroyDeviceInfoList\" failed.";
            }
          })
        };

        SP_DEVICE_INTERFACE_DATA dev_interface_data {};
        dev_interface_data.cbSize = sizeof(dev_interface_data);
        for (DWORD monitor_index = 0;; ++monitor_index) {
          if (!SetupDiEnumDeviceInterfaces(dev_info_handle, nullptr, &monitor_guid, monitor_index, &dev_interface_data)) {
            const DWORD error_code {GetLastError()};
            if (error_code == ERROR_NO_MORE_ITEMS) {
              break;
            }

            DD_LOG(warning) << w_api.getErrorString(static_cast<LONG>(error_code)) << " \"SetupDiEnumDeviceInterfaces\" failed.";
            continue;
          }

          std::wstring dev_interface_path;
          SP_DEVINFO_DATA dev_info_data {};
          dev_info_data.cbSize = sizeof(dev_info_data);
          if (!getDeviceInterfaceDetail(w_api, dev_info_handle, dev_interface_data, dev_interface_path, dev_info_data)) {
            // Error already logged
            continue;
          }

          if (!boost::iequals(dev_interface_path, device_path)) {
            continue;
          }

          std::wstring instance_id;
          if (!getDeviceInstanceId(w_api, dev_info_handle, dev_info_data, instance_id)) {
            // Error already logged
            break;
          }

          std::vector<std::byte> edid;
          if (!getDeviceEdid(w_api, dev_info_handle, dev_info_data, edid)) {
            // EDID is optional for our purposes; continue with instance ID only.
            DD_LOG(verbose) << "EDID not available for device path: " << toUtf8(w_api, dev_interface_path) << "; using instance ID only.";
          }

          return std::make_tuple(std::move(instance_id), std::move(edid));
        }
      }

      return std::nullopt;
    }

    /**
     * @brief Converts a UTF-16 wide string into a UTF-8 string.
     * @param w_api Reference to the WinApiLayer.
     * @param value The UTF-16 wide string.
     * @return The converted UTF-8 string.
     */
    std::string toUtf8(const WinApiLayerInterface &w_api, const std::wstring &value) {
      // No conversion needed if the string is empty
      if (value.empty()) {
        return {};
      }

      // Get the output size required to store the string
      auto output_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
      if (output_size == 0) {
        DD_LOG(error) << w_api.getErrorString(static_cast<LONG>(GetLastError())) << " failed to get UTF-8 buffer size.";
        return {};
      }

      // Perform the conversion
      std::string output(output_size, '\0');
      output_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), static_cast<int>(output.size()), nullptr, nullptr);
      if (output_size == 0) {
        DD_LOG(error) << w_api.getErrorString(static_cast<LONG>(GetLastError())) << " failed to convert string to UTF-8.";
        return {};
      }

      return output;
    }

    /**
     * @brief Check if the Windows 11 version is equal to 24H2 update or later.
     * @param w_api Reference to the WinApiLayer.
     * @return True if version >= W11 24H2, false otherwise.
     */
    bool is_W11_24H2_OrAbove(const WinApiLayerInterface &w_api) {
      OSVERSIONINFOEXA os_version_info;
      os_version_info.dwOSVersionInfoSize = sizeof(os_version_info);
      os_version_info.dwMajorVersion = HIBYTE(_WIN32_WINNT_WIN10);
      os_version_info.dwMinorVersion = LOBYTE(_WIN32_WINNT_WIN10);
      os_version_info.dwBuildNumber = 26100;  // The earliest pre-release version is 25947, whereas the stable is 26100

      ULONGLONG condition_mask {0};
      condition_mask = VerSetConditionMask(condition_mask, VER_MAJORVERSION, VER_GREATER_EQUAL);  // Major version condition
      condition_mask = VerSetConditionMask(condition_mask, VER_MINORVERSION, VER_GREATER_EQUAL);  // Minor version condition
      condition_mask = VerSetConditionMask(condition_mask, VER_BUILDNUMBER, VER_GREATER_EQUAL);  // Build number condition

      BOOL result {VerifyVersionInfoA(&os_version_info, VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER, condition_mask)};
      if (result == FALSE) {
        DD_LOG(debug) << "\"is_W11_24H2_OrAbove\" returned false.";
        return false;
      }

      DD_LOG(debug) << "\"is_W11_24H2_OrAbove\" returned true.";
      return true;
    }
  }  // namespace

  std::string WinApiLayer::getErrorString(LONG error_code) const {
    std::ostringstream detail;
    detail << "error=" << error_code;
    ApiCallTimer timer("WinApiLayer::getErrorString", detail.str());
    std::ostringstream error;
    error << "[code: ";
    switch (error_code) {
      case ERROR_INVALID_PARAMETER:
        error << "ERROR_INVALID_PARAMETER";
        break;
      case ERROR_NOT_SUPPORTED:
        error << "ERROR_NOT_SUPPORTED";
        break;
      case ERROR_ACCESS_DENIED:
        error << "ERROR_ACCESS_DENIED";
        break;
      case ERROR_INSUFFICIENT_BUFFER:
        error << "ERROR_INSUFFICIENT_BUFFER";
        break;
      case ERROR_GEN_FAILURE:
        error << "ERROR_GEN_FAILURE";
        break;
      case ERROR_SUCCESS:
        error << "ERROR_SUCCESS";
        break;
      default:
        error << error_code;
        break;
    }
    error << ", message: " << std::system_category().message(static_cast<int>(error_code)) << "]";
    return error.str();
  }

  std::optional<PathAndModeData> WinApiLayer::queryDisplayConfig(QueryType type) const {
    auto make_flags = [&](bool virtual_mode_aware) -> UINT32 {
      UINT32 f = (type == QueryType::Active) ? QDC_ONLY_ACTIVE_PATHS : QDC_ALL_PATHS;
      if (virtual_mode_aware) {
        f |= QDC_VIRTUAL_MODE_AWARE;
      }
      return f;
    };

    // Simple signature helper to detect topology changes and gate verbose dumps
    auto make_signature = [](const std::vector<DISPLAYCONFIG_PATH_INFO> &paths, const std::vector<DISPLAYCONFIG_MODE_INFO> &modes) {
      std::size_t h = paths.size() * 1315423911u ^ modes.size();
      auto mix = [&](std::size_t v) {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      };
      for (const auto &p : paths) {
        mix(static_cast<std::size_t>(p.sourceInfo.adapterId.HighPart));
        mix(static_cast<std::size_t>(p.sourceInfo.adapterId.LowPart));
        mix(static_cast<std::size_t>(p.sourceInfo.id));
        mix(static_cast<std::size_t>(p.targetInfo.id));
        mix(static_cast<std::size_t>(p.flags));
      }
      for (const auto &m : modes) {
        mix(static_cast<std::size_t>(m.id));
        mix(static_cast<std::size_t>(m.infoType));
      }
      return h;
    };

    static std::size_t last_sig = 0;

    for (int attempt = 0; attempt < 2; ++attempt) {
      const bool use_virtual = (attempt == 0);
      const UINT32 flags = make_flags(use_virtual);

      // First get a baseline size
      UINT32 path_count = 0, mode_count = 0;
      LONG result = GetDisplayConfigBufferSizes(flags, &path_count, &mode_count);
      if (result != ERROR_SUCCESS) {
        if (use_virtual && (result == ERROR_NOT_SUPPORTED || result == ERROR_INVALID_PARAMETER)) {
          DD_LOG(debug) << getErrorString(result)
                        << " getting buffer sizes with QDC_VIRTUAL_MODE_AWARE; retrying without it.";
          continue;  // try compat
        }
        DD_LOG(error) << getErrorString(result) << " failed 'to get display buffer size's!";
        ::Sleep(QUERY_ERROR_COOLDOWN_MS);
        continue;
      }

      std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
      std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);

      // Bounded retry with backoff if topology is changing.
      constexpr int kMaxTries = 9;
      int aggregated_failures = 0;
      LONG last_error = ERROR_SUCCESS;
      auto flush_failures = [&]() {
        if (aggregated_failures > 0) {
          DD_LOG(error) << getErrorString(last_error)
                        << " failed to query display paths and modes (" << aggregated_failures << " attempt"
                        << (aggregated_failures == 1 ? "" : "s") << ").";
          aggregated_failures = 0;
        }
      };
      auto record_failure = [&](LONG error) {
        last_error = error;
        ++aggregated_failures;
        ::Sleep(QUERY_ERROR_COOLDOWN_MS);
      };

      for (int tries = 0; tries < kMaxTries; ++tries) {
        UINT32 pc = static_cast<UINT32>(paths.size());
        UINT32 mc = static_cast<UINT32>(modes.size());
        result = QueryDisplayConfig(flags, &pc, pc ? paths.data() : nullptr, &mc, mc ? modes.data() : nullptr, nullptr);

        if (result == ERROR_SUCCESS) {
          flush_failures();
          paths.resize(pc);
          modes.resize(mc);
          const auto sig_now = make_signature(paths, modes);
          if (sig_now != last_sig) {
            DD_LOG(verbose) << "Result of " << (type == QueryType::Active ? "ACTIVE" : "ALL")
                            << " display config (" << (use_virtual ? "virtual-aware" : "compat")
                            << "):\n"
                            << dumpPathsAndModes(paths, modes) << "\n";
            last_sig = sig_now;
          }
          return PathAndModeData {paths, modes};
        }

        if (result == ERROR_INSUFFICIENT_BUFFER) {
          // pc/mc now contain the required sizes. Grow (only grow) and retry, no new sizes call.
          bool grew = false;
          if (pc > paths.size()) {
            paths.resize(pc);
            grew = true;
          }
          if (mc > modes.size()) {
            modes.resize(mc);
            grew = true;
          }

          // If we didn't grow, topology might be flapping—recompute sizes once.
          if (!grew) {
            result = GetDisplayConfigBufferSizes(flags, &path_count, &mode_count);
            if (result != ERROR_SUCCESS) {
              break;
            }
            if (path_count > paths.size()) {
              paths.resize(path_count);
            }
            if (mode_count > modes.size()) {
              modes.resize(mode_count);
            }
          }

          // Backoff (exponential, clamped)
          const DWORD delay = std::min<DWORD>(500, 25u * (1u << tries));
          ::Sleep(delay);
          continue;
        }

        if (use_virtual && (result == ERROR_NOT_SUPPORTED || result == ERROR_INVALID_PARAMETER)) {
          DD_LOG(debug) << getErrorString(result)
                        << " querying with QDC_VIRTUAL_MODE_AWARE; retrying without it.";
          record_failure(result);
          break;  // break inner; outer loop will try compat
        }

        record_failure(result);
        // Other errors: log and bail
        continue;
      }

      // If we exit the retry loop without success, try compat once (if we were virtual)
      if (use_virtual) {
        flush_failures();
        continue;
      }
      flush_failures();
      DD_LOG(error) << "Giving up after retries while querying display config.";
      return std::nullopt;
    }

    return std::nullopt;
  }

  std::string WinApiLayer::getDeviceId(const DISPLAYCONFIG_PATH_INFO &path) const {
    const auto device_path {getMonitorDevicePathWstr(*this, path)};
    if (device_path.empty()) {
      // Error already logged
      return {};
    }

    std::vector<std::byte> device_id_data;
    auto instance_id_and_edid {getInstanceIdAndEdid(*this, device_path)};
    if (instance_id_and_edid) {
      // Instance ID is unique in the system and persists restarts, but not driver re-installs.
      // It looks like this:
      //     DISPLAY\ACI27EC\5&4FD2DE4&5&UID4352 (also used in the device path it seems)
      //                a    b    c    d    e
      //
      //  a) Hardware ID - stable
      //  b) Either a bus number or has something to do with device capabilities - stable
      //  c) Another ID, somehow tied to adapter (not an adapter ID from path object) - stable
      //  d) Some sort of rotating counter thing, changes after driver reinstall - unstable
      //  e) Seems to be the same as a target ID from path, it changes based on GPU port - semi-stable
      //
      // The instance ID also seems to be a part of the registry key (in case some other info is needed in the future):
      //     HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Enum\DISPLAY\ACI27EC\5&4fd2de4&5&UID4352
      [this, &device_id_data, &instance_id_and_edid]() {
        auto [instance_id, edid] = *instance_id_and_edid;

        // We are going to discard the unstable parts of the instance ID and merge the stable parts with the edid buffer (if available)
        auto unstable_part_index = instance_id.find_first_of(L'&', 0);
        if (unstable_part_index != std::wstring::npos) {
          unstable_part_index = instance_id.find_first_of(L'&', unstable_part_index + 1);
        }

        if (unstable_part_index == std::wstring::npos) {
          DD_LOG(error) << "Failed to split off the stable part from instance id string " << toUtf8(*this, instance_id);
          return;
        }

        auto semi_stable_part_index = instance_id.find_first_of(L'&', unstable_part_index + 1);
        if (semi_stable_part_index == std::wstring::npos) {
          DD_LOG(error) << "Failed to split off the semi-stable part from instance id string " << toUtf8(*this, instance_id);
          return;
        }

        device_id_data.swap(edid);
        device_id_data.insert(std::end(device_id_data), reinterpret_cast<const std::byte *>(instance_id.data()), reinterpret_cast<const std::byte *>(instance_id.data() + unstable_part_index));
        device_id_data.insert(std::end(device_id_data), reinterpret_cast<const std::byte *>(instance_id.data() + semi_stable_part_index), reinterpret_cast<const std::byte *>(instance_id.data() + instance_id.size()));

        static const auto dump_device_id_data {[](const auto &data) -> std::string {
          if (data.empty()) {
            return {};
          }

          std::ostringstream output;
          output << "[";
          for (std::size_t i = 0; i < data.size(); ++i) {
            output << "0x" << std::setw(2) << std::setfill('0') << std::hex << std::uppercase << static_cast<int>(data[i]);
            if (i + 1 < data.size()) {
              output << " ";
            }
          }
          output << "]";

          return output.str();
        }};
        DD_LOG(verbose) << "Creating device id from EDID + instance ID: " << dump_device_id_data(device_id_data);
      }();
    }

    if (device_id_data.empty()) {
      // Using the device path as a fallback, which is always unique, but not as stable as the preferred one
      DD_LOG(verbose) << "Creating device id from path " << toUtf8(*this, device_path);
      device_id_data.insert(std::end(device_id_data), reinterpret_cast<const std::byte *>(device_path.data()), reinterpret_cast<const std::byte *>(device_path.data() + device_path.size()));
    }

    static constexpr boost::uuids::uuid ns_id {};  // null namespace = no salt
    const auto boost_uuid {boost::uuids::name_generator_sha1 {ns_id}(device_id_data.data(), device_id_data.size())};
    const std::string device_id {"{" + boost::uuids::to_string(boost_uuid) + "}"};

    DD_LOG(verbose) << "Created device id: " << toUtf8(*this, device_path) << " -> " << device_id;
    return device_id;
  }

  std::vector<std::byte> WinApiLayer::getEdid(const DISPLAYCONFIG_PATH_INFO &path) const {
    const auto device_path {getMonitorDevicePathWstr(*this, path)};
    if (device_path.empty()) {
      // Error already logged
      return {};
    }

    auto instance_id_and_edid {getInstanceIdAndEdid(*this, device_path)};
    return instance_id_and_edid ? std::get<1>(*instance_id_and_edid) : std::vector<std::byte> {};
  }

  std::string WinApiLayer::getMonitorDevicePath(const DISPLAYCONFIG_PATH_INFO &path) const {
    return toUtf8(*this, getMonitorDevicePathWstr(*this, path));
  }

  std::string WinApiLayer::getFriendlyName(const DISPLAYCONFIG_PATH_INFO &path) const {
    DISPLAYCONFIG_TARGET_DEVICE_NAME target_name = {};
    target_name.header.adapterId = path.targetInfo.adapterId;
    target_name.header.id = path.targetInfo.id;
    target_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target_name.header.size = sizeof(target_name);

    LONG result {DisplayConfigGetDeviceInfo(&target_name.header)};
    if (result != ERROR_SUCCESS) {
      DD_LOG(error) << getErrorString(result) << " failed to get target device name!";
      return {};
    }

    return target_name.flags.friendlyNameFromEdid ? toUtf8(*this, target_name.monitorFriendlyDeviceName) : std::string {};
  }

  std::string WinApiLayer::getDisplayName(const DISPLAYCONFIG_PATH_INFO &path) const {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name = {};
    source_name.header.id = path.sourceInfo.id;
    source_name.header.adapterId = path.sourceInfo.adapterId;
    source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source_name.header.size = sizeof(source_name);

    LONG result {DisplayConfigGetDeviceInfo(&source_name.header)};
    if (result != ERROR_SUCCESS) {
      DD_LOG(error) << getErrorString(result) << " failed to get display name!";
      return {};
    }

    return toUtf8(*this, source_name.viewGdiDeviceName);
  }

  LONG WinApiLayer::setDisplayConfig(std::vector<DISPLAYCONFIG_PATH_INFO> paths, std::vector<DISPLAYCONFIG_MODE_INFO> modes, UINT32 flags) {
    std::ostringstream detail;
    detail << "paths=" << paths.size() << " modes=" << modes.size() << " flags=0x" << std::hex << flags;
    ApiCallTimer timer("WinApiLayer::setDisplayConfig", detail.str());
    if ((flags & SDC_APPLY) != 0) {
      invalidateDisplayModeCache();
    }
    const bool topology_apply =
      (flags & SDC_TOPOLOGY_SUPPLIED) != 0 ||
      ((flags & SDC_USE_SUPPLIED_DISPLAY_CONFIG) != 0 && !paths.empty() && modes.empty());
    const bool recovery_allowed = detail::current_display_recovery_behavior() != DisplayRecoveryBehavior::Skip;
    const auto callWithFlags = [&](const std::vector<DISPLAYCONFIG_PATH_INFO> &p, const std::vector<DISPLAYCONFIG_MODE_INFO> &m, UINT32 f, const char *reason) -> LONG {
      auto invoke = [&](UINT32 actual_flags) -> LONG {
        return ::SetDisplayConfig(
          static_cast<UINT32>(p.size()),
          p.empty() ? nullptr : const_cast<DISPLAYCONFIG_PATH_INFO *>(p.data()),
          static_cast<UINT32>(m.size()),
          m.empty() ? nullptr : const_cast<DISPLAYCONFIG_MODE_INFO *>(m.data()),
          actual_flags
        );
      };

      LONG result = invoke(f);
      if ((result == ERROR_NOT_SUPPORTED || result == ERROR_INVALID_PARAMETER) && (f & SDC_VIRTUAL_MODE_AWARE)) {
        const UINT32 compat_flags = (f & ~SDC_VIRTUAL_MODE_AWARE);
        DD_LOG(warning) << getErrorString(result) << " while applying with SDC_VIRTUAL_MODE_AWARE during " << reason << "; retrying without it.";
        result = invoke(compat_flags);
      }

      if (result != ERROR_SUCCESS) {
        DD_LOG(warning) << getErrorString(result) << " while executing " << reason << ".";
      }

      return result;
    };

    const auto validateAndCallWithFlags = [&](const std::vector<DISPLAYCONFIG_PATH_INFO> &p, const std::vector<DISPLAYCONFIG_MODE_INFO> &m, UINT32 f, const char *reason) -> LONG {
      if ((f & SDC_APPLY) != 0) {
        const std::string validate_reason = std::string {reason} + " validation";
        const UINT32 validate_flags = (f & ~SDC_APPLY) | SDC_VALIDATE;
        const LONG validate_result = callWithFlags(p, m, validate_flags, validate_reason.c_str());
        if (validate_result != ERROR_SUCCESS) {
          return validate_result;
        }
      }

      return callWithFlags(p, m, f, reason);
    };

    const auto applyRequestedConfig = [&](UINT32 f, const char *context) -> LONG {
      return validateAndCallWithFlags(paths, modes, f, context);
    };

    const auto stackRecovery = [&]() {
      recoverDisplayStack();
    };

    LONG result = applyRequestedConfig(flags, "initial setDisplayConfig");

    if ((flags & SDC_APPLY) == 0) {
      return result;
    }

    if (result != ERROR_SUCCESS) {
      auto forceEnumeration = [&](QueryType type, const char *label) -> bool {
        const auto display_data = queryDisplayConfig(type);
        if (!display_data || display_data->m_paths.empty()) {
          return false;
        }
        static constexpr UINT32 reenum_flags = SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_FORCE_MODE_ENUMERATION | SDC_VIRTUAL_MODE_AWARE;
        const LONG reenum_result = validateAndCallWithFlags(display_data->m_paths, display_data->m_modes, reenum_flags, label);
        if (reenum_result == ERROR_SUCCESS) {
          result = applyRequestedConfig(flags, "post-force-enumeration apply");
        } else {
          result = reenum_result;
        }
        return true;
      };

      if (!forceEnumeration(QueryType::All, "forced mode enumeration")) {
        static_cast<void>(forceEnumeration(QueryType::Active, "forced mode enumeration (active-only)"));
      }

      if (result != ERROR_SUCCESS) {
        if (!recovery_allowed) {
          DD_LOG(debug) << "SetDisplayConfig failure; skipping display stack recovery (behavior=skip).";
          return result;
        }
        if (!topology_apply) {
          DD_LOG(debug) << "SetDisplayConfig failure; skipping display stack recovery (non-topology apply).";
          return result;
        }
        stackRecovery();
        result = applyRequestedConfig(flags, "post-recovery setDisplayConfig");
      }
    }

    return result;
  }

  void WinApiLayer::recoverDisplayStack() {
    DD_LOG(info) << "SetDisplayConfig detected failure; attempting display stack recovery.";

    auto log_result = [&](LONG result, const char *label) {
      if (result == ERROR_SUCCESS) {
        DD_LOG(info) << label << " succeeded.";
      } else {
        DD_LOG(warning) << getErrorString(result) << " while executing " << label << ".";
      }
    };

    const LONG restore_db = ::SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_USE_DATABASE_CURRENT);
    log_result(restore_db, "SetDisplayConfig(SDC_USE_DATABASE_CURRENT)");

    static constexpr std::array<UINT32, 4> topology_flags = {
      SDC_TOPOLOGY_INTERNAL,
      SDC_TOPOLOGY_EXTERNAL,
      SDC_TOPOLOGY_EXTEND,
      SDC_TOPOLOGY_CLONE
    };

    for (const auto flag : topology_flags) {
      const LONG topo_result = ::SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | flag);
      log_result(topo_result, "SetDisplayConfig(topology jog)");
      if (topo_result == ERROR_SUCCESS) {
        break;
      }
    }

    const LONG cds_reset = ::ChangeDisplaySettingsExA(nullptr, nullptr, nullptr, CDS_RESET, nullptr);
    if (cds_reset != DISP_CHANGE_SUCCESSFUL) {
      DD_LOG(warning) << "ChangeDisplaySettingsExA with CDS_RESET failed: " << cds_reset;
    } else {
      DD_LOG(info) << "ChangeDisplaySettingsExA with CDS_RESET succeeded.";
    }

    const auto display_data = queryDisplayConfig(QueryType::All);
    if (display_data && !display_data->m_paths.empty()) {
      auto callWithFlags = [&](const std::vector<DISPLAYCONFIG_PATH_INFO> &p, const std::vector<DISPLAYCONFIG_MODE_INFO> &m, UINT32 f, const char *reason) -> LONG {
        auto invoke = [&](UINT32 actual_flags) -> LONG {
          return ::SetDisplayConfig(
            static_cast<UINT32>(p.size()),
            p.empty() ? nullptr : const_cast<DISPLAYCONFIG_PATH_INFO *>(p.data()),
            static_cast<UINT32>(m.size()),
            m.empty() ? nullptr : const_cast<DISPLAYCONFIG_MODE_INFO *>(m.data()),
            actual_flags
          );
        };

        LONG result = invoke(f);
        if ((result == ERROR_NOT_SUPPORTED || result == ERROR_INVALID_PARAMETER) && (f & SDC_VIRTUAL_MODE_AWARE)) {
          const UINT32 compat_flags = (f & ~SDC_VIRTUAL_MODE_AWARE);
          DD_LOG(warning) << getErrorString(result) << " while applying with SDC_VIRTUAL_MODE_AWARE during " << reason << "; retrying without it.";
          result = invoke(compat_flags);
        }

        if (result != ERROR_SUCCESS) {
          DD_LOG(warning) << getErrorString(result) << " while executing " << reason << ".";
        }

        return result;
      };

      auto validateAndCallWithFlags = [&](const std::vector<DISPLAYCONFIG_PATH_INFO> &p, const std::vector<DISPLAYCONFIG_MODE_INFO> &m, UINT32 f, const char *reason) -> LONG {
        if ((f & SDC_APPLY) != 0) {
          const std::string validate_reason = std::string {reason} + " validation";
          const UINT32 validate_flags = (f & ~SDC_APPLY) | SDC_VALIDATE;
          const LONG validate_result = callWithFlags(p, m, validate_flags, validate_reason.c_str());
          if (validate_result != ERROR_SUCCESS) {
            return validate_result;
          }
        }

        return callWithFlags(p, m, f, reason);
      };

      static constexpr UINT32 reenum_flags = SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_FORCE_MODE_ENUMERATION | SDC_VIRTUAL_MODE_AWARE;
      static_cast<void>(validateAndCallWithFlags(display_data->m_paths, display_data->m_modes, reenum_flags, "post-recovery forced mode enumeration"));
    }
  }

  std::optional<HdrState> WinApiLayer::getHdrState(const DISPLAYCONFIG_PATH_INFO &path) const {
    ApiCallTimer timer("WinApiLayer::getHdrState", pathIdentifier(path));
    if (is_W11_24H2_OrAbove(*this)) {
      DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 color_info = {};
      color_info.header.adapterId = path.targetInfo.adapterId;
      color_info.header.id = path.targetInfo.id;
      color_info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
      color_info.header.size = sizeof(color_info);

      LONG result {DisplayConfigGetDeviceInfo(&color_info.header)};
      if (result != ERROR_SUCCESS) {
        DD_LOG(error) << getErrorString(result) << " failed to get advanced color info 2!";
        return std::nullopt;
      }

      return color_info.highDynamicRangeSupported ? std::make_optional(color_info.highDynamicRangeUserEnabled ? HdrState::Enabled : HdrState::Disabled) : std::nullopt;
    }

    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color_info = {};
    color_info.header.adapterId = path.targetInfo.adapterId;
    color_info.header.id = path.targetInfo.id;
    color_info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    color_info.header.size = sizeof(color_info);

    LONG result {DisplayConfigGetDeviceInfo(&color_info.header)};
    if (result != ERROR_SUCCESS) {
      DD_LOG(error) << getErrorString(result) << " failed to get advanced color info!";
      return std::nullopt;
    }

    return color_info.advancedColorSupported ? std::make_optional(color_info.advancedColorEnabled ? HdrState::Enabled : HdrState::Disabled) : std::nullopt;
  }

  bool WinApiLayer::setHdrState(const DISPLAYCONFIG_PATH_INFO &path, HdrState state) {
    std::ostringstream detail;
    detail << pathIdentifier(path) << " state=" << hdrStateToString(state);
    ApiCallTimer timer("WinApiLayer::setHdrState", detail.str());
    if (is_W11_24H2_OrAbove(*this)) {
      DISPLAYCONFIG_SET_HDR_STATE hdr_state = {};
      hdr_state.header.adapterId = path.targetInfo.adapterId;
      hdr_state.header.id = path.targetInfo.id;
      hdr_state.header.type = DISPLAYCONFIG_DEVICE_INFO_TYPE(16);
      hdr_state.header.size = sizeof(hdr_state);
      hdr_state.enableHdr = state == HdrState::Enabled ? 1 : 0;

      LONG result {DisplayConfigSetDeviceInfo(&hdr_state.header)};
      if (result != ERROR_SUCCESS) {
        DD_LOG(error) << getErrorString(result) << " failed to set HDR state!";
        return false;
      }

      return true;
    }

    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE color_state = {};
    color_state.header.adapterId = path.targetInfo.adapterId;
    color_state.header.id = path.targetInfo.id;
    color_state.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
    color_state.header.size = sizeof(color_state);
    color_state.enableAdvancedColor = state == HdrState::Enabled ? 1 : 0;

    LONG result {DisplayConfigSetDeviceInfo(&color_state.header)};
    if (result != ERROR_SUCCESS) {
      DD_LOG(error) << getErrorString(result) << " failed to set advanced color info!";
      return false;
    }

    return true;
  }

  std::optional<Resolution> WinApiLayer::getPreferredResolution(const DISPLAYCONFIG_PATH_INFO &path) const {
    ApiCallTimer timer("WinApiLayer::getPreferredResolution", pathIdentifier(path));
    DISPLAYCONFIG_TARGET_PREFERRED_MODE preferred = {};
    preferred.header.adapterId = path.targetInfo.adapterId;
    preferred.header.id = path.targetInfo.id;
    preferred.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_PREFERRED_MODE;
    preferred.header.size = sizeof(preferred);

    LONG result {DisplayConfigGetDeviceInfo(&preferred.header)};
    if (result != ERROR_SUCCESS) {
      DD_LOG(error) << getErrorString(result) << " failed to get preferred target mode!";
      return std::nullopt;
    }

    const auto &active = preferred.targetMode.targetVideoSignalInfo.activeSize;
    if (active.cx == 0 || active.cy == 0) {
      DD_LOG(debug) << "Preferred mode returned zero size.";
      return std::nullopt;
    }

    return Resolution {static_cast<unsigned int>(active.cx), static_cast<unsigned int>(active.cy)};
  }

    std::vector<DisplayMode> WinApiLayer::getSupportedDisplayModes(const DISPLAYCONFIG_PATH_INFO &path) const {
    ApiCallTimer timer("WinApiLayer::getSupportedDisplayModes", pathIdentifier(path));
    const std::string display_name = getDisplayName(path);
    if (display_name.empty()) {
      // Inactive target may not have a display name; return empty list.
      DD_LOG(debug) << "Display name is empty; cannot enumerate modes for inactive target.";
      return {};
    }

    std::vector<DisplayMode> cached_modes;
    if (tryGetCachedDisplayModes(display_name, cached_modes)) {
      DD_LOG(debug) << "Returning cached display modes for " << display_name << ".";
      return cached_modes;
    }

    struct DisplayModeKey {
      unsigned int m_width;
      unsigned int m_height;
      unsigned int m_refresh_num;
      unsigned int m_refresh_den;

      bool operator==(const DisplayModeKey &o) const {
        return m_width == o.m_width && m_height == o.m_height && m_refresh_num == o.m_refresh_num && m_refresh_den == o.m_refresh_den;
      }
    };

    struct DisplayModeKeyHash {
      std::size_t operator()(const DisplayModeKey &k) const noexcept {
        std::size_t hash = static_cast<std::size_t>(k.m_width);
        hash = (hash << 16) ^ static_cast<std::size_t>(k.m_height);
        hash = (hash << 16) ^ static_cast<std::size_t>(k.m_refresh_num);
        hash = (hash << 16) ^ static_cast<std::size_t>(k.m_refresh_den);
        return hash;
      }
    };

    const auto enumerate_via_dxgi = [&]() -> std::vector<DisplayMode> {
      std::vector<DisplayMode> result;
      std::unordered_set<DisplayModeKey, DisplayModeKeyHash> seen;
      const auto add_mode = [&](unsigned int width, unsigned int height, unsigned int numerator, unsigned int denominator) {
        if (width == 0 || height == 0) {
          return;
        }

        const unsigned int sanitized_denominator = denominator == 0 ? 1U : denominator;
        DisplayModeKey key {width, height, numerator, sanitized_denominator};
        if (seen.insert(key).second) {
          result.push_back(DisplayMode {Resolution {width, height}, Rational {numerator, sanitized_denominator}});
        }
      };

      DISPLAYCONFIG_TARGET_DEVICE_NAME target_name = {};
      target_name.header.adapterId = path.targetInfo.adapterId;
      target_name.header.id = path.targetInfo.id;
      target_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
      target_name.header.size = sizeof(target_name);

      const auto normalized_display_name = normalizeDisplayName(display_name);
      const auto target_monitor = monitorFromDisplayName(display_name);

      std::optional<UINT32> connector_instance;
      const LONG target_result {DisplayConfigGetDeviceInfo(&target_name.header)};
      if (target_result == ERROR_SUCCESS) {
        connector_instance = target_name.connectorInstance;
      } else {
        DD_LOG(debug) << getErrorString(target_result) << " failed to get target device info while enumerating display modes for " << display_name << '.';
      }

      Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
      HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
      if (FAILED(hr)) {
        DD_LOG(debug) << "CreateDXGIFactory1 failed with HRESULT=0x" << std::hex << hr << " while enumerating display modes for " << display_name << ".";
        return {};
      }

      const LUID adapter_luid = path.targetInfo.adapterId;
      bool matched_output = false;

      const auto query_output_modes = [&](Microsoft::WRL::ComPtr<IDXGIOutput> &output) -> bool {
        if (!output) {
          return false;
        }

        matched_output = true;
        constexpr UINT flags = DXGI_ENUM_MODES_INTERLACED | DXGI_ENUM_MODES_SCALING;
        constexpr DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM};

        for (DXGI_FORMAT format : formats) {
          UINT mode_count = 0;
          hr = output->GetDisplayModeList(format, flags, &mode_count, nullptr);
          if (FAILED(hr)) {
            DD_LOG(debug) << "GetDisplayModeList (count) failed for format " << static_cast<int>(format) << " with HRESULT=0x" << std::hex << hr << '.';
            continue;
          }

          if (mode_count == 0) {
            continue;
          }

          std::vector<DXGI_MODE_DESC> modes(mode_count);
          hr = output->GetDisplayModeList(format, flags, &mode_count, modes.data());
          if (FAILED(hr)) {
            DD_LOG(debug) << "GetDisplayModeList failed for format " << static_cast<int>(format) << " with HRESULT=0x" << std::hex << hr << '.';
            continue;
          }

          modes.resize(mode_count);
          for (const auto &mode : modes) {
            add_mode(mode.Width, mode.Height, mode.RefreshRate.Numerator, mode.RefreshRate.Denominator);
          }

          if (!result.empty()) {
            return true;
          }
        }

        return false;
      };

      for (UINT adapter_index = 0;; ++adapter_index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(adapter_index, adapter.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND) {
          break;
        }

        if (FAILED(hr)) {
          DD_LOG(debug) << "EnumAdapters1 failed at index " << adapter_index << " with HRESULT=0x" << std::hex << hr << '.';
          return {};
        }

        DXGI_ADAPTER_DESC1 adapter_desc;
        if (FAILED(adapter->GetDesc1(&adapter_desc))) {
          DD_LOG(debug) << "GetDesc1 failed for adapter index " << adapter_index << '.';
          continue;
        }

        if (adapter_desc.AdapterLuid.HighPart != adapter_luid.HighPart || adapter_desc.AdapterLuid.LowPart != adapter_luid.LowPart) {
          continue;
        }

        if (connector_instance) {
          Microsoft::WRL::ComPtr<IDXGIOutput> output;
          hr = adapter->EnumOutputs(*connector_instance, output.GetAddressOf());
          if (hr == DXGI_ERROR_NOT_FOUND) {
            DD_LOG(debug) << "EnumOutputs could not find connector " << *connector_instance << " while enumerating display modes for " << display_name << '.';
          } else if (FAILED(hr)) {
            DD_LOG(debug) << "EnumOutputs failed at connector " << *connector_instance << " with HRESULT=0x" << std::hex << hr << '.';
            return {};
          } else {
            if (query_output_modes(output)) {
              return result;
            }
          }
        }

        for (UINT output_index = 0;; ++output_index) {
          Microsoft::WRL::ComPtr<IDXGIOutput> output;
          hr = adapter->EnumOutputs(output_index, output.GetAddressOf());
          if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
          }

          if (FAILED(hr)) {
            DD_LOG(debug) << "EnumOutputs failed at index " << output_index << " with HRESULT=0x" << std::hex << hr << '.';
            return {};
          }

          DXGI_OUTPUT_DESC output_desc;
          if (FAILED(output->GetDesc(&output_desc))) {
            DD_LOG(debug) << "GetDesc failed for output index " << output_index << '.';
            continue;
          }

          const std::wstring dxgi_name_w {output_desc.DeviceName};
          const auto dxgi_display_name = toUtf8(*this, dxgi_name_w);
          const bool monitor_matches = target_monitor && output_desc.Monitor && *target_monitor == output_desc.Monitor;
          bool name_matches = false;
          if (!normalized_display_name.empty()) {
            name_matches = normalizeDisplayName(dxgi_display_name) == normalized_display_name;
          }
          if (!name_matches && !monitor_matches) {
            continue;
          }

          if (query_output_modes(output)) {
            return result;
          }

          DD_LOG(debug) << "DXGI returned no display modes for " << display_name << '.';
          return {};
        }
      }

      if (!matched_output) {
        DD_LOG(debug) << "DXGI could not match adapter/output for " << display_name << '.';
      }

      return result;
    };

    const auto enumerate_via_gdi = [&]() -> std::vector<DisplayMode> {
      std::vector<DisplayMode> result;
      std::unordered_set<DisplayModeKey, DisplayModeKeyHash> seen;
      const auto add_mode = [&](unsigned int width, unsigned int height, unsigned int frequency) {
        if (width == 0 || height == 0) {
          return;
        }

        DisplayModeKey key {width, height, frequency, 1};
        if (seen.insert(key).second) {
          result.push_back(DisplayMode {Resolution {width, height}, Rational {frequency, 1}});
        }
      };

      for (DWORD i = 0;; ++i) {
        DEVMODEA dm {};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsExA(display_name.c_str(), i, &dm, 0) != TRUE) {
          break;
        }

        if (!(dm.dmFields & (DM_PELSWIDTH | DM_PELSHEIGHT))) {
          continue;
        }

        const unsigned int width = static_cast<unsigned int>(dm.dmPelsWidth);
        const unsigned int height = static_cast<unsigned int>(dm.dmPelsHeight);
        unsigned int frequency = 0;
        if (dm.dmFields & DM_DISPLAYFREQUENCY) {
          frequency = static_cast<unsigned int>(dm.dmDisplayFrequency);
        }

        add_mode(width, height, frequency);
      }

      return result;
    };

    std::vector<DisplayMode> supported = enumerate_via_dxgi();
    if (supported.empty()) {
      supported = enumerate_via_gdi();
    }

    storeDisplayModesInCache(display_name, supported);
    return supported;
  }

  std::optional<Rational> WinApiLayer::getDisplayScale(const std::string &display_name, const DISPLAYCONFIG_SOURCE_MODE &source_mode) const {
    std::ostringstream detail;
    detail << "display=" << display_name << " width=" << source_mode.width << " height=" << source_mode.height;
    ApiCallTimer timer("WinApiLayer::getDisplayScale", detail.str());
    // Note: implementation based on https://stackoverflow.com/a/74046173
    struct EnumData {
      std::string m_display_name;
      std::optional<int> m_width;
    };

    EnumData enum_data {display_name, std::nullopt};
    EnumDisplayMonitors(
      nullptr,
      nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM user_data) -> BOOL {
        auto *data = reinterpret_cast<EnumData *>(user_data);
        if (data == nullptr) {
          // Sanity check
          DD_LOG(error) << "EnumData is a nullptr!";
          return FALSE;
        }

        MONITORINFOEXA monitor_info {sizeof(MONITORINFOEXA)};
        if (GetMonitorInfoA(monitor, &monitor_info)) {
          if (data->m_display_name == monitor_info.szDevice) {
            data->m_width = monitor_info.rcMonitor.right - monitor_info.rcMonitor.left;
            return FALSE;
          }
        }

        return TRUE;
      },
      reinterpret_cast<LPARAM>(&enum_data)
    );

    if (!enum_data.m_width) {
      DD_LOG(debug) << "Failed to get monitor info for " << display_name << "!";
      return std::nullopt;
    }

    if (*enum_data.m_width * source_mode.width == 0) {
      DD_LOG(debug) << "Cannot get display scale for " << display_name << " from a width of 0!";
      return std::nullopt;
    }

    const auto width {static_cast<double>(*enum_data.m_width) / static_cast<double>(source_mode.width)};
    return Rational {static_cast<unsigned int>(std::round((static_cast<double>(GetDpiForSystem()) / 96. / width) * 100)), 100};
  }

  void WinApiLayer::invalidateDisplayModeCache() const {
    std::lock_guard<std::mutex> lock(m_display_mode_cache_mutex);
    m_display_mode_cache.clear();
  }

  bool WinApiLayer::tryGetCachedDisplayModes(const std::string &display_name, std::vector<DisplayMode> &modes) const {
    std::lock_guard<std::mutex> lock(m_display_mode_cache_mutex);
    const auto it = m_display_mode_cache.find(display_name);
    if (it == m_display_mode_cache.end()) {
      return false;
    }

    modes = it->second.m_modes;
    return true;
  }

  void WinApiLayer::storeDisplayModesInCache(const std::string &display_name, const std::vector<DisplayMode> &modes) const {
    std::lock_guard<std::mutex> lock(m_display_mode_cache_mutex);
    m_display_mode_cache[display_name].m_modes = modes;
  }
}  // namespace display_device
