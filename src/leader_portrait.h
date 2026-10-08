#pragma once

#include <windows.h>
#include <objidl.h>
#include <algorithm>
// Older Windows SDK GDI+ headers use unqualified min/max with NOMINMAX.
namespace Gdiplus { using std::min; using std::max; }
#pragma warning(push)
#pragma warning(disable: 4458)
#include <gdiplus.h>
#pragma warning(pop)
#include <memory>
#include <cstring>
#include <string>
#include "resources.h"

// Unicode case comparison is independent of the process C locale.
inline bool isLeaderName(const std::wstring& nickname, const std::wstring& leader = L"восток") {
    const auto first = nickname.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return false;
    const auto name = nickname.substr(first, nickname.find_last_not_of(L" \t\r\n") - first + 1);
    return CompareStringOrdinal(name.c_str(), static_cast<int>(name.size()),
        leader.c_str(), static_cast<int>(leader.size()), TRUE) == CSTR_EQUAL;
}

// Profile APIs otherwise convert Unicode to the system ANSI code page in old INIs.
// ASCII escapes keep nicknames portable without rewriting the user's existing file.
inline std::wstring encodeLeaderName(const std::wstring& name) {
    std::wstring result;
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (wchar_t c : name) {
        if (c >= 32 && c < 127 && c != L'\\' && c != L'"' && c != L';') result += c;
        else {
            result += L"\\u";
            for (int shift = 12; shift >= 0; shift -= 4) result += hex[(c >> shift) & 15];
        }
    }
    return result;
}

inline std::wstring decodeLeaderName(const std::wstring& value) {
    std::wstring result;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == L'\\' && i + 5 < value.size() && value[i + 1] == L'u') {
            unsigned int decoded = 0;
            bool valid = true;
            for (size_t j = i + 2; j < i + 6; ++j) {
                wchar_t c = value[j];
                int digit = (c >= L'0' && c <= L'9') ? c - L'0' :
                    (c >= L'a' && c <= L'f') ? c - L'a' + 10 :
                    (c >= L'A' && c <= L'F') ? c - L'A' + 10 : -1;
                if (digit < 0) { valid = false; break; }
                decoded = (decoded << 4) | digit;
            }
            if (valid && decoded != 0) { result += static_cast<wchar_t>(decoded); i += 5; continue; }
        }
        result += value[i];
    }
    return result;
}

inline bool leaderMouthOpen(bool talking, ULONGLONG tick) {
    return talking && ((tick / 200) % 2 == 0);
}

class LeaderPortrait {
    struct Frame {
        IStream* stream{};
        std::unique_ptr<Gdiplus::Bitmap> image;

        void reset() {
            image.reset(); // GDI+ requires the stream to outlive the bitmap.
            if (stream) stream->Release();
            stream = nullptr;
        }

        ~Frame() { reset(); }

        void load(HINSTANCE module, int id) {
            HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
            if (!resource) return;
            const DWORD size = SizeofResource(module, resource);
            const void* bytes = LockResource(LoadResource(module, resource));
            if (!size || !bytes) return;
            HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
            if (!memory) return;
            void* buffer = GlobalLock(memory);
            if (!buffer) { GlobalFree(memory); return; }
            std::memcpy(buffer, bytes, size);
            GlobalUnlock(memory);
            if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream))) {
                GlobalFree(memory);
                return;
            }
            image.reset(Gdiplus::Bitmap::FromStream(stream));
            if (!image || image->GetLastStatus() != Gdiplus::Ok) reset();
        }
    };

    ULONG_PTR token_{};
    Frame idle_;
    Frame speaking_;

public:
    explicit LeaderPortrait(HINSTANCE module) {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token_, &input, nullptr) != Gdiplus::Ok) {
            token_ = 0;
            return;
        }
        idle_.load(module, IDR_LEADER_IDLE);
        speaking_.load(module, IDR_LEADER_SPEAKING);
    }

    LeaderPortrait(const LeaderPortrait&) = delete;
    LeaderPortrait& operator=(const LeaderPortrait&) = delete;

    ~LeaderPortrait() {
        idle_.reset();
        speaking_.reset();
        if (token_) Gdiplus::GdiplusShutdown(token_);
    }

    bool ready() const { return idle_.image && speaking_.image; }

    bool draw(HDC dc, int x, int y, int size, bool talking, ULONGLONG tick) {
        if (!ready()) return false;
        auto* frame = leaderMouthOpen(talking, tick) ? speaking_.image.get() : idle_.image.get();
        Gdiplus::Graphics graphics(dc);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        return graphics.DrawImage(frame, x, y, size, size) == Gdiplus::Ok;
    }
};
