#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "teamspeak/public_definitions.h"
#include "teamspeak/public_errors.h"
#include "ts3_functions.h"
#include "leader_portrait.h"
#include "paint_buffer.h"
#include "overlay_clock.h"

namespace {

constexpr int kPluginApiVersion = 26;
constexpr wchar_t kOverlayClass[] = L"GooseOverlayWindow";
constexpr wchar_t kSettingsClass[] = L"GooseOverlaySettingsWindow";
constexpr COLORREF kTransparentColor = RGB(1, 2, 3);
constexpr UINT kMoveModeMessage = WM_APP + 1;
constexpr UINT kPositionChangedMessage = WM_APP + 2;
constexpr UINT kMoveStateChangedMessage = WM_APP + 3;
constexpr int kMoveHintHeight = 40;

struct ClientState {
    anyID id{};
    std::wstring name;
    bool talking{};
    bool whisper{};
    bool muted{};
    bool soundMuted{};
};

struct Config {
    OverlayClock clock;
    std::wstring leaderName{L"восток"};
    bool leaderEnabled{true};
    int leaderSize{128};
    std::vector<std::wstring> processes;
    std::wstring titleContains;
    bool hideWhenUnfocused{true};
    int opacity{225};
    int width{310};
    int offsetX{24};
    int offsetY{72};
    int rowHeight{30};
    int fontSize{17};
    int maxClients{16};
};

TS3Functions g_ts3{};
std::mutex g_modelMutex;
std::vector<ClientState> g_clients;
// Incoming whispers may originate outside the subscribed/current channel.
std::unordered_map<anyID, ClientState> g_whisperClients;
std::unordered_map<anyID, std::pair<bool, bool>> g_talkState;
std::mutex g_configMutex;
Config g_config;
std::wstring g_configPath;
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_stop{false};
std::atomic<uint64> g_server{0};
std::atomic<HWND> g_overlayWindow{nullptr};
std::atomic<HWND> g_gameWindow{nullptr};
std::atomic<HWND> g_settingsWindow{nullptr};
std::atomic<bool> g_settingsOpen{false};
std::atomic<bool> g_moveMode{false};
std::thread g_overlayThread;
std::string g_pluginId;
HINSTANCE g_module{};

std::wstring utf8ToWide(const char* text) {
    if (!text || !*text) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (count <= 1) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), count);
    result.pop_back();
    return result;
}

std::wstring trim(std::wstring value) {
    const auto notSpace = [](wchar_t c) { return !iswspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

// Created, accessed and destroyed exclusively on the overlay window thread.
LeaderPortrait* g_leaderPortrait{};
PaintBuffer* g_paintBuffer{};

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}

std::vector<std::wstring> splitProcesses(const std::wstring& input) {
    std::vector<std::wstring> result;
    std::wstringstream stream(input);
    std::wstring item;
    while (std::getline(stream, item, L',')) {
        item = lower(trim(std::move(item)));
        if (!item.empty()) result.push_back(std::move(item));
    }
    return result;
}

std::wstring joinProcesses(const std::vector<std::wstring>& processes) {
    std::wstring result;
    for (const auto& process : processes) {
        if (!result.empty()) result += L",";
        result += process;
    }
    return result;
}

int readInt(const wchar_t* section, const wchar_t* key, int fallback, int minValue, int maxValue) {
    const int value = GetPrivateProfileIntW(section, key, fallback, g_configPath.c_str());
    return std::clamp(value, minValue, maxValue);
}

void invalidateOverlay();

void writeDefaultConfig() {
    if (GetFileAttributesW(g_configPath.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    WritePrivateProfileStringW(L"game", L"processes", L"BattlesForGlory.exe,Battles For Glory.exe,BFG.exe", g_configPath.c_str());
    WritePrivateProfileStringW(L"game", L"title_contains", L"Battles for Glory", g_configPath.c_str());
    WritePrivateProfileStringW(L"game", L"hide_when_unfocused", L"1", g_configPath.c_str());
    WritePrivateProfileStringW(L"overlay", L"enabled", L"1", g_configPath.c_str());
    WritePrivateProfileStringW(L"overlay", L"opacity", L"225", g_configPath.c_str());
    WritePrivateProfileStringW(L"overlay", L"width", L"310", g_configPath.c_str());
    WritePrivateProfileStringW(L"overlay", L"offset_x", L"24", g_configPath.c_str());
    WritePrivateProfileStringW(L"overlay", L"offset_y", L"72", g_configPath.c_str());
    WritePrivateProfileStringW(L"overlay", L"row_height", L"30", g_configPath.c_str());
    WritePrivateProfileStringW(L"overlay", L"font_size", L"17", g_configPath.c_str());
    WritePrivateProfileStringW(L"overlay", L"max_clients", L"16", g_configPath.c_str());
}

void loadConfig() {
    writeDefaultConfig();
    wchar_t buffer[1024]{};
    GetPrivateProfileStringW(L"game", L"processes", L"BattlesForGlory.exe,Battles For Glory.exe,bsfg.exe", buffer, 1024, g_configPath.c_str());
    Config next;
    next.processes = splitProcesses(buffer);
    const auto defaultLeader = encodeLeaderName(next.leaderName);
    GetPrivateProfileStringW(L"overlay", L"leader_name", defaultLeader.c_str(), buffer, 1024, g_configPath.c_str());
    next.leaderName = trim(decodeLeaderName(buffer));
    if (next.leaderName.empty()) next.leaderName = L"восток";
    next.leaderEnabled = readInt(L"overlay", L"leader_enabled", 1, 0, 1) != 0;
    next.leaderSize = readInt(L"overlay", L"leader_size", 128, 64, 256);
    next.clock.enabled = readInt(L"overlay", L"clock_enabled", 1, 0, 1) != 0;
    GetPrivateProfileStringW(L"overlay", L"clock_timezone", L"Russian Standard Time", buffer, 1024, g_configPath.c_str());
    setClockTimeZone(next.clock, buffer);
    GetPrivateProfileStringW(L"game", L"title_contains", L"Battles for Glory", buffer, 1024, g_configPath.c_str());
    next.titleContains = lower(trim(buffer));
    next.hideWhenUnfocused = readInt(L"game", L"hide_when_unfocused", 1, 0, 1) != 0;
    next.opacity = readInt(L"overlay", L"opacity", 225, 40, 255);
    next.width = readInt(L"overlay", L"width", 310, 180, 800);
    next.offsetX = readInt(L"overlay", L"offset_x", 24, 0, 32767);
    next.offsetY = readInt(L"overlay", L"offset_y", 72, 0, 32767);
    next.rowHeight = readInt(L"overlay", L"row_height", 30, 20, 80);
    next.fontSize = readInt(L"overlay", L"font_size", 17, 10, 44);
    next.maxClients = readInt(L"overlay", L"max_clients", 16, 1, 64);
    g_enabled = readInt(L"overlay", L"enabled", 1, 0, 1) != 0;
    std::lock_guard<std::mutex> lock(g_configMutex);
    g_config = std::move(next);
}

void persistEnabled() {
    WritePrivateProfileStringW(L"overlay", L"enabled", g_enabled ? L"1" : L"0", g_configPath.c_str());
}

bool saveConfig(const Config& config, bool enabled) {
    if (!WritePrivateProfileStringW(L"overlay", L"clock_enabled", config.clock.enabled ? L"1" : L"0", g_configPath.c_str()) ||
        !WritePrivateProfileStringW(L"overlay", L"clock_timezone", config.clock.zoneId.c_str(), g_configPath.c_str())) return false;
    if (!WritePrivateProfileStringW(L"overlay", L"leader_name",
        encodeLeaderName(config.leaderName).c_str(), g_configPath.c_str())) return false;
    if (!WritePrivateProfileStringW(L"overlay", L"leader_enabled", config.leaderEnabled ? L"1" : L"0", g_configPath.c_str()) ||
        !WritePrivateProfileStringW(L"overlay", L"leader_size", std::to_wstring(config.leaderSize).c_str(), g_configPath.c_str())) return false;
    const std::wstring processes = joinProcesses(config.processes);
    WritePrivateProfileStringW(L"game", L"processes", processes.c_str(), g_configPath.c_str());
    WritePrivateProfileStringW(L"game", L"title_contains", config.titleContains.c_str(), g_configPath.c_str());
    WritePrivateProfileStringW(L"game", L"hide_when_unfocused", config.hideWhenUnfocused ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"overlay", L"enabled", enabled ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"overlay", L"opacity", std::to_wstring(config.opacity).c_str(), g_configPath.c_str());
    WritePrivateProfileStringW(L"overlay", L"width", std::to_wstring(config.width).c_str(), g_configPath.c_str());
    if (!WritePrivateProfileStringW(L"overlay", L"offset_x", std::to_wstring(config.offsetX).c_str(), g_configPath.c_str()) ||
        !WritePrivateProfileStringW(L"overlay", L"offset_y", std::to_wstring(config.offsetY).c_str(), g_configPath.c_str())) return false;
    WritePrivateProfileStringW(L"overlay", L"row_height", std::to_wstring(config.rowHeight).c_str(), g_configPath.c_str());
    WritePrivateProfileStringW(L"overlay", L"font_size", std::to_wstring(config.fontSize).c_str(), g_configPath.c_str());
    WritePrivateProfileStringW(L"overlay", L"max_clients", std::to_wstring(config.maxClients).c_str(), g_configPath.c_str());
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        g_config = config;
    }
    g_enabled = enabled;
    g_gameWindow = nullptr;
    invalidateOverlay();
    return true;
}

void logMessage(const char* message, LogLevel level = LogLevel_INFO) {
    if (g_ts3.logMessage) g_ts3.logMessage(message, level, "GooseOverlay", g_server.load());
}

void printMessage(const std::string& message) {
    if (g_ts3.printMessageToCurrentTab) g_ts3.printMessageToCurrentTab(message.c_str());
}

void invalidateOverlay() {
    const HWND overlay = g_overlayWindow.load();
    if (overlay) InvalidateRect(overlay, nullptr, FALSE);
}

void clearClients() {
    std::lock_guard<std::mutex> lock(g_modelMutex);
    g_clients.clear();
    g_whisperClients.clear();
    g_talkState.clear();
    invalidateOverlay();
}

void refreshClients(uint64 server) {
    if (!server || !g_ts3.getClientID || !g_ts3.getChannelOfClient || !g_ts3.getChannelClientList) {
        clearClients();
        return;
    }

    anyID self{};
    uint64 channel{};
    anyID* ids = nullptr;
    if (g_ts3.getClientID(server, &self) != ERROR_ok ||
        g_ts3.getChannelOfClient(server, self, &channel) != ERROR_ok ||
        g_ts3.getChannelClientList(server, channel, &ids) != ERROR_ok || !ids) {
        clearClients();
        return;
    }

    std::vector<ClientState> next;
    for (size_t i = 0; ids[i] != 0; ++i) {
        char* nickname = nullptr;
        if (g_ts3.getClientVariableAsString(server, ids[i], CLIENT_NICKNAME, &nickname) != ERROR_ok || !nickname) continue;
        ClientState client;
        client.id = ids[i];
        client.name = utf8ToWide(nickname);
        g_ts3.freeMemory(nickname);
        int inputMuted{};
        if (g_ts3.getClientVariableAsInt(server, client.id, CLIENT_INPUT_MUTED, &inputMuted) == ERROR_ok) {
            client.muted = inputMuted != 0;
        }
        int outputMuted{};
        int outputOnlyMuted{};
        const bool hasOutputMuted = g_ts3.getClientVariableAsInt(server, client.id, CLIENT_OUTPUT_MUTED, &outputMuted) == ERROR_ok;
        const bool hasOutputOnlyMuted = g_ts3.getClientVariableAsInt(server, client.id, CLIENT_OUTPUTONLY_MUTED, &outputOnlyMuted) == ERROR_ok;
        client.soundMuted = (hasOutputMuted && outputMuted != 0) || (hasOutputOnlyMuted && outputOnlyMuted != 0);
        {
            std::lock_guard<std::mutex> lock(g_modelMutex);
            const auto talk = g_talkState.find(client.id);
            if (talk != g_talkState.end()) {
                client.talking = talk->second.first;
                client.whisper = client.talking && talk->second.second;
            }
        }
        next.push_back(std::move(client));
    }
    g_ts3.freeMemory(ids);

    std::stable_sort(next.begin(), next.end(), [](const ClientState& a, const ClientState& b) {
        return a.talking > b.talking;
    });
    {
        std::lock_guard<std::mutex> lock(g_modelMutex);
        g_clients = std::move(next);
    }
    invalidateOverlay();
}

std::wstring processName(HWND window) {
    DWORD processId{};
    GetWindowThreadProcessId(window, &processId);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return {};
    wchar_t path[32768]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &length);
    CloseHandle(process);
    if (!ok) return {};
    const wchar_t* slash = wcsrchr(path, L'\\');
    return lower(slash ? slash + 1 : path);
}

struct FindGameContext {
    Config config;
    HWND found{};
};

BOOL CALLBACK findGameCallback(HWND window, LPARAM parameter) {
    auto* context = reinterpret_cast<FindGameContext*>(parameter);
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) return TRUE;

    const std::wstring executable = processName(window);
    const bool processMatches = std::find(context->config.processes.begin(), context->config.processes.end(), executable) != context->config.processes.end();
    bool titleMatches = false;
    if (!context->config.titleContains.empty()) {
        wchar_t title[512]{};
        GetWindowTextW(window, title, static_cast<int>(std::size(title)));
        titleMatches = lower(title).find(context->config.titleContains) != std::wstring::npos;
    }
    if (processMatches || titleMatches) {
        context->found = window;
        return FALSE;
    }
    return TRUE;
}

HWND findGameWindow(const Config& config) {
    FindGameContext context{config, nullptr};
    EnumWindows(findGameCallback, reinterpret_cast<LPARAM>(&context));
    return context.found;
}

bool getClientAreaOnScreen(HWND window, RECT& result) {
    RECT client{};
    POINT topLeft{};
    if (!GetClientRect(window, &client) || !ClientToScreen(window, &topLeft)) return false;
    result = {topLeft.x, topLeft.y, topLeft.x + client.right, topLeft.y + client.bottom};
    return client.right > 0 && client.bottom > 0;
}

void drawTextLine(HDC dc, const std::wstring& text, RECT rect, COLORREF color, HFONT font, UINT format) {
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    const HGDIOBJ oldFont = SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format | DT_NOPREFIX | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, oldFont);
}

void drawMutedMicrophone(HDC dc, int x, int y) {
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(232, 83, 83));
    const HGDIOBJ oldPen = SelectObject(dc, pen);
    const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));

    RoundRect(dc, x + 5, y + 1, x + 12, y + 12, 5, 5);
    MoveToEx(dc, x + 8, y + 12, nullptr);
    LineTo(dc, x + 8, y + 16);
    MoveToEx(dc, x + 4, y + 16, nullptr);
    LineTo(dc, x + 13, y + 16);
    MoveToEx(dc, x + 2, y + 1, nullptr);
    LineTo(dc, x + 15, y + 17);

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void drawMutedSpeaker(HDC dc, int x, int y) {
    const COLORREF speakerColor = RGB(239, 190, 70);
    HPEN speakerPen = CreatePen(PS_SOLID, 1, speakerColor);
    HBRUSH speakerBrush = CreateSolidBrush(speakerColor);
    const HGDIOBJ oldPen = SelectObject(dc, speakerPen);
    const HGDIOBJ oldBrush = SelectObject(dc, speakerBrush);
    POINT speaker[] = {
        {x + 2, y + 7}, {x + 6, y + 7}, {x + 11, y + 3},
        {x + 11, y + 15}, {x + 6, y + 11}, {x + 2, y + 11},
    };
    Polygon(dc, speaker, static_cast<int>(std::size(speaker)));

    HPEN slashPen = CreatePen(PS_SOLID, 2, RGB(232, 83, 83));
    SelectObject(dc, slashPen);
    MoveToEx(dc, x + 1, y + 2, nullptr);
    LineTo(dc, x + 15, y + 17);

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(slashPen);
    DeleteObject(speakerBrush);
    DeleteObject(speakerPen);
}

std::vector<ClientState> displayClients() {
    std::lock_guard<std::mutex> lock(g_modelMutex);
    auto clients = g_clients;
    for (const auto& entry : g_whisperClients) {
        const auto existing = std::find_if(clients.begin(), clients.end(), [&](const ClientState& client) {
            return client.id == entry.first;
        });
        if (existing == clients.end()) clients.push_back(entry.second);
        else {
            existing->talking = true;
            existing->whisper = true;
        }
    }
    std::stable_sort(clients.begin(), clients.end(), [](const ClientState& a, const ClientState& b) {
        if (a.whisper != b.whisper) return a.whisper > b.whisper;
        return a.talking > b.talking;
    });
    return clients;
}

RECT overlayPanelRect(const RECT& bounds, const Config& config, size_t clientCount) {
    const int rows = std::max(1, std::min(static_cast<int>(clientCount), config.maxClients));
    const int width = std::min(config.width, static_cast<int>(bounds.right));
    const int height = 36 + rows * config.rowHeight + 8 + kMoveHintHeight;
    const int left = std::clamp(static_cast<int>(bounds.right) - config.offsetX - width,
        0, std::max(0, static_cast<int>(bounds.right) - width));
    const int top = std::clamp(config.offsetY, 0, std::max(0, static_cast<int>(bounds.bottom) - height));
    return {left, top, left + width, top + height};
}

RECT largeLeaderPortraitRect(const RECT& bounds, const Config& config, size_t clientCount) {
    const auto panel = overlayPanelRect(bounds, config, clientCount);
    const int width = panel.right - panel.left;
    const int top = panel.bottom + 8;
    const int size = std::max(0, std::min({config.leaderSize, width,
        static_cast<int>(bounds.bottom) - top}));
    const int x = panel.left + (width - size) / 2;
    return {x, top, x + size, top + size};
}

RECT overlayHeaderRect(const RECT& bounds, const Config& config) {
    const auto panel = overlayPanelRect(bounds, config, displayClients().size());
    return {panel.left + 16, panel.top, panel.right - 12, panel.top + 36};
}

void drawMoveHint(HDC dc, const RECT& panel, bool moving) {
    const wchar_t* first = moving ? L"Отпустите ЛКМ — сохранить" : L"Ctrl+Alt+M";
    const wchar_t* second = moving ? L"Esc / ПКМ — отмена" : L"Переместить панель";
    const int available = std::max(1L, panel.right - panel.left - 28);
    // Keep the instructions readable at the minimum supported panel width.
    int fontSize = 13;
    HFONT font{};
    for (;;) {
        font = CreateFontW(-fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        const auto oldFont = SelectObject(dc, font);
        SIZE firstSize{}, secondSize{};
        GetTextExtentPoint32W(dc, first, static_cast<int>(wcslen(first)), &firstSize);
        GetTextExtentPoint32W(dc, second, static_cast<int>(wcslen(second)), &secondSize);
        SelectObject(dc, oldFont);
        if (std::max(firstSize.cx, secondSize.cx) <= available || fontSize <= 10) break;
        DeleteObject(font);
        --fontSize;
    }
    RECT separator{panel.left + 16, panel.bottom - kMoveHintHeight, panel.right - 12, panel.bottom - kMoveHintHeight + 1};
    HBRUSH brush = CreateSolidBrush(RGB(46, 52, 62));
    FillRect(dc, &separator, brush);
    DeleteObject(brush);
    RECT line{panel.left + 16, separator.bottom + 2, panel.right - 12, separator.bottom + 19};
    drawTextLine(dc, first, line, moving ? RGB(239, 190, 70) : RGB(155, 165, 180), font, DT_LEFT | DT_END_ELLIPSIS);
    OffsetRect(&line, 0, 17);
    drawTextLine(dc, second, line, RGB(155, 165, 180), font, DT_LEFT | DT_END_ELLIPSIS);
    DeleteObject(font);
}

void renderOverlay(HDC dc, RECT bounds, ULONGLONG tick, const SYSTEMTIME* utc = nullptr) {
    HBRUSH transparentBrush = CreateSolidBrush(kTransparentColor);
    FillRect(dc, &bounds, transparentBrush);
    DeleteObject(transparentBrush);

    Config config;
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        config = g_config;
    }
    const auto clients = displayClients();

    const int visibleCount = std::min<int>(static_cast<int>(clients.size()), config.maxClients);
    RECT panel = overlayPanelRect(bounds, config, clients.size());
    const int left = panel.left;
    const int top = panel.top;
    const int panelHeight = panel.bottom - panel.top;

    HBRUSH panelBrush = CreateSolidBrush(RGB(18, 22, 28));
    FillRect(dc, &panel, panelBrush);
    DeleteObject(panelBrush);
    RECT accent{left, top, left + 4, top + panelHeight};
    HBRUSH accentBrush = CreateSolidBrush(RGB(229, 167, 43));
    FillRect(dc, &accent, accentBrush);
    DeleteObject(accentBrush);

    HFONT headerFont = CreateFontW(-(g_moveMode ? std::min(config.fontSize, 17) : config.fontSize), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT bodyFont = CreateFontW(-config.fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    RECT header = overlayHeaderRect(bounds, config);
    if (config.clock.enabled && !g_moveMode) {
        // Cap the header clock font so it fits even on the narrowest panel.
        HFONT clockFont = CreateFontW(-std::min(config.fontSize, 20), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ previousFont = SelectObject(dc, clockFont);
        SIZE extent{};
        GetTextExtentPoint32W(dc, L"00:00:00", 8, &extent);
        SelectObject(dc, previousFont);
        RECT clockRect = header;
        clockRect.left = std::max(header.left, header.right - extent.cx - 4);
        drawTextLine(dc, utc ? formatOverlayClock(config.clock, *utc) : currentOverlayClock(config.clock),
            clockRect, RGB(190, 197, 207), clockFont, DT_RIGHT);
        header.right = std::max(header.left, clockRect.left - 10);
        DeleteObject(clockFont);
    }
    drawTextLine(dc, g_moveMode ? L"Тяните мышью" : L"GooseOverlay", header,
        RGB(229, 167, 43), headerFont, DT_LEFT | DT_END_ELLIPSIS);

    if (clients.empty()) {
        RECT row{left + 16, top + 36, panel.right - 12, top + 36 + config.rowHeight};
        drawTextLine(dc, L"No active channel", row, RGB(150, 158, 170), bodyFont, DT_LEFT | DT_END_ELLIPSIS);
    } else {
        for (int i = 0; i < visibleCount; ++i) {
            const auto& client = clients[static_cast<size_t>(i)];
            const int y = top + 36 + i * config.rowHeight;
            const COLORREF indicator = client.whisper ? RGB(239, 190, 70) : (client.talking ? RGB(76, 214, 124) : RGB(92, 101, 113));
            HBRUSH dotBrush = CreateSolidBrush(indicator);
            HGDIOBJ oldBrush = SelectObject(dc, dotBrush);
            HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, left + 16, y + (config.rowHeight - 10) / 2, left + 26, y + (config.rowHeight - 10) / 2 + 10);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
            DeleteObject(dotBrush);
            int textRight = panel.right - 12;
            const int iconY = y + std::max(1, (config.rowHeight - 18) / 2);
            if (client.soundMuted) {
                drawMutedSpeaker(dc, textRight - 17, iconY);
                textRight -= 23;
            }
            if (client.muted) {
                drawMutedMicrophone(dc, textRight - 17, iconY);
                textRight -= 23;
            }
            RECT row{left + 36, y, textRight, y + config.rowHeight};
            const std::wstring label = client.whisper ? L"[WHISPER] " + client.name : client.name;
            drawTextLine(dc, label, row, client.whisper ? RGB(239, 190, 70) :
                (client.talking ? RGB(245, 247, 250) : RGB(190, 197, 207)), bodyFont, DT_LEFT | DT_END_ELLIPSIS);
        }
    }

    drawMoveHint(dc, panel, g_moveMode);

    if (config.leaderEnabled && g_leaderPortrait) {
        const auto leader = std::find_if(clients.begin(), clients.end(), [&](const ClientState& client) {
            return client.talking && !client.muted && isLeaderName(client.name, config.leaderName);
        });
        if (leader != clients.end()) {
            const auto rect = largeLeaderPortraitRect(bounds, config, clients.size());
            if (rect.bottom > rect.top) g_leaderPortrait->draw(dc, rect.left, rect.top,
                rect.right - rect.left, leader->talking && !leader->muted, tick);
        }
    }

    DeleteObject(headerFont);
    DeleteObject(bodyFont);
}

bool renderBufferedOverlay(PaintBuffer& buffer, HDC target, RECT bounds, RECT dirty, ULONGLONG tick, const SYSTEMTIME* utc = nullptr) {
    if (!buffer.ensure(target, bounds.right, bounds.bottom)) return false;
    // Nothing touches the visible window until the entire dirty region is ready.
    // Compose with the same clip for full and partial updates: GDI+ image sampling
    // can round differently when the memory DC clip changes between frames.
    renderOverlay(buffer.dc(), bounds, tick, utc);
    return buffer.present(target, dirty);
}

void paintOverlay(HWND window) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT bounds{};
    GetClientRect(window, &bounds);
    if (g_paintBuffer) renderBufferedOverlay(*g_paintBuffer, dc, bounds, paint.rcPaint, GetTickCount64());
    EndPaint(window, &paint);
}

// Accessed only on the overlay thread; edit mode is never persisted.
struct DragState {
    bool active{};
    POINT grab{};
    int originalX{};
    int originalY{};
};
DragState g_drag;

void setMoveMode(HWND window, bool enabled) {
    if (g_drag.active) {
        g_drag.active = false;
        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            g_config.offsetX = g_drag.originalX;
            g_config.offsetY = g_drag.originalY;
        }
        if (GetCapture() == window) ReleaseCapture();
    }
    g_moveMode = enabled;
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    SetWindowLongPtrW(window, GWL_EXSTYLE, enabled ? style & ~WS_EX_TRANSPARENT : style | WS_EX_TRANSPARENT);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    SetCursor(LoadCursorW(nullptr, enabled ? IDC_SIZEALL : IDC_ARROW));
    InvalidateRect(window, nullptr, FALSE);
    if (const HWND settings = g_settingsWindow.load()) PostMessageW(settings, kMoveStateChangedMessage, 0, 0);
}

void stopMoving() {
    if (const HWND overlay = g_overlayWindow.load()) SendMessageW(overlay, kMoveModeMessage, 0, 0);
}

void toggleMoving() {
    if (const HWND overlay = g_overlayWindow.load()) PostMessageW(overlay, kMoveModeMessage, 2, 0);
}

bool moveShortcutDown() {
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
        (GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState('M') & 0x8000) &&
        !(GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
        !(GetAsyncKeyState(VK_LWIN) & 0x8000) && !(GetAsyncKeyState(VK_RWIN) & 0x8000);
}

struct MoveShortcut {
    bool wasDown{};
    bool sample(bool down, bool available) {
        const bool pressed = down && !wasDown;
        wasDown = down;
        return pressed && available;
    }
};

void updateDragPosition(HWND window, POINT cursor) {
    RECT bounds{};
    GetClientRect(window, &bounds);
    const auto count = displayClients().size();
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        const auto panel = overlayPanelRect(bounds, g_config, count);
        const int maxX = std::max(0L, bounds.right - (panel.right - panel.left));
        const int maxY = std::max(0L, bounds.bottom - (panel.bottom - panel.top));
        const int x = std::clamp(static_cast<int>(cursor.x - g_drag.grab.x), 0, maxX);
        const int y = std::clamp(static_cast<int>(cursor.y - g_drag.grab.y), 0, maxY);
        g_config.offsetX = std::min(32767, maxX - x);
        g_config.offsetY = std::min(32767, y);
    }
    InvalidateRect(window, nullptr, FALSE);
}

bool persistPosition(int x, int y) {
    return WritePrivateProfileStringW(L"overlay", L"offset_x", std::to_wstring(x).c_str(), g_configPath.c_str()) &&
        WritePrivateProfileStringW(L"overlay", L"offset_y", std::to_wstring(y).c_str(), g_configPath.c_str());
}

void finishDragging(HWND window) {
    bool saved;
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        saved = persistPosition(g_config.offsetX, g_config.offsetY);
        if (!saved) {
            persistPosition(g_drag.originalX, g_drag.originalY);
            g_config.offsetX = g_drag.originalX;
            g_config.offsetY = g_drag.originalY;
        }
    }
    g_drag.active = false;
    if (GetCapture() == window) ReleaseCapture();
    setMoveMode(window, false);
    if (const HWND settings = g_settingsWindow.load()) PostMessageW(settings, kPositionChangedMessage, 0, 0);
    if (!saved) {
        logMessage("Unable to save overlay position", LogLevel_ERROR);
        printMessage("GooseOverlay: unable to save position; previous position restored.");
    }
}

LRESULT CALLBACK overlayWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paintOverlay(window); return 0;
        case kMoveModeMessage:
            setMoveMode(window, g_enabled && (wParam == 2 ? !g_moveMode.load() : wParam != 0));
            return 0;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_NCHITTEST: return g_moveMode ? HTCLIENT : HTTRANSPARENT;
        case WM_SETCURSOR:
            if (g_moveMode) { SetCursor(LoadCursorW(nullptr, IDC_SIZEALL)); return TRUE; }
            break;
        case WM_LBUTTONDOWN:
            if (g_moveMode) {
                const POINT cursor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                RECT bounds{};
                GetClientRect(window, &bounds);
                const auto count = displayClients().size();
                {
                    std::lock_guard<std::mutex> lock(g_configMutex);
                    const auto panel = overlayPanelRect(bounds, g_config, count);
                    if (!PtInRect(&panel, cursor)) return 0;
                    g_drag = {true, {cursor.x - panel.left, cursor.y - panel.top}, g_config.offsetX, g_config.offsetY};
                }
                SetCapture(window);
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
            if (g_drag.active) {
                updateDragPosition(window, {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (g_drag.active) {
                updateDragPosition(window, {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
                finishDragging(window);
                return 0;
            }
            break;
        case WM_RBUTTONDOWN:
        case WM_CANCELMODE:
            setMoveMode(window, false);
            return 0;
        case WM_CAPTURECHANGED:
            if (g_drag.active) setMoveMode(window, false);
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void overlayMain() {
    PaintBuffer paintBuffer;
    g_paintBuffer = &paintBuffer;
    LeaderPortrait portrait(g_module);
    g_leaderPortrait = &portrait;
    if (!portrait.ready()) logMessage("Unable to load leader portrait frames", LogLevel_WARNING);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = g_module;
    windowClass.lpfnWndProc = overlayWindowProc;
    windowClass.lpszClassName = kOverlayClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&windowClass);

    HWND overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kOverlayClass, L"GooseOverlay", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_module, nullptr);
    if (!overlay) {
        g_paintBuffer = nullptr;
        g_leaderPortrait = nullptr;
        logMessage("Unable to create overlay window", LogLevel_ERROR);
        return;
    }
    g_overlayWindow = overlay;

    MSG message{};
    DWORD lastSearch{};
    int appliedOpacity = -1;
    ULONGLONG animationFrame = GetTickCount64() / 200;
    std::wstring lastClockText;
    MoveShortcut moveShortcut{moveShortcutDown()};
    while (!g_stop) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        Config config;
        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            config = g_config;
        }
        if (appliedOpacity != config.opacity &&
            SetLayeredWindowAttributes(overlay, kTransparentColor, static_cast<BYTE>(config.opacity), LWA_COLORKEY | LWA_ALPHA)) {
            appliedOpacity = config.opacity;
        }

        HWND game = g_gameWindow.load();
        const DWORD now = GetTickCount();
        if (!IsWindow(game) || now - lastSearch > 2000) {
            game = findGameWindow(config);
            g_gameWindow = game;
            lastSearch = now;
        }

        bool shouldShow = g_enabled && IsWindow(game) && !IsIconic(game);
        const bool gameFocused = GetAncestor(GetForegroundWindow(), GA_ROOT) == game;
        if (moveShortcut.sample(moveShortcutDown(), shouldShow && gameFocused)) {
            setMoveMode(overlay, !g_moveMode.load());
        }
        if (g_moveMode && (!g_enabled || (g_drag.active && (!shouldShow || !gameFocused)) ||
            (gameFocused && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)))) {
            setMoveMode(overlay, false);
        }
        if (shouldShow && config.hideWhenUnfocused) {
            shouldShow = gameFocused;
        }
        RECT gameRect{};
        if (shouldShow && getClientAreaOnScreen(game, gameRect)) {
            RECT currentRect{};
            GetWindowRect(overlay, &currentRect);
            if (!IsWindowVisible(overlay) || !EqualRect(&currentRect, &gameRect)) {
                SetWindowPos(overlay, HWND_TOPMOST, gameRect.left, gameRect.top,
                    gameRect.right - gameRect.left, gameRect.bottom - gameRect.top,
                    SWP_NOACTIVATE | SWP_SHOWWINDOW);
                InvalidateRect(overlay, nullptr, FALSE);
            }
            if (g_drag.active) {
                // A non-activating window may miss mouse messages outside its
                // visible area. Finish the drag even if button-up is missed.
                POINT cursor{};
                if (GetCursorPos(&cursor) && ScreenToClient(overlay, &cursor)) {
                    updateDragPosition(overlay, cursor);
                    if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) finishDragging(overlay);
                } else setMoveMode(overlay, false);
            }
            const ULONGLONG frame = GetTickCount64() / 200;
            const auto clockText = currentOverlayClock(config.clock);
            if (clockText != lastClockText) {
                RECT bounds{};
                GetClientRect(overlay, &bounds);
                const auto dirty = overlayHeaderRect(bounds, config);
                InvalidateRect(overlay, &dirty, FALSE);
                lastClockText = clockText;
            }
            if (frame != animationFrame && config.leaderEnabled && portrait.ready()) {
                RECT bounds{};
                GetClientRect(overlay, &bounds);
                const auto clients = displayClients();
                const int count = static_cast<int>(clients.size());
                for (int i = 0; i < count; ++i) {
                    const auto& client = clients[static_cast<size_t>(i)];
                    if (client.talking && !client.muted && isLeaderName(client.name, config.leaderName)) {
                        const auto dirty = largeLeaderPortraitRect(bounds, config, clients.size());
                        if (dirty.bottom > dirty.top) InvalidateRect(overlay, &dirty, FALSE);
                        break;
                    }
                }
            }
            animationFrame = frame;
        } else {
            if (IsWindowVisible(overlay)) ShowWindow(overlay, SW_HIDE);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, (g_drag.active || (shouldShow && gameFocused)) ? 16 : 100, QS_ALLINPUT);
    }
    setMoveMode(overlay, false);
    g_overlayWindow = nullptr;
    DestroyWindow(overlay);
    UnregisterClassW(kOverlayClass, g_module);
    g_leaderPortrait = nullptr;
    g_paintBuffer = nullptr;
}

void updateTalkState(uint64 server, anyID clientId, bool talking, bool whisper) {
    if (server != g_server.load()) return;
    ClientState incoming;
    if (talking && whisper) {
        incoming.id = clientId;
        incoming.talking = true;
        incoming.whisper = true;
        char name[512]{};
        if (g_ts3.getClientDisplayName &&
            g_ts3.getClientDisplayName(server, clientId, name, sizeof(name)) == ERROR_ok) {
            incoming.name = utf8ToWide(name);
        }
        if (incoming.name.empty()) incoming.name = L"Client #" + std::to_wstring(clientId);
    }
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_modelMutex);
        if (server != g_server.load()) return;
        if (talking && whisper) g_whisperClients[clientId] = std::move(incoming);
        else g_whisperClients.erase(clientId);
        g_talkState[clientId] = {talking, talking && whisper};
        for (auto& client : g_clients) {
            if (client.id == clientId) {
                client.talking = talking;
                client.whisper = talking && whisper;
                found = true;
                break;
            }
        }
        std::stable_sort(g_clients.begin(), g_clients.end(), [](const ClientState& a, const ClientState& b) {
            return a.talking > b.talking;
        });
    }
    if (!found && talking && !whisper) refreshClients(server);
    invalidateOverlay();
}

void refreshCurrentServer(uint64 server) {
    if (g_server.exchange(server) != server) clearClients();
    refreshClients(server);
}

enum SettingsControlId {
    IDC_SETTINGS_ENABLED = 1001,
    IDC_SETTINGS_HIDE_UNFOCUSED,
    IDC_SETTINGS_PROCESSES = 1010,
    IDC_SETTINGS_TITLE,
    IDC_SETTINGS_LEADER_NAME,
    IDC_SETTINGS_LEADER_ENABLED,
    IDC_SETTINGS_LEADER_SIZE,
    IDC_SETTINGS_CLOCK_ENABLED,
    IDC_SETTINGS_CLOCK_ZONE,
    IDC_SETTINGS_OPACITY = 1020,
    IDC_SETTINGS_WIDTH,
    IDC_SETTINGS_OFFSET_X,
    IDC_SETTINGS_OFFSET_Y,
    IDC_SETTINGS_ROW_HEIGHT,
    IDC_SETTINGS_FONT_SIZE,
    IDC_SETTINGS_MAX_CLIENTS,
    IDC_SETTINGS_APPLY = 1030,
    IDC_SETTINGS_MOVE,
    IDC_SETTINGS_RESET_POSITION,
};

struct SettingsState {
    Config config;
    bool enabled{};
    HFONT font{};
    std::vector<DYNAMIC_TIME_ZONE_INFORMATION> clockZones;
};

void setControlFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND createSettingsControl(HWND parent, DWORD exStyle, const wchar_t* className, const wchar_t* text,
    DWORD style, int x, int y, int width, int height, int id, HFONT font) {
    HWND control = CreateWindowExW(exStyle, className, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_module, nullptr);
    if (control) setControlFont(control, font);
    return control;
}

void createSettingsLabel(HWND parent, const wchar_t* text, int x, int y, int width, HFONT font) {
    createSettingsControl(parent, 0, L"STATIC", text, SS_LEFT, x, y, width, 22, 0, font);
}

void createSettingsEdit(HWND parent, int id, const std::wstring& value, int x, int y, int width, bool numeric, HFONT font) {
    createSettingsControl(parent, WS_EX_CLIENTEDGE, L"EDIT", value.c_str(),
        WS_TABSTOP | ES_AUTOHSCROLL | (numeric ? ES_NUMBER : 0), x, y, width, 25, id, font);
}

std::wstring settingsControlText(HWND window, int id) {
    const HWND control = GetDlgItem(window, id);
    const int length = GetWindowTextLengthW(control);
    std::wstring result(static_cast<size_t>(length + 1), L'\0');
    GetWindowTextW(control, result.data(), length + 1);
    result.resize(static_cast<size_t>(length));
    return trim(std::move(result));
}

bool readSettingsNumber(HWND window, int id, const wchar_t* label, int minValue, int maxValue, int& result) {
    const std::wstring text = settingsControlText(window, id);
    wchar_t* end{};
    const long value = wcstol(text.c_str(), &end, 10);
    if (text.empty() || !end || *end != L'\0' || value < minValue || value > maxValue) {
        std::wstringstream message;
        message << label << L": введите число от " << minValue << L" до " << maxValue << L".";
        MessageBoxW(window, message.str().c_str(), L"GooseOverlay", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(window, id));
        return false;
    }
    result = static_cast<int>(value);
    return true;
}

bool applySettings(HWND window) {
    auto* state = reinterpret_cast<SettingsState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!state) return false;
    stopMoving();

    Config next = state->config;
    next.clock.enabled = SendDlgItemMessageW(window, IDC_SETTINGS_CLOCK_ENABLED, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const LRESULT zoneIndex = SendDlgItemMessageW(window, IDC_SETTINGS_CLOCK_ZONE, CB_GETCURSEL, 0, 0);
    if (zoneIndex < 0 || static_cast<size_t>(zoneIndex) > state->clockZones.size()) return false;
    next.clock.zoneId.clear();
    next.clock.zone = {};
    if (zoneIndex > 0) {
        next.clock.zone = state->clockZones[static_cast<size_t>(zoneIndex - 1)];
        next.clock.zoneId = next.clock.zone.TimeZoneKeyName;
    }
    next.leaderName = settingsControlText(window, IDC_SETTINGS_LEADER_NAME);
    if (next.leaderName.empty() || next.leaderName.size() > 128) {
        MessageBoxW(window, L"Укажите ник клан-лидера (от 1 до 128 символов).", L"GooseOverlay", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(window, IDC_SETTINGS_LEADER_NAME));
        return false;
    }
    next.processes = splitProcesses(settingsControlText(window, IDC_SETTINGS_PROCESSES));
    next.titleContains = lower(settingsControlText(window, IDC_SETTINGS_TITLE));
    if (next.processes.empty() && next.titleContains.empty()) {
        MessageBoxW(window, L"Укажите хотя бы имя процесса или часть заголовка окна игры.", L"GooseOverlay", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(window, IDC_SETTINGS_PROCESSES));
        return false;
    }
    next.hideWhenUnfocused = SendDlgItemMessageW(window, IDC_SETTINGS_HIDE_UNFOCUSED, BM_GETCHECK, 0, 0) == BST_CHECKED;
    next.leaderEnabled = SendDlgItemMessageW(window, IDC_SETTINGS_LEADER_ENABLED, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!readSettingsNumber(window, IDC_SETTINGS_LEADER_SIZE, L"Размер большого аватара", 64, 256, next.leaderSize)) return false;
    if (!readSettingsNumber(window, IDC_SETTINGS_OPACITY, L"Непрозрачность", 40, 255, next.opacity) ||
        !readSettingsNumber(window, IDC_SETTINGS_WIDTH, L"Ширина панели", 180, 800, next.width) ||
        !readSettingsNumber(window, IDC_SETTINGS_OFFSET_X, L"Отступ справа", 0, 32767, next.offsetX) ||
        !readSettingsNumber(window, IDC_SETTINGS_OFFSET_Y, L"Отступ сверху", 0, 32767, next.offsetY) ||
        !readSettingsNumber(window, IDC_SETTINGS_ROW_HEIGHT, L"Высота строки", 20, 80, next.rowHeight) ||
        !readSettingsNumber(window, IDC_SETTINGS_FONT_SIZE, L"Размер шрифта", 10, 44, next.fontSize) ||
        !readSettingsNumber(window, IDC_SETTINGS_MAX_CLIENTS, L"Максимум пользователей", 1, 64, next.maxClients)) {
        return false;
    }

    const bool enabled = SendDlgItemMessageW(window, IDC_SETTINGS_ENABLED, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!saveConfig(next, enabled)) {
        MessageBoxW(window, L"Не удалось сохранить настройки. Проверьте доступ к файлу конфигурации TeamSpeak.", L"GooseOverlay", MB_OK | MB_ICONERROR);
        return false;
    }
    state->config = next;
    state->enabled = enabled;
    return true;
}

LRESULT CALLBACK settingsWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<SettingsState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<SettingsState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
        case WM_CREATE: {
            state->font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HFONT font = state->font;
            HWND enabled = createSettingsControl(window, 0, L"BUTTON", L"Оверлей включён", BS_AUTOCHECKBOX | WS_TABSTOP,
                18, 16, 220, 24, IDC_SETTINGS_ENABLED, font);
            SendMessageW(enabled, BM_SETCHECK, state->enabled ? BST_CHECKED : BST_UNCHECKED, 0);
            HWND hide = createSettingsControl(window, 0, L"BUTTON", L"Скрывать, когда игра неактивна", BS_AUTOCHECKBOX | WS_TABSTOP,
                260, 16, 320, 24, IDC_SETTINGS_HIDE_UNFOCUSED, font);
            SendMessageW(hide, BM_SETCHECK, state->config.hideWhenUnfocused ? BST_CHECKED : BST_UNCHECKED, 0);

            createSettingsLabel(window, L"Имена процессов игры через запятую", 18, 56, 560, font);
            createSettingsEdit(window, IDC_SETTINGS_PROCESSES, joinProcesses(state->config.processes), 18, 78, 584, false, font);
            createSettingsLabel(window, L"Часть заголовка окна (резервный способ поиска)", 18, 114, 560, font);
            createSettingsEdit(window, IDC_SETTINGS_TITLE, state->config.titleContains, 18, 136, 584, false, font);

            createSettingsLabel(window, L"Непрозрачность (40–255)", 18, 184, 210, font);
            createSettingsEdit(window, IDC_SETTINGS_OPACITY, std::to_wstring(state->config.opacity), 218, 180, 80, true, font);
            createSettingsLabel(window, L"Ширина панели (180–800)", 322, 184, 200, font);
            createSettingsEdit(window, IDC_SETTINGS_WIDTH, std::to_wstring(state->config.width), 522, 180, 80, true, font);

            createSettingsLabel(window, L"Отступ справа (px)", 18, 226, 190, font);
            createSettingsEdit(window, IDC_SETTINGS_OFFSET_X, std::to_wstring(state->config.offsetX), 218, 222, 80, true, font);
            createSettingsLabel(window, L"Отступ сверху (px)", 322, 226, 190, font);
            createSettingsEdit(window, IDC_SETTINGS_OFFSET_Y, std::to_wstring(state->config.offsetY), 522, 222, 80, true, font);

            createSettingsLabel(window, L"Размер шрифта (10–44)", 18, 268, 190, font);
            createSettingsEdit(window, IDC_SETTINGS_FONT_SIZE, std::to_wstring(state->config.fontSize), 218, 264, 80, true, font);
            createSettingsLabel(window, L"Высота строки (20–80)", 322, 268, 190, font);
            createSettingsEdit(window, IDC_SETTINGS_ROW_HEIGHT, std::to_wstring(state->config.rowHeight), 522, 264, 80, true, font);

            createSettingsLabel(window, L"Максимум пользователей (1–64)", 18, 310, 230, font);
            createSettingsEdit(window, IDC_SETTINGS_MAX_CLIENTS, std::to_wstring(state->config.maxClients), 258, 306, 80, true, font);
            createSettingsControl(window, 0, L"BUTTON", g_moveMode ? L"Отменить перемещение" : L"Переместить мышью",
                BS_PUSHBUTTON | WS_TABSTOP, 18, 345, 280, 30, IDC_SETTINGS_MOVE, font);
            createSettingsControl(window, 0, L"BUTTON", L"Сбросить положение",
                BS_PUSHBUTTON | WS_TABSTOP, 322, 345, 280, 30, IDC_SETTINGS_RESET_POSITION, font);

            createSettingsLabel(window, L"Ник клан-лидера", 18, 393, 190, font);
            createSettingsEdit(window, IDC_SETTINGS_LEADER_NAME, state->config.leaderName, 218, 389, 384, false, font);
            SendDlgItemMessageW(window, IDC_SETTINGS_LEADER_NAME, EM_SETLIMITTEXT, 128, 0);
            HWND avatarEnabled = createSettingsControl(window, 0, L"BUTTON", L"Показывать аватар под оверлеем во время речи лидера",
                BS_AUTOCHECKBOX | WS_TABSTOP, 18, 424, 584, 24, IDC_SETTINGS_LEADER_ENABLED, font);
            SendMessageW(avatarEnabled, BM_SETCHECK, state->config.leaderEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
            createSettingsLabel(window, L"Размер большого аватара (64–256 px)", 18, 461, 300, font);
            createSettingsEdit(window, IDC_SETTINGS_LEADER_SIZE, std::to_wstring(state->config.leaderSize), 330, 457, 80, true, font);

            HWND clockEnabled = createSettingsControl(window, 0, L"BUTTON", L"Показывать время в заголовке (ЧЧ:ММ:СС)",
                BS_AUTOCHECKBOX | WS_TABSTOP, 18, 493, 584, 24, IDC_SETTINGS_CLOCK_ENABLED, font);
            SendMessageW(clockEnabled, BM_SETCHECK, state->config.clock.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
            createSettingsLabel(window, L"Часовой пояс", 18, 533, 140, font);
            HWND zones = createSettingsControl(window, 0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                158, 529, 444, 250, IDC_SETTINGS_CLOCK_ZONE, font);
            SendMessageW(zones, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Время компьютера"));
            state->clockZones = clockTimeZones();
            size_t selectedZone = 0;
            for (size_t i = 0; i < state->clockZones.size(); ++i) {
                const auto& zone = state->clockZones[i];
                const auto label = clockZoneLabel(zone);
                SendMessageW(zones, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
                if (state->config.clock.zoneId == zone.TimeZoneKeyName) selectedZone = i + 1;
            }
            SendMessageW(zones, CB_SETCURSEL, selectedZone, 0);
            createSettingsLabel(window, L"Сезонный перевод времени учитывается по правилам Windows.", 18, 567, 584, font);

            createSettingsControl(window, 0, L"BUTTON", L"Сохранить", BS_DEFPUSHBUTTON | WS_TABSTOP,
                310, 610, 92, 30, IDOK, font);
            createSettingsControl(window, 0, L"BUTTON", L"Применить", BS_PUSHBUTTON | WS_TABSTOP,
                410, 610, 92, 30, IDC_SETTINGS_APPLY, font);
            createSettingsControl(window, 0, L"BUTTON", L"Отмена", BS_PUSHBUTTON | WS_TABSTOP,
                510, 610, 92, 30, IDCANCEL, font);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_SETTINGS_MOVE:
                    if (!g_enabled || !g_overlayWindow.load()) {
                        MessageBoxW(window, L"Сначала включите оверлей и нажмите «Применить».", L"GooseOverlay", MB_OK | MB_ICONINFORMATION);
                    } else toggleMoving();
                    return 0;
                case IDC_SETTINGS_RESET_POSITION: {
                    stopMoving();
                    // Discard a queued drag notification before editing the fields.
                    MSG pending{};
                    while (PeekMessageW(&pending, window, kPositionChangedMessage, kPositionChangedMessage, PM_REMOVE)) {}
                    const Config defaults;
                    SetDlgItemInt(window, IDC_SETTINGS_OFFSET_X, defaults.offsetX, FALSE);
                    SetDlgItemInt(window, IDC_SETTINGS_OFFSET_Y, defaults.offsetY, FALSE);
                    return 0;
                }
                case IDOK:
                    if (applySettings(window)) DestroyWindow(window);
                    return 0;
                case IDC_SETTINGS_APPLY:
                    if (applySettings(window)) MessageBoxW(window, L"Настройки применены.", L"GooseOverlay", MB_OK | MB_ICONINFORMATION);
                    return 0;
                case IDCANCEL:
                    DestroyWindow(window);
                    return 0;
                default:
                    break;
            }
            break;
        case kPositionChangedMessage: {
            {
                std::lock_guard<std::mutex> lock(g_configMutex);
                state->config.offsetX = g_config.offsetX;
                state->config.offsetY = g_config.offsetY;
            }
            SetDlgItemInt(window, IDC_SETTINGS_OFFSET_X, state->config.offsetX, FALSE);
            SetDlgItemInt(window, IDC_SETTINGS_OFFSET_Y, state->config.offsetY, FALSE);
            return 0;
        }
        case kMoveStateChangedMessage:
            SetDlgItemTextW(window, IDC_SETTINGS_MOVE, g_moveMode ? L"Отменить перемещение" : L"Переместить мышью");
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            g_settingsWindow = nullptr;
            if (state) {
                if (state->font) DeleteObject(state->font);
                delete state;
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            }
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void showSettingsWindow(HWND owner) {
    if (g_settingsOpen.exchange(true)) return;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = g_module;
    windowClass.lpfnWndProc = settingsWindowProc;
    windowClass.lpszClassName = kSettingsClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    RegisterClassExW(&windowClass);

    auto* state = new SettingsState;
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        state->config = g_config;
    }
    state->enabled = g_enabled;

    constexpr int width = 638;
    constexpr int height = 698;
    RECT position{};
    if (!owner || !IsWindow(owner) || !GetWindowRect(owner, &position)) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &position, 0);
    }
    const int x = position.left + std::max(0, (static_cast<int>(position.right - position.left) - width) / 2);
    const int y = position.top + std::max(0, (static_cast<int>(position.bottom - position.top) - height) / 2);
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, kSettingsClass, L"Настройки GooseOverlay",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, width, height,
        IsWindow(owner) ? owner : nullptr, nullptr, g_module, state);
    if (!window) {
        delete state;
        g_settingsOpen = false;
        return;
    }
    g_settingsWindow = window;

    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    UnregisterClassW(kSettingsClass, g_module);
    g_settingsOpen = false;
}

} // namespace

#define TS3_EXPORT extern "C" __declspec(dllexport)

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

TS3_EXPORT const char* ts3plugin_name() { return "GooseOverlay"; }
TS3_EXPORT const char* ts3plugin_version() { return "0.13.0"; }
TS3_EXPORT int ts3plugin_apiVersion() { return kPluginApiVersion; }
TS3_EXPORT const char* ts3plugin_author() { return "GooseOverlay contributors"; }
TS3_EXPORT const char* ts3plugin_description() { return "TeamSpeak speaking overlay for Battles for Glory (borderless/windowed mode)."; }
TS3_EXPORT void ts3plugin_setFunctionPointers(const TS3Functions functions) { g_ts3 = functions; }

TS3_EXPORT int ts3plugin_init() {
    char configPath[4096]{};
    if (g_ts3.getConfigPath) g_ts3.getConfigPath(configPath, std::size(configPath));
    std::wstring directory = utf8ToWide(configPath);
    if (directory.empty()) {
        wchar_t appData[4096]{};
        const DWORD length = GetEnvironmentVariableW(L"APPDATA", appData, static_cast<DWORD>(std::size(appData)));
        if (length > 0 && length < std::size(appData)) directory = std::wstring(appData) + L"\\TS3Client\\";
    } else if (directory.back() != L'\\' && directory.back() != L'/') {
        directory.push_back(L'\\');
    }
    g_configPath = directory + L"goose_overlay.ini";
    const std::wstring legacyConfigPath = directory + L"bfg_overlay.ini";
    if (GetFileAttributesW(g_configPath.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(legacyConfigPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        CopyFileW(legacyConfigPath.c_str(), g_configPath.c_str(), TRUE);
    }
    loadConfig();
    g_stop = false;
    g_overlayThread = std::thread(overlayMain);
    if (g_ts3.getCurrentServerConnectionHandlerID) refreshCurrentServer(g_ts3.getCurrentServerConnectionHandlerID());
    logMessage("GooseOverlay initialized");
    return 0;
}

TS3_EXPORT void ts3plugin_shutdown() {
    for (int i = 0; i < 200 && g_settingsOpen; ++i) {
        const HWND settings = g_settingsWindow.load();
        if (settings) PostMessageW(settings, WM_CLOSE, 0, 0);
        Sleep(10);
    }
    g_stop = true;
    const HWND overlay = g_overlayWindow.load();
    if (overlay) PostMessageW(overlay, WM_CLOSE, 0, 0);
    if (g_overlayThread.joinable()) g_overlayThread.join();
    clearClients();
    g_pluginId.clear();
}

TS3_EXPORT void ts3plugin_registerPluginID(const char* id) { g_pluginId = id ? id : ""; }
TS3_EXPORT void ts3plugin_freeMemory(void* data) { std::free(data); }
TS3_EXPORT const char* ts3plugin_commandKeyword() { return "goose"; }
TS3_EXPORT int ts3plugin_offersConfigure() { return PLUGIN_OFFERS_CONFIGURE_NEW_THREAD; }
TS3_EXPORT void ts3plugin_configure(void* handle, void*) { showSettingsWindow(static_cast<HWND>(handle)); }

TS3_EXPORT int ts3plugin_processCommand(uint64, const char* command) {
    std::istringstream input(command ? command : "");
    std::string action;
    input >> action;
    std::transform(action.begin(), action.end(), action.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (action == "toggle") {
        g_enabled = !g_enabled.load();
        persistEnabled();
        printMessage(std::string("GooseOverlay: ") + (g_enabled ? "enabled" : "disabled"));
    } else if (action == "show") {
        g_enabled = true;
        persistEnabled();
        printMessage("GooseOverlay: enabled");
    } else if (action == "hide") {
        g_enabled = false;
        persistEnabled();
        printMessage("GooseOverlay: disabled");
    } else if (action == "move") {
        toggleMoving();
        printMessage("GooseOverlay: move mode toggled. Drag the panel in game; release to save. Escape or right click cancels.");
    } else if (action == "opacity") {
        int opacity{};
        if (input >> opacity) {
            opacity = std::clamp(opacity, 40, 255);
            {
                std::lock_guard<std::mutex> lock(g_configMutex);
                g_config.opacity = opacity;
            }
            WritePrivateProfileStringW(L"overlay", L"opacity", std::to_wstring(opacity).c_str(), g_configPath.c_str());
            printMessage("GooseOverlay: opacity updated");
        } else {
            printMessage("Usage: /goose opacity 40..255");
        }
    } else if (action == "reload") {
        stopMoving();
        loadConfig();
        g_gameWindow = nullptr;
        invalidateOverlay();
        printMessage("GooseOverlay: configuration reloaded");
    } else if (action == "status") {
        printMessage(std::string("GooseOverlay: ") + (g_enabled ? "enabled" : "disabled") +
            (IsWindow(g_gameWindow.load()) ? ", game detected" : ", waiting for game"));
    } else {
        printMessage("GooseOverlay commands: /goose toggle | show | hide | move | opacity 40..255 | reload | status");
    }
    return 0;
}

TS3_EXPORT void ts3plugin_initHotkeys(PluginHotkey*** hotkeys) {
    *hotkeys = static_cast<PluginHotkey**>(std::malloc(sizeof(PluginHotkey*) * 3));
    (*hotkeys)[0] = static_cast<PluginHotkey*>(std::calloc(1, sizeof(PluginHotkey)));
    strcpy_s((*hotkeys)[0]->keyword, PLUGIN_HOTKEY_BUFSZ, "goose_overlay_toggle");
    strcpy_s((*hotkeys)[0]->description, PLUGIN_HOTKEY_BUFSZ, "Toggle GooseOverlay");
    (*hotkeys)[1] = static_cast<PluginHotkey*>(std::calloc(1, sizeof(PluginHotkey)));
    strcpy_s((*hotkeys)[1]->keyword, PLUGIN_HOTKEY_BUFSZ, "goose_overlay_move");
    strcpy_s((*hotkeys)[1]->description, PLUGIN_HOTKEY_BUFSZ, "Move GooseOverlay with mouse");
    (*hotkeys)[2] = nullptr;
}

TS3_EXPORT void ts3plugin_onHotkeyEvent(const char* keyword) {
    if (keyword && std::strcmp(keyword, "goose_overlay_toggle") == 0) {
        g_enabled = !g_enabled.load();
        persistEnabled();
    } else if (keyword && std::strcmp(keyword, "goose_overlay_move") == 0 && !moveShortcutDown()) {
        toggleMoving();
    }
}

TS3_EXPORT void ts3plugin_currentServerConnectionChanged(uint64 server) { refreshCurrentServer(server); }

TS3_EXPORT void ts3plugin_onConnectStatusChangeEvent(uint64 server, int status, unsigned int) {
    if (status == STATUS_CONNECTION_ESTABLISHED) refreshCurrentServer(server);
    else if (server == g_server.load() && status == STATUS_DISCONNECTED) clearClients();
}

TS3_EXPORT void ts3plugin_onTalkStatusChangeEvent(uint64 server, int status, int isWhisper, anyID clientId) {
    updateTalkState(server, clientId, status == STATUS_TALKING, isWhisper != 0);
}

TS3_EXPORT void ts3plugin_onUpdateClientEvent(uint64 server, anyID, anyID, const char*, const char*) {
    if (server == g_server.load()) refreshClients(server);
}

TS3_EXPORT void ts3plugin_onClientMoveEvent(uint64 server, anyID, uint64, uint64, int, const char*) {
    if (server == g_server.load()) refreshClients(server);
}

TS3_EXPORT void ts3plugin_onClientMoveSubscriptionEvent(uint64 server, anyID, uint64, uint64, int) {
    if (server == g_server.load()) refreshClients(server);
}

TS3_EXPORT void ts3plugin_onClientMoveTimeoutEvent(uint64 server, anyID, uint64, uint64, int, const char*) {
    if (server == g_server.load()) refreshClients(server);
}

TS3_EXPORT void ts3plugin_onClientMoveMovedEvent(uint64 server, anyID, uint64, uint64, int, anyID, const char*, const char*, const char*) {
    if (server == g_server.load()) refreshClients(server);
}

TS3_EXPORT void ts3plugin_onClientKickFromChannelEvent(uint64 server, anyID, uint64, uint64, int, anyID, const char*, const char*, const char*) {
    if (server == g_server.load()) refreshClients(server);
}

TS3_EXPORT void ts3plugin_onClientKickFromServerEvent(uint64 server, anyID, uint64, uint64, int, anyID, const char*, const char*, const char*) {
    if (server == g_server.load()) refreshClients(server);
}
