#include "ui/taskbar/resource_panel.h"
#include "ui/taskbar/animation_clock.h"
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <cstdint>

namespace {
constexpr float kDockResourceZoneGap = 10.0f;
constexpr float kResourceCellSafetyPadding = 1.0f;
constexpr UINT kResourceSnapshotPollMs = 250;
} // namespace
using taskbar_detail::monotonicNowMs;

float ResourcePanel::width() const {
    if (compact_) {
        float width = 0.0f;
        int count = 0;
        for (float cellWidth : resourceSingleLineWidths_) {
            if (cellWidth > 0.0f) {
                width += cellWidth;
                ++count;
            }
        }
        return width + std::max(0, count - 1) * kDockResourceZoneGap;
    }
    return resourceLeftColumnW_ +
           (resourceRightColumnW_ > 0.0f ? kDockResourceZoneGap + resourceRightColumnW_ : 0.0f);
}

std::vector<ResourcePanel::DockResourceRow> ResourcePanel::rows() const {
    std::vector<std::wstring> cells{L"CPU: " + resourceCpuText_, L"内存: " + resourceMemoryText_};
    if (dockResourceVisibility_.download)
        cells.push_back(L"下行: " + resourceDownloadText_);
    if (dockResourceVisibility_.upload)
        cells.push_back(L"上行: " + resourceUploadText_);
    if (dockResourceVisibility_.gpuUsage)
        cells.push_back(L"GPU: " + resourceGpuText_);
    if (dockResourceVisibility_.cpuFrequency)
        cells.push_back(L"频率: " + resourceCpuFrequencyText_);

    std::vector<DockResourceRow> rows;
    if (compact_) {
        for (size_t i = 0; i < cells.size(); i += 2)
            rows.push_back({cells[i], i + 1 < cells.size() ? cells[i + 1] : L""});
        return rows;
    }
    for (size_t i = 0; i < cells.size(); i += 4) {
        rows.push_back({cells[i], i + 2 < cells.size() ? cells[i + 2] : L""});
        rows.push_back(
            {i + 1 < cells.size() ? cells[i + 1] : L"", i + 3 < cells.size() ? cells[i + 3] : L""});
    }
    return rows;
}

size_t ResourcePanel::pageCount() const {
    const auto rows = ResourcePanel::rows();
    return compact_ ? rows.size() : rows.size() / 2;
}

std::wstring ResourcePanel::formatResourcePercent(double percent) {
    wchar_t text[16]{};
    swprintf_s(text, L"%.0f%%", std::clamp(percent, 0.0, 100.0));
    return text;
}

std::wstring ResourcePanel::formatNetworkRate(uint64_t bytesPerSecond) {
    static constexpr const wchar_t* units[] = {L"B/s", L"KB/s", L"MB/s", L"GB/s", L"TB/s"};
    double value = static_cast<double>(bytesPerSecond);
    size_t unit = 0;
    while (std::round(value) >= 1000.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    if (unit + 1 == std::size(units))
        value = std::min(value, 999.0);

    const double roundedTenths = std::round(value * 10.0) / 10.0;
    wchar_t text[32]{};
    if (unit == 0 || roundedTenths >= 100.0)
        swprintf_s(text, L"%.0f%ls", std::round(value), units[unit]);
    else
        swprintf_s(text, L"%.1f%ls", roundedTenths, units[unit]);
    return text;
}

std::wstring ResourcePanel::formatFrequency(bool available, double gigahertz) {
    if (!available)
        return L"—";
    wchar_t text[24]{};
    swprintf_s(text, L"%.1fGHz", std::clamp(gigahertz, 0.0, 99.0));
    return text;
}

void ResourcePanel::start() {
    resourceCpuText_ = L"—";
    resourceMemoryText_ = L"—";
    resourceDownloadText_ = L"—";
    resourceUploadText_ = L"—";
    resourceGpuText_ = L"—";
    resourceCpuFrequencyText_ = L"—";
    resourceSnapshotRevision_ = UINT64_MAX;
    nextResourceSnapshotPollMs_ = 0;
    resourceMonitor_.start();
}

void ResourcePanel::stop() {
    resourceMonitor_.stop();
    nextResourceSnapshotPollMs_ = 0;
}

bool ResourcePanel::refresh() {
    const ULONGLONG now = monotonicNowMs();
    if (now < nextResourceSnapshotPollMs_)
        return false;
    nextResourceSnapshotPollMs_ = now + kResourceSnapshotPollMs;

    const system_monitor::ResourceSnapshot snapshot = resourceMonitor_.snapshot();
    if (snapshot.revision == resourceSnapshotRevision_)
        return false;
    resourceSnapshotRevision_ = snapshot.revision;
    resourceCpuText_ =
        snapshot.cpuAvailable ? ResourcePanel::formatResourcePercent(snapshot.cpuPercent) : L"—";
    resourceMemoryText_ = snapshot.memoryAvailable
                              ? ResourcePanel::formatResourcePercent(snapshot.memoryPercent)
                              : L"—";
    resourceDownloadText_ = snapshot.networkAvailable
                                ? ResourcePanel::formatNetworkRate(snapshot.downloadBytesPerSecond)
                                : L"—";
    resourceUploadText_ = snapshot.networkAvailable
                              ? ResourcePanel::formatNetworkRate(snapshot.uploadBytesPerSecond)
                              : L"—";
    resourceGpuText_ =
        snapshot.gpuAvailable ? ResourcePanel::formatResourcePercent(snapshot.gpuPercent) : L"—";
    resourceCpuFrequencyText_ =
        ResourcePanel::formatFrequency(snapshot.cpuFrequencyAvailable, snapshot.cpuFrequencyGHz);
    return true;
}

void ResourcePanel::draw(ID2D1DeviceContext* rt, IDWriteTextFormat* format, ID2D1Brush* brush,
                         float x, float h, float width) const {
    if (!rt || !format || width <= 0.0f || !brush)
        return;

    const auto rows = ResourcePanel::rows();
    const size_t pageCount = compact_ ? rows.size() : rows.size() / 2;
    const size_t page = std::min(dockResourcePage_, pageCount - 1);

    if (compact_) {
        const std::array<const std::wstring*, 2> cells{&rows[page].left, &rows[page].right};
        rt->PushAxisAlignedClip(D2D1::RectF(x, 0.0f, x + width, h), D2D1_ANTIALIAS_MODE_ALIASED);
        float cellX = x;
        for (size_t i = 0; i < cells.size(); ++i) {
            if (cells[i]->empty())
                continue;
            const float cellRight = std::min(x + width, cellX + resourceSingleLineWidths_[i]);
            if (cellRight <= cellX)
                break;
            rt->DrawTextW(cells[i]->c_str(), static_cast<UINT32>(cells[i]->size()), format,
                          D2D1::RectF(cellX, 0.0f, cellRight, h), brush,
                          D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            cellX += resourceSingleLineWidths_[i] + kDockResourceZoneGap;
        }
        rt->PopAxisAlignedClip();
        return;
    }

    std::array<DockResourceRow, 2> visibleRows{};
    visibleRows[0] = rows[page * 2];
    visibleRows[1] = rows[page * 2 + 1];
    const float blockH = std::min(38.0f, std::max(1.0f, h - 2.0f));
    const float top = std::max(0.0f, (h - blockH) * 0.5f);
    const float rowH = blockH * 0.5f;
    const float leftColumnW =
        std::min(resourceLeftColumnW_, std::max(1.0f, width - kDockResourceZoneGap));
    const float rightColumnX = x + leftColumnW + kDockResourceZoneGap;
    const float rightColumnW = std::max(1.0f, width - leftColumnW - kDockResourceZoneGap);
    const D2D1_DRAW_TEXT_OPTIONS drawOptions = width + 1.0f >= ResourcePanel::width()
                                                   ? D2D1_DRAW_TEXT_OPTIONS_NONE
                                                   : D2D1_DRAW_TEXT_OPTIONS_CLIP;

    auto drawResourceCell = [&](const std::wstring& text, float left, float rowTop, float cellW) {
        if (text.empty() || cellW <= 0.0f)
            return;
        rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format,
                      D2D1::RectF(left, rowTop, left + cellW, rowTop + rowH), brush, drawOptions,
                      DWRITE_MEASURING_MODE_NATURAL);
    };
    for (size_t row = 0; row < visibleRows.size(); ++row) {
        const auto& current = visibleRows[row];
        const float rowTop = top + row * rowH;
        if (current.right.empty()) {
            drawResourceCell(current.left, x, rowTop, width);
        } else {
            drawResourceCell(current.left, x, rowTop, leftColumnW);
            drawResourceCell(current.right, rightColumnX, rowTop, rightColumnW);
        }
    }
}

float ResourcePanel::measureResourceTextWidth(IDWriteFactory* dwrite, IDWriteTextFormat* format,
                                              const std::wstring& text) {
    if (!dwrite || !format || text.empty())
        return 0.0f;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                        0.0f, 0.0f, &layout)) ||
        !layout)
        return 0.0f;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    float measuredWidth = 0.0f;
    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(layout->GetMetrics(&metrics)))
        measuredWidth = std::max(measuredWidth, metrics.widthIncludingTrailingWhitespace);
    DWRITE_OVERHANG_METRICS overhangs{};
    if (SUCCEEDED(layout->GetOverhangMetrics(&overhangs)))
        measuredWidth = std::max(measuredWidth, overhangs.right);
    layout->Release();
    return measuredWidth;
}

bool ResourcePanel::updateColumnWidths(IDWriteFactory* dwrite, IDWriteTextFormat* format) {
    if (!dwrite || !format)
        return false;

    wchar_t widestDigit = L'8';
    float widestDigitWidth = -1.0f;
    for (wchar_t digit = L'0'; digit <= L'9'; ++digit) {
        const float digitWidth =
            ResourcePanel::measureResourceTextWidth(dwrite, format, std::wstring(1, digit));
        if (digitWidth > widestDigitWidth) {
            widestDigitWidth = digitWidth;
            widestDigit = digit;
        }
    }

    const std::wstring singleDigit(1, widestDigit);
    const std::wstring twoDigits = singleDigit + singleDigit;
    const std::wstring threeDigits = twoDigits + singleDigit;
    const std::wstring decimalDigits = twoDigits + L"." + singleDigit;

    float leftMaxWidth = 0.0f;
    float rightMaxWidth = 0.0f;
    std::array<float, 2> singleLineWidths{};
    size_t cellIndex = 0;
    auto measureCell = [&](const std::vector<std::wstring>& samples) {
        float& columnWidth = cellIndex % 4 < 2 ? leftMaxWidth : rightMaxWidth;
        for (const auto& sample : samples) {
            const float measured = ResourcePanel::measureResourceTextWidth(dwrite, format, sample);
            columnWidth = std::max(columnWidth, measured);
            singleLineWidths[cellIndex % 2] = std::max(singleLineWidths[cellIndex % 2], measured);
        }
        ++cellIndex;
    };
    measureCell({L"CPU: 100%", L"CPU: " + twoDigits + L"%"});
    measureCell({L"内存: 100%", L"内存: " + twoDigits + L"%"});
    static constexpr const wchar_t* rateUnits[] = {L"B/s", L"KB/s", L"MB/s", L"GB/s", L"TB/s"};
    auto measureRate = [&](const wchar_t* prefixLabel) {
        std::vector<std::wstring> samples;
        for (const wchar_t* unitLabel : rateUnits) {
            samples.push_back(std::wstring(prefixLabel) + decimalDigits + unitLabel);
            samples.push_back(std::wstring(prefixLabel) + threeDigits + unitLabel);
        }
        measureCell(samples);
    };
    if (dockResourceVisibility_.download)
        measureRate(L"下行: ");
    if (dockResourceVisibility_.upload)
        measureRate(L"上行: ");
    if (dockResourceVisibility_.gpuUsage)
        measureCell({L"GPU: 100%", L"GPU: " + twoDigits + L"%"});
    if (dockResourceVisibility_.cpuFrequency)
        measureCell({L"频率: " + decimalDigits + L"GHz"});

    const float nextLeftWidth =
        std::max(1.0f, std::ceil(leftMaxWidth) + kResourceCellSafetyPadding);
    const float nextRightWidth =
        rightMaxWidth > 0.0f ? std::ceil(rightMaxWidth) + kResourceCellSafetyPadding : 0.0f;
    for (float& cellWidth : singleLineWidths)
        if (cellWidth > 0.0f)
            cellWidth = std::ceil(cellWidth) + kResourceCellSafetyPadding;

    const bool changed = nextLeftWidth != resourceLeftColumnW_ ||
                         nextRightWidth != resourceRightColumnW_ ||
                         singleLineWidths != resourceSingleLineWidths_;
    resourceLeftColumnW_ = nextLeftWidth;
    resourceRightColumnW_ = nextRightWidth;
    resourceSingleLineWidths_ = singleLineWidths;
    return changed;
}

bool ResourcePanel::advancePage(int steps) {
    const size_t pageCount = ResourcePanel::pageCount();
    if (pageCount <= 1 || steps == 0)
        return false;

    const long long count = static_cast<long long>(pageCount);
    long long next = static_cast<long long>(dockResourcePage_) + steps;
    next %= count;
    if (next < 0)
        next += count;
    dockResourcePage_ = static_cast<size_t>(next);
    return true;
}
