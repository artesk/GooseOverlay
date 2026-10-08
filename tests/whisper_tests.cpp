// Standalone state test: real plugin logic with an in-memory TS3 function table.
#include "../src/plugin.cpp"
#include <cassert>
#include <iostream>

void testClock() {
    OverlayClock clock;
    assert(clock.enabled && clock.zoneId == L"Russian Standard Time");
    SYSTEMTIME utc{2026, 1, 0, 1, 22, 30, 59, 0};
    assert(formatOverlayClock(clock, utc) == L"01:30:59"); // Next calendar day.
    assert(setClockTimeZone(clock, L"UTC"));
    assert(formatOverlayClock(clock, utc) == L"22:30:59");
    assert(setClockTimeZone(clock, L"Nepal Standard Time"));
    assert(formatOverlayClock(clock, utc) == L"04:15:59"); // Fractional UTC offset.
    assert(setClockTimeZone(clock, L"Pacific Standard Time"));
    assert(formatOverlayClock(clock, utc) == L"14:30:59");
    utc.wMonth = 7;
    assert(formatOverlayClock(clock, utc) == L"15:30:59"); // Summer time.
    utc = {2026, 3, 0, 8, 9, 59, 59, 0};
    assert(formatOverlayClock(clock, utc) == L"01:59:59");
    utc.wHour = 10; utc.wMinute = 0; utc.wSecond = 0;
    assert(formatOverlayClock(clock, utc) == L"03:00:00"); // DST transition.
    assert(!setClockTimeZone(clock, L"Nonexistent test time zone"));
    assert(clock.zoneId.empty());
    SYSTEMTIME expected{};
    DYNAMIC_TIME_ZONE_INFORMATION computer{};
    assert(GetDynamicTimeZoneInformation(&computer) != TIME_ZONE_ID_INVALID);
    assert(SystemTimeToTzSpecificLocalTimeEx(&computer, &utc, &expected));
    wchar_t text[16]{};
    swprintf_s(text, L"%02u:%02u:%02u", static_cast<unsigned>(expected.wHour),
        static_cast<unsigned>(expected.wMinute), static_cast<unsigned>(expected.wSecond));
    assert(formatOverlayClock(clock, utc) == text);
    clock.enabled = false;
    assert(formatOverlayClock(clock, utc).empty());
}

void testClockRendering() {
    g_config = Config{};
    g_config.offsetX = g_config.offsetY = 0;
    g_config.leaderEnabled = false;
    g_clients = {{1, L"восток", true, false, false, false}};
    g_whisperClients.clear();
    SYSTEMTIME first{2026, 9, 0, 10, 12, 34, 59, 0};
    SYSTEMTIME second{2026, 9, 0, 10, 12, 35, 0, 0};
    const RECT bounds{0, 0, 310, 100};
    HDC screen = GetDC(nullptr);
    assert(screen);
    PaintBuffer partial, complete, before;
    assert(partial.ensure(screen, 310, 100) && complete.ensure(screen, 310, 100) && before.ensure(screen, 310, 100));
    renderOverlay(partial.dc(), bounds, 0, &first);
    renderOverlay(before.dc(), bounds, 0, &first);
    renderOverlay(complete.dc(), bounds, 0, &second);
    PaintBuffer staging;
    const RECT header = overlayHeaderRect(bounds, g_config);
    assert(renderBufferedOverlay(staging, partial.dc(), bounds, header, 0, &second));
    int changed = 0;
    for (int y = 0; y < 100; ++y) {
        for (int x = 0; x < 310; ++x) {
            const COLORREF pixel = GetPixel(partial.dc(), x, y);
            assert(pixel == GetPixel(complete.dc(), x, y));
            if (pixel != GetPixel(before.dc(), x, y)) {
                ++changed;
                assert(x >= header.left && x < header.right && y >= header.top && y < header.bottom);
            }
        }
    }
    assert(changed > 0);
    // Disabling the clock must erase all old digits and restore title space.
    g_config.clock.enabled = false;
    assert(renderBufferedOverlay(staging, partial.dc(), bounds, header, 0, &second));
    renderOverlay(complete.dc(), bounds, 0, &second);
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 310; ++x)
            assert(GetPixel(partial.dc(), x, y) == GetPixel(complete.dc(), x, y));
    ReleaseDC(nullptr, screen);
    ULONG_PTR gdiplusToken{};
    Gdiplus::GdiplusStartupInput startup;
    assert(Gdiplus::GdiplusStartup(&gdiplusToken, &startup, nullptr) == Gdiplus::Ok);
    {
        Gdiplus::Bitmap preview(620, 200, PixelFormat32bppRGB);
        Gdiplus::Graphics graphics(&preview);
        HDC dc = graphics.GetHDC();
        for (int i = 0; i < 4; ++i) {
            g_config.width = i < 2 ? 310 : 180;
            g_config.fontSize = i < 2 ? 17 : 44;
            g_config.clock.enabled = i % 2 == 0;
            const int saved = SaveDC(dc);
            SetViewportOrgEx(dc, (i % 2) * 310, (i / 2) * 100, nullptr);
            renderOverlay(dc, bounds, 0, &first);
            RestoreDC(dc, saved);
        }
        graphics.ReleaseHDC(dc);
        UINT count{}, bytes{};
        Gdiplus::GetImageEncodersSize(&count, &bytes);
        std::vector<BYTE> storage(bytes);
        auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
        Gdiplus::GetImageEncoders(count, bytes, encoders);
        bool saved = false;
        for (UINT i = 0; i < count; ++i) {
            if (wcscmp(encoders[i].MimeType, L"image/png") == 0)
                saved = preview.Save(L"dist/clock-preview.png", &encoders[i].Clsid) == Gdiplus::Ok;
        }
        assert(saved);
    }
    Gdiplus::GdiplusShutdown(gdiplusToken);
    g_config = Config{};
}

void testLeaderSettings() {
    wchar_t folder[MAX_PATH]{};
    wchar_t path[MAX_PATH]{};
    assert(GetFullPathNameW(L"dist", MAX_PATH, folder, nullptr));
    assert(GetTempFileNameW(folder, L"cfg", 0, path));
    const auto originalPath = g_configPath;
    g_configPath = path;
    // An older inline-mode preference must not disable the new default mode.
    WritePrivateProfileStringW(L"overlay", L"leader_inline", L"1", path);
    loadConfig();
    assert(g_config.leaderName == L"восток");
    assert(g_config.leaderEnabled && g_config.leaderSize == 128);
    assert(g_config.clock.enabled && g_config.clock.zoneId == L"Russian Standard Time");
    // Exercise the actual settings controls and Apply handler without showing a window.
    g_module = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.hInstance = g_module;
    wc.lpfnWndProc = settingsWindowProc;
    wc.lpszClassName = L"GooseOverlaySettingsTest";
    assert(RegisterClassW(&wc));
    auto* state = new SettingsState{g_config, true, nullptr, {}};
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Test",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, 0, 0, 638, 698,
        nullptr, nullptr, g_module, state);
    assert(window);
    assert(settingsControlText(window, IDC_SETTINGS_LEADER_NAME) == L"восток");
    assert(SetDlgItemTextW(window, IDC_SETTINGS_LEADER_NAME, L"  Новый Лидер 漢字  "));
    assert(SendDlgItemMessageW(window, IDC_SETTINGS_LEADER_ENABLED, BM_GETCHECK, 0, 0) == BST_CHECKED);
    SendDlgItemMessageW(window, IDC_SETTINGS_LEADER_ENABLED, BM_SETCHECK, BST_UNCHECKED, 0);
    assert(SetDlgItemTextW(window, IDC_SETTINGS_LEADER_SIZE, L"192"));
    assert(SendDlgItemMessageW(window, IDC_SETTINGS_CLOCK_ENABLED, BM_GETCHECK, 0, 0) == BST_CHECKED);
    const auto selected = SendDlgItemMessageW(window, IDC_SETTINGS_CLOCK_ZONE, CB_GETCURSEL, 0, 0);
    assert(selected > 0 && std::wstring(state->clockZones[selected - 1].TimeZoneKeyName) == L"Russian Standard Time");
    size_t nepalIndex = 0;
    for (size_t i = 0; i < state->clockZones.size(); ++i) {
        if (std::wstring(state->clockZones[i].TimeZoneKeyName) == L"Nepal Standard Time") nepalIndex = i + 1;
    }
    assert(nepalIndex > 0);
    SendDlgItemMessageW(window, IDC_SETTINGS_CLOCK_ZONE, CB_SETCURSEL, nepalIndex, 0);
    SendDlgItemMessageW(window, IDC_SETTINGS_CLOCK_ENABLED, BM_SETCHECK, BST_UNCHECKED, 0);
    assert(applySettings(window));
    assert(g_config.leaderName == L"Новый Лидер 漢字");
    assert(DestroyWindow(window));
    assert(UnregisterClassW(wc.lpszClassName, g_module));
    loadConfig();
    assert(g_config.leaderName == L"Новый Лидер 漢字");
    assert(!g_config.leaderEnabled && g_config.leaderSize == 192);
    assert(!g_config.clock.enabled && g_config.clock.zoneId == L"Nepal Standard Time");
    assert(setClockTimeZone(g_config.clock, L""));
    g_config.clock.enabled = true;
    assert(saveConfig(g_config, true));
    loadConfig();
    assert(g_config.clock.enabled && g_config.clock.zoneId.empty());
    assert(isLeaderName(L"НОВЫЙ ЛИДЕР 漢字", g_config.leaderName));
    assert(!isLeaderName(L"восток", g_config.leaderName));
    const std::wstring escaped = L"\\u0432";
    assert(decodeLeaderName(encodeLeaderName(escaped)) == escaped);
    assert(decodeLeaderName(L"\\uXX42") == L"\\uXX42");
    assert(DeleteFileW(path));
    g_configPath = originalPath;
    g_config = Config{};
}

void testMoveShortcutAndHint() {
    MoveShortcut shortcut;
    assert(!shortcut.sample(false, true));
    assert(shortcut.sample(true, true));
    for (int i = 0; i < 100; ++i) assert(!shortcut.sample(true, true)); // No repeat while held.
    assert(!shortcut.sample(false, true));
    assert(shortcut.sample(true, true)); // A second press cancels.
    assert(!shortcut.sample(false, false));
    assert(!shortcut.sample(true, false)); // Ignore keys outside the game or when disabled.
    assert(!shortcut.sample(true, true)); // Focus change with held keys must not activate.
    assert(!shortcut.sample(false, true));
    assert(shortcut.sample(true, true));
    MoveShortcut alreadyHeld{true};
    assert(!alreadyHeld.sample(true, true)); // Loading the plugin is not a key press.

    g_config = Config{};
    g_config.offsetX = g_config.offsetY = 0;
    g_config.leaderEnabled = false;
    g_clients = {{1, L"Участник", true, false, false, false}};
    g_whisperClients.clear();
    ULONG_PTR token{};
    Gdiplus::GdiplusStartupInput startup;
    assert(Gdiplus::GdiplusStartup(&token, &startup, nullptr) == Gdiplus::Ok);
    {
        Gdiplus::Bitmap preview(510, 280, PixelFormat32bppRGB);
        Gdiplus::Graphics graphics(&preview);
        HDC target = graphics.GetHDC();
        for (int i = 0; i < 2; ++i) {
            g_config.width = i == 0 ? 310 : 180;
            const RECT bounds{0, 0, g_config.width, 140};
            PaintBuffer idle, moving;
            assert(idle.ensure(target, g_config.width, 140));
            assert(moving.ensure(target, g_config.width, 140));
            g_moveMode = false;
            renderOverlay(idle.dc(), bounds, 0);
            const auto idlePanel = overlayPanelRect(bounds, g_config, 1);
            g_moveMode = true;
            renderOverlay(moving.dc(), bounds, 0);
            const auto movingPanel = overlayPanelRect(bounds, g_config, 1);
            assert(EqualRect(&idlePanel, &movingPanel)); // Hints never shift the panel during a drag.
            int hintChanges = 0;
            for (int y = 36; y < movingPanel.bottom; ++y) {
                for (int x = 0; x < g_config.width; ++x) {
                    const bool changed = GetPixel(idle.dc(), x, y) != GetPixel(moving.dc(), x, y);
                    if (y < movingPanel.bottom - kMoveHintHeight) assert(!changed); // Participants remain intact.
                    else hintChanges += changed;
                }
            }
            assert(hintChanges > 0);
            assert(BitBlt(target, i == 0 ? 0 : 330, 0, g_config.width, 140, idle.dc(), 0, 0, SRCCOPY));
            assert(BitBlt(target, i == 0 ? 0 : 330, 140, g_config.width, 140, moving.dc(), 0, 0, SRCCOPY));
        }
        graphics.ReleaseHDC(target);
        UINT count{}, bytes{};
        Gdiplus::GetImageEncodersSize(&count, &bytes);
        std::vector<BYTE> storage(bytes);
        auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
        Gdiplus::GetImageEncoders(count, bytes, encoders);
        bool saved = false;
        for (UINT i = 0; i < count; ++i)
            if (wcscmp(encoders[i].MimeType, L"image/png") == 0)
                saved = preview.Save(L"dist/move-hint-preview.png", &encoders[i].Clsid) == Gdiplus::Ok;
        assert(saved);
    }
    Gdiplus::GdiplusShutdown(token);
    g_moveMode = false;
    g_config = Config{};
    std::cout << "Move shortcut edge/focus handling and normal/narrow hint rendering tests passed\n";
}

void testMousePosition() {
    const auto originalPath = g_configPath;
    wchar_t folder[MAX_PATH]{}, path[MAX_PATH]{};
    assert(GetFullPathNameW(L"dist", MAX_PATH, folder, nullptr));
    assert(GetTempFileNameW(folder, L"pos", 0, path));
    g_configPath = path;
    loadConfig();
    g_config.opacity = 180;
    g_config.width = 350;
    g_config.leaderName = L"Мой лидер";
    g_config.leaderSize = 192;
    assert(setClockTimeZone(g_config.clock, L"UTC"));
    assert(saveConfig(g_config, true));
    g_clients.clear();
    g_whisperClients.clear();
    g_module = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.hInstance = g_module;
    wc.lpfnWndProc = overlayWindowProc;
    wc.lpszClassName = L"GooseOverlayDragTest";
    assert(RegisterClassW(&wc));
    HWND overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"Drag test", WS_POPUP, 0, 0, 1200, 800, nullptr, nullptr, g_module, nullptr);
    assert(overlay);
    g_overlayWindow = overlay;
    assert(SendMessageW(overlay, WM_NCHITTEST, 0, 0) == HTTRANSPARENT);
    SendMessageW(overlay, kMoveModeMessage, 1, 0);
    assert(g_moveMode && !(GetWindowLongPtrW(overlay, GWL_EXSTYLE) & WS_EX_TRANSPARENT));
    assert(GetWindowLongPtrW(overlay, GWL_EXSTYLE) & WS_EX_NOACTIVATE);
    assert(SendMessageW(overlay, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE);
    SendMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(836, 82));
    assert(g_drag.active);
    SendMessageW(overlay, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(610, 210));
    assert(g_config.offsetX == 250 && g_config.offsetY == 200);
    // Preview must not write to disk on every mouse move.
    assert(GetPrivateProfileIntW(L"overlay", L"offset_x", -1, path) == 24);
    SendMessageW(overlay, WM_LBUTTONUP, 0, MAKELPARAM(610, 210));
    assert(!g_moveMode && !g_drag.active && GetCapture() != overlay);
    assert(GetWindowLongPtrW(overlay, GWL_EXSTYLE) & WS_EX_TRANSPARENT);
    loadConfig();
    assert(g_config.offsetX == 250 && g_config.offsetY == 200);
    assert(g_config.opacity == 180 && g_config.width == 350 && g_config.clock.zoneId == L"UTC");
    assert(g_config.leaderName == L"Мой лидер" && g_config.leaderSize == 192);

    // Cancel and lost capture restore the last saved coordinates.
    for (const UINT cancel : {WM_RBUTTONDOWN, WM_CAPTURECHANGED, WM_CANCELMODE}) {
        SendMessageW(overlay, kMoveModeMessage, 1, 0);
        SendMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(610, 210));
        SendMessageW(overlay, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(-100, -100));
        assert(g_config.offsetX == 850 && g_config.offsetY == 0);
        SendMessageW(overlay, cancel, 0, 0);
        assert(!g_moveMode && !g_drag.active);
        assert(g_config.offsetX == 250 && g_config.offsetY == 200);
    }
    SendMessageW(overlay, kMoveModeMessage, 1, 0);
    SendMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(610, 210));
    SendMessageW(overlay, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(3000, 3000));
    assert(g_config.offsetX == 0 && g_config.offsetY == 686);
    stopMoving();
    assert(g_config.offsetX == 250 && g_config.offsetY == 200);
    // A smaller game window must keep the panel reachable.
    const auto smallPanel = overlayPanelRect({0, 0, 200, 140}, g_config, 0);
    assert(smallPanel.left == 0 && smallPanel.right == 200 && smallPanel.top == 26 && smallPanel.bottom == 140);
    const auto tall = overlayPanelRect({0, 0, 200, 100}, g_config, 16);
    assert(tall.top == 0);

    // Failed persistence restores coordinates and always releases mouse input.
    const auto validPath = g_configPath;
    g_configPath = std::wstring(folder) + L"\\missing-position-test-folder\\config.ini";
    SendMessageW(overlay, kMoveModeMessage, 1, 0);
    SendMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(610, 210));
    SendMessageW(overlay, WM_LBUTTONUP, 0, MAKELPARAM(710, 310));
    assert(!g_moveMode && !g_drag.active);
    assert(g_config.offsetX == 250 && g_config.offsetY == 200);
    assert(GetWindowLongPtrW(overlay, GWL_EXSTYLE) & WS_EX_TRANSPARENT);
    g_configPath = validPath;

    // Use the actual settings handler: reset changes only two pending fields.
    WNDCLASSW settingsClass{};
    settingsClass.hInstance = g_module;
    settingsClass.lpfnWndProc = settingsWindowProc;
    settingsClass.lpszClassName = L"GooseOverlayPositionSettingsTest";
    assert(RegisterClassW(&settingsClass));
    HWND settings = CreateWindowExW(0, settingsClass.lpszClassName, L"Settings test",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, 0, 0, 638, 698, nullptr, nullptr, g_module,
        new SettingsState{g_config, true, nullptr, {}});
    assert(settings);
    g_settingsWindow = settings;
    assert(SetDlgItemTextW(settings, IDC_SETTINGS_OPACITY, L"190"));
    SendMessageW(settings, WM_COMMAND, IDC_SETTINGS_RESET_POSITION, 0);
    assert(settingsControlText(settings, IDC_SETTINGS_OFFSET_X) == L"24");
    assert(settingsControlText(settings, IDC_SETTINGS_OFFSET_Y) == L"72");
    assert(settingsControlText(settings, IDC_SETTINGS_OPACITY) == L"190");
    assert(g_config.offsetX == 250 && GetPrivateProfileIntW(L"overlay", L"offset_x", -1, path) == 250);
    assert(applySettings(settings));
    loadConfig();
    assert(g_config.offsetX == 24 && g_config.offsetY == 72 && g_config.opacity == 190);
    assert(g_config.width == 350 && g_config.leaderName == L"Мой лидер" && g_config.clock.zoneId == L"UTC");
    // Completed dragging must update an already open settings window.
    SendMessageW(overlay, kMoveModeMessage, 1, 0);
    SendMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(836, 82));
    SendMessageW(overlay, WM_LBUTTONUP, 0, MAKELPARAM(510, 110));
    MSG message{};
    while (PeekMessageW(&message, settings, kPositionChangedMessage, kMoveStateChangedMessage, PM_REMOVE))
        DispatchMessageW(&message);
    assert(settingsControlText(settings, IDC_SETTINGS_OFFSET_X) == L"350");
    assert(settingsControlText(settings, IDC_SETTINGS_OFFSET_Y) == L"100");
    assert(applySettings(settings));
    loadConfig();
    assert(g_config.offsetX == 350 && g_config.offsetY == 100);
    SendMessageW(settings, WM_COMMAND, IDC_SETTINGS_RESET_POSITION, 0);
    SendMessageW(settings, WM_COMMAND, IDCANCEL, 0);
    loadConfig();
    assert(g_config.offsetX == 350 && g_config.offsetY == 100); // Cancel discards reset.
    assert(UnregisterClassW(settingsClass.lpszClassName, g_module));
    PluginHotkey** hotkeys{};
    ts3plugin_initHotkeys(&hotkeys);
    assert(hotkeys[0] && hotkeys[1] && !hotkeys[2]);
    assert(std::strcmp(hotkeys[1]->keyword, "goose_overlay_move") == 0);
    for (int i = 0; hotkeys[i]; ++i) ts3plugin_freeMemory(hotkeys[i]);
    ts3plugin_freeMemory(hotkeys);
    g_overlayWindow = nullptr;
    assert(DestroyWindow(overlay));
    assert(UnregisterClassW(wc.lpszClassName, g_module));
    assert(DeleteFileW(path));
    g_configPath = originalPath;
    g_config = Config{};
    std::cout << "Mouse movement, bounds, cancel, persistence and position-only reset tests passed\n";
}

void testLeaderPortrait() {
    g_config.clock.enabled = false; // Keep avatar pixel comparisons independent of wall time.
    assert(isLeaderName(L"восток"));
    assert(isLeaderName(L"  ВОСТОК  "));
    assert(isLeaderName(L"Восток"));
    assert(!isLeaderName(L"восток123"));
    assert(!isLeaderName(L"дальний восток"));
    assert(!isLeaderName(L""));
    assert(leaderMouthOpen(true, 0));
    assert(!leaderMouthOpen(true, 200));
    assert(leaderMouthOpen(true, 400));
    assert(!leaderMouthOpen(false, 0));

#ifdef _WIN64
    const wchar_t* dll = L"dist/package/plugins/bfg_overlay_win64.dll";
#else
    const wchar_t* dll = L"dist/package/plugins/bfg_overlay_win32.dll";
#endif
    HMODULE module = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_AS_DATAFILE);
    assert(module);
    {
        LeaderPortrait portrait(module);
        assert(portrait.ready()); // Decode both frames from the actual release DLL.
        g_leaderPortrait = &portrait;
        g_config.leaderEnabled = true;
        g_config.offsetX = 0;
        g_config.offsetY = 0;
        g_config.width = 310;
        g_config.rowHeight = 30;
        g_clients = {{1, L"восток", true, false, false, false},
                     {2, L"Другой участник", false, false, true, true}};
        Gdiplus::Bitmap openFrame(310, 300, PixelFormat32bppRGB);
        Gdiplus::Bitmap closedFrame(310, 300, PixelFormat32bppRGB);
        Gdiplus::Bitmap idleFrame(310, 300, PixelFormat32bppRGB);
        auto render = [&](Gdiplus::Bitmap& frame, bool talking, ULONGLONG tick) {
            g_clients[0].talking = talking;
            Gdiplus::Graphics graphics(&frame);
            HDC dc = graphics.GetHDC();
            PaintBuffer buffer;
            assert(renderBufferedOverlay(buffer, dc, {0, 0, 310, 300}, {0, 0, 310, 300}, tick));
            graphics.ReleaseHDC(dc);
        };
        render(openFrame, true, 0);
        render(closedFrame, true, 200);
        render(idleFrame, false, 0);
        // A portrait-only update must produce exactly the same pixels as a full frame,
        // while leaving the panel text, background and other users untouched.
        Gdiplus::Bitmap partialFrame(310, 300, PixelFormat32bppRGB);
        g_clients[0].talking = true;
        {
            Gdiplus::Graphics graphics(&partialFrame);
            HDC dc = graphics.GetHDC();
            PaintBuffer buffer;
            assert(renderBufferedOverlay(buffer, dc, {0, 0, 310, 300}, {0, 0, 310, 300}, 0));
            const HDC reusedDC = buffer.dc();
            const auto dirty = largeLeaderPortraitRect({0, 0, 310, 300}, g_config, g_clients.size());
            assert(renderBufferedOverlay(buffer, dc, {0, 0, 310, 300}, dirty, 200));
            assert(buffer.dc() == reusedDC);
            assert(buffer.ensure(dc, 620, 216)); // Recreate safely when the game changes size.
            assert(buffer.ensure(dc, 310, 300));
            graphics.ReleaseHDC(dc);
        }
        for (int y = 0; y < 300; ++y) {
            for (int x = 0; x < 310; ++x) {
                Gdiplus::Color partial, full;
                partialFrame.GetPixel(x, y, &partial);
                closedFrame.GetPixel(x, y, &full);
                if (partial.GetValue() != full.GetValue()) {
                    std::cerr << "Partial frame mismatch at " << x << "," << y << ": "
                        << partial.GetValue() << " != " << full.GetValue() << "\n";
                    std::abort();
                }
            }
        }
        int differences = 0;
        const auto avatarRect = largeLeaderPortraitRect({0, 0, 310, 300}, g_config, g_clients.size());
        assert(avatarRect.right - avatarRect.left == 128 && avatarRect.top == 152);
        for (int y = avatarRect.top; y < avatarRect.bottom; ++y) {
            for (int x = avatarRect.left; x < avatarRect.right; ++x) {
                Gdiplus::Color open, closed, idle;
                openFrame.GetPixel(x, y, &open);
                closedFrame.GetPixel(x, y, &closed);
                idleFrame.GetPixel(x, y, &idle);
                differences += open.GetValue() != closed.GetValue();
                const COLORREF background = kTransparentColor;
                assert(RGB(idle.GetR(), idle.GetG(), idle.GetB()) == background);
            }
        }
        assert(differences > 0);
        // Turning the feature off during speech clears the already displayed avatar
        // without changing text, activity indicators, or mute icons in the panel.
        Gdiplus::Bitmap disabledFrame(310, 300, PixelFormat32bppRGB);
        {
            Gdiplus::Graphics graphics(&disabledFrame);
            HDC dc = graphics.GetHDC();
            PaintBuffer buffer;
            g_clients[0].talking = true;
            assert(renderBufferedOverlay(buffer, dc, {0, 0, 310, 300}, {0, 0, 310, 300}, 0));
            g_config.leaderEnabled = false;
            assert(renderBufferedOverlay(buffer, dc, {0, 0, 310, 300}, {0, 0, 310, 300}, 0));
            graphics.ReleaseHDC(dc);
        }
        for (int y = 0; y < 300; ++y) {
            for (int x = 0; x < 310; ++x) {
                Gdiplus::Color disabled, original;
                disabledFrame.GetPixel(x, y, &disabled);
                openFrame.GetPixel(x, y, &original);
                if (x >= avatarRect.left && x < avatarRect.right && y >= avatarRect.top && y < avatarRect.bottom) {
                    assert(RGB(disabled.GetR(), disabled.GetG(), disabled.GetB()) == kTransparentColor);
                } else assert(disabled.GetValue() == original.GetValue());
            }
        }
        g_config.leaderEnabled = true;
        // Stop speaking on an already painted window: the former avatar must be erased.
        Gdiplus::Bitmap stoppedFrame(310, 300, PixelFormat32bppRGB);
        {
            Gdiplus::Graphics graphics(&stoppedFrame);
            HDC dc = graphics.GetHDC();
            PaintBuffer buffer;
            g_clients[0].talking = true;
            assert(renderBufferedOverlay(buffer, dc, {0, 0, 310, 300}, {0, 0, 310, 300}, 0));
            ts3plugin_onTalkStatusChangeEvent(g_server.load(), STATUS_NOT_TALKING, 0, 1);
            assert(renderBufferedOverlay(buffer, dc, {0, 0, 310, 300}, {0, 0, 310, 300}, 200));
            graphics.ReleaseHDC(dc);
        }
        for (int y = 0; y < 300; ++y) {
            for (int x = 0; x < 310; ++x) {
                Gdiplus::Color stopped, idle;
                stoppedFrame.GetPixel(x, y, &stopped);
                idleFrame.GetPixel(x, y, &idle);
                assert(stopped.GetValue() == idle.GetValue());
            }
        }
        Gdiplus::Bitmap preview(930, 300, PixelFormat32bppRGB);
        {
            Gdiplus::Graphics graphics(&preview);
            graphics.DrawImage(&openFrame, 0, 0);
            graphics.DrawImage(&closedFrame, 310, 0);
            graphics.DrawImage(&idleFrame, 620, 0);
        }
        UINT count{}, bytes{};
        Gdiplus::GetImageEncodersSize(&count, &bytes);
        std::vector<BYTE> storage(bytes);
        auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
        Gdiplus::GetImageEncoders(count, bytes, encoders);
        bool saved = false;
        for (UINT i = 0; i < count; ++i) {
            if (wcscmp(encoders[i].MimeType, L"image/png") == 0) {
                saved = preview.Save(L"dist/leader-preview.png", &encoders[i].Clsid) == Gdiplus::Ok;
            }
        }
        assert(saved);
        g_leaderPortrait = nullptr;
    }
    FreeLibrary(module);
}

int main() {
    g_ts3.getClientID = [](uint64, anyID* id) -> unsigned int { *id = 1; return ERROR_ok; };
    g_ts3.getChannelOfClient = [](uint64, anyID, uint64* id) -> unsigned int { *id = 5; return ERROR_ok; };
    g_ts3.getChannelClientList = [](uint64, uint64, anyID** ids) -> unsigned int {
        *ids = static_cast<anyID*>(std::malloc(3 * sizeof(anyID)));
        (*ids)[0] = 1; (*ids)[1] = 2; (*ids)[2] = 0;
        return ERROR_ok;
    };
    g_ts3.getClientVariableAsString = [](uint64, anyID, size_t, char** text) -> unsigned int {
        *text = _strdup("Channel member"); return ERROR_ok;
    };
    g_ts3.getClientVariableAsInt = [](uint64, anyID, size_t, int* result) -> unsigned int {
        *result = 0; return ERROR_ok;
    };
    g_ts3.freeMemory = [](void* ptr) -> unsigned int { std::free(ptr); return ERROR_ok; };
    g_ts3.getClientDisplayName = [](uint64, anyID id, char* name, size_t size) -> unsigned int {
        if (id == 99) return ERROR_undefined;
        strcpy_s(name, size, "восток"); return ERROR_ok;
    };

    refreshCurrentServer(77);
    assert(displayClients().size() == 2);
    ts3plugin_onTalkStatusChangeEvent(77, STATUS_TALKING, 1, 3);
    auto clients = displayClients();
    assert(clients.size() == 3 && clients[0].id == 3 && clients[0].whisper);
    assert(clients[0].name == L"восток" && isLeaderName(clients[0].name));
    refreshClients(77); // Channel update must not remove an active external speaker.
    assert(displayClients().size() == 3);
    ts3plugin_onTalkStatusChangeEvent(77, STATUS_TALKING, 1, 4);
    assert(displayClients().size() == 4);
    ts3plugin_onTalkStatusChangeEvent(77, STATUS_NOT_TALKING, 0, 3);
    assert(displayClients().size() == 3);
    ts3plugin_onTalkStatusChangeEvent(77, STATUS_NOT_TALKING, 1, 4);
    assert(displayClients().size() == 2);
    ts3plugin_onTalkStatusChangeEvent(77, STATUS_TALKING, 1, 2);
    clients = displayClients();
    assert(clients.size() == 2 && clients[0].id == 2 && clients[0].whisper);
    ts3plugin_onTalkStatusChangeEvent(77, STATUS_NOT_TALKING, 0, 2);
    assert(displayClients().size() == 2 && !displayClients()[0].whisper);
    ts3plugin_onTalkStatusChangeEvent(77, STATUS_TALKING, 1, 99);
    assert(displayClients()[0].name == L"Client #99");
    ts3plugin_onTalkStatusChangeEvent(88, STATUS_TALKING, 1, 9);
    assert(displayClients().size() == 3); // Ignore another server's events.
    refreshCurrentServer(88);
    assert(displayClients().size() == 2 && g_whisperClients.empty());
    ts3plugin_onTalkStatusChangeEvent(88, STATUS_TALKING_WHILE_DISABLED, 1, 1);
    assert(!displayClients()[0].talking);
    ts3plugin_onTalkStatusChangeEvent(88, STATUS_TALKING, 1, 3);
    ts3plugin_onConnectStatusChangeEvent(88, STATUS_DISCONNECTED, 0);
    assert(displayClients().empty());
    std::cout << "Whisper state tests passed\n";
    testClock();
    testClockRendering();
    testLeaderSettings();
    testMoveShortcutAndHint();
    testMousePosition();
    std::cout << "Clock defaults, settings, time zones, DST and partial rendering tests passed\n";
    testLeaderPortrait();
    std::cout << "Leader settings persistence, matching, resources and frame rendering tests passed\n";
}
