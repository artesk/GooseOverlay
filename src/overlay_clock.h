#pragma once

#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>

struct OverlayClock {
    OverlayClock();
    bool enabled{true};
    // An empty key follows the computer's current time zone.
    std::wstring zoneId{L"Russian Standard Time"};
    DYNAMIC_TIME_ZONE_INFORMATION zone{};
};

inline std::vector<DYNAMIC_TIME_ZONE_INFORMATION> clockTimeZones() {
    std::vector<DYNAMIC_TIME_ZONE_INFORMATION> zones;
    for (DWORD index = 0;; ++index) {
        DYNAMIC_TIME_ZONE_INFORMATION zone{};
        if (EnumDynamicTimeZoneInformation(index, &zone) != ERROR_SUCCESS) break;
        zones.push_back(zone);
    }
    std::sort(zones.begin(), zones.end(), [](const auto& a, const auto& b) {
        if (a.Bias != b.Bias) return a.Bias > b.Bias;
        return wcscmp(a.StandardName, b.StandardName) < 0;
    });
    return zones;
}

inline bool setClockTimeZone(OverlayClock& clock, const std::wstring& key) {
    clock.zoneId.clear();
    clock.zone = {};
    if (key.empty()) return true;
    for (const auto& zone : clockTimeZones()) {
        if (key == zone.TimeZoneKeyName) {
            clock.zoneId = key;
            clock.zone = zone;
            return true;
        }
    }
    return false; // Unknown keys safely fall back to computer time.
}

inline OverlayClock::OverlayClock() {
    // Avoid enumerating the registry whenever a render-thread Config is copied.
    zone.Bias = -180;
    wcscpy_s(zone.TimeZoneKeyName, L"Russian Standard Time");
}

inline std::wstring clockZoneLabel(const DYNAMIC_TIME_ZONE_INFORMATION& zone) {
    // Use the same localized city-based labels as Windows time zone settings.
    const auto path = std::wstring(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones\\") + zone.TimeZoneKeyName;
    HKEY key{};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) == ERROR_SUCCESS) {
        wchar_t label[512]{};
        DWORD bytes = sizeof(label);
        LSTATUS status = RegLoadMUIStringW(key, L"MUI_Display", label, sizeof(label), nullptr, 0, nullptr);
        if (status != ERROR_SUCCESS)
            status = RegGetValueW(key, nullptr, L"Display", RRF_RT_REG_SZ, nullptr, label, &bytes);
        RegCloseKey(key);
        if (status == ERROR_SUCCESS && label[0]) return label;
    }
    const LONG offset = -(zone.Bias + zone.StandardBias);
    const LONG magnitude = offset < 0 ? -offset : offset;
    wchar_t prefix[32]{};
    swprintf_s(prefix, L"(UTC%c%02ld:%02ld) ", offset < 0 ? L'-' : L'+', magnitude / 60, magnitude % 60);
    return std::wstring(prefix) + zone.StandardName;
}

inline std::wstring formatOverlayClock(const OverlayClock& clock, const SYSTEMTIME& utc) {
    if (!clock.enabled) return {};
    SYSTEMTIME local{};
    DYNAMIC_TIME_ZONE_INFORMATION computerZone{};
    const auto* zone = &clock.zone;
    if (clock.zoneId.empty()) {
        if (GetDynamicTimeZoneInformation(&computerZone) == TIME_ZONE_ID_INVALID) return L"--:--:--";
        zone = &computerZone;
    }
    if (!SystemTimeToTzSpecificLocalTimeEx(zone, &utc, &local)) return L"--:--:--";
    wchar_t text[16]{};
    swprintf_s(text, L"%02u:%02u:%02u", static_cast<unsigned>(local.wHour),
        static_cast<unsigned>(local.wMinute), static_cast<unsigned>(local.wSecond));
    return text;
}

inline std::wstring currentOverlayClock(const OverlayClock& clock) {
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    return formatOverlayClock(clock, utc);
}
