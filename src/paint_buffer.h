#pragma once
#include <windows.h>

// Owned by the overlay thread. Reuse GDI objects until the window size changes.
class PaintBuffer {
    HDC dc_{};
    HBITMAP bitmap_{};
    HGDIOBJ original_{};
    int width_{};
    int height_{};

public:
    PaintBuffer() = default;
    PaintBuffer(const PaintBuffer&) = delete;
    PaintBuffer& operator=(const PaintBuffer&) = delete;
    ~PaintBuffer() { reset(); }

    void reset() {
        if (dc_ && original_) SelectObject(dc_, original_);
        if (bitmap_) DeleteObject(bitmap_);
        if (dc_) DeleteDC(dc_);
        dc_ = nullptr;
        bitmap_ = nullptr;
        original_ = nullptr;
        width_ = height_ = 0;
    }

    bool ensure(HDC target, int width, int height) {
        if (width <= 0 || height <= 0) return false;
        if (dc_ && width == width_ && height == height_) return true;
        HDC nextDC = CreateCompatibleDC(target);
        if (!nextDC) return false;
        HBITMAP nextBitmap = CreateCompatibleBitmap(target, width, height);
        if (!nextBitmap) { DeleteDC(nextDC); return false; }
        HGDIOBJ original = SelectObject(nextDC, nextBitmap);
        if (!original || original == HGDI_ERROR) {
            DeleteObject(nextBitmap);
            DeleteDC(nextDC);
            return false;
        }
        reset();
        dc_ = nextDC;
        bitmap_ = nextBitmap;
        original_ = original;
        width_ = width;
        height_ = height;
        return true;
    }

    HDC dc() const { return dc_; }

    bool present(HDC target, const RECT& dirty) const {
        return BitBlt(target, dirty.left, dirty.top, dirty.right - dirty.left,
            dirty.bottom - dirty.top, dc_, dirty.left, dirty.top, SRCCOPY) != FALSE;
    }
};
