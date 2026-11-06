#pragma once
#include "model/display_types.h"
#include "monitor/resource_monitor.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <algorithm>
#include <array>
#include <string>
#include <vector>

// 独占采样、展示文案、列宽和分页；绘图目标、字体和画刷只在 draw 调用期间借用。
class ResourcePanel {
public:
    const DockResourceVisibility& visibility() const { return dockResourceVisibility_; }
    void setVisibility(const DockResourceVisibility& value) {
        dockResourceVisibility_ = value;
        resetPage();
    }
    void setCompact(bool compact) {
        if (compact_ == compact)
            return;
        dockResourcePage_ = compact ? dockResourcePage_ * 2 : dockResourcePage_ / 2;
        compact_ = compact;
        dockResourcePage_ = std::min(dockResourcePage_, pageCount() - 1);
    }
    void resetPage() { dockResourcePage_ = 0; }
    void start();
    void stop();
    bool refresh();
    float width() const;
    size_t pageCount() const;
    bool advancePage(int steps);
    bool updateColumnWidths(IDWriteFactory* dwrite, IDWriteTextFormat* format);
    void draw(ID2D1DeviceContext* target, IDWriteTextFormat* format, ID2D1Brush* brush, float x,
              float height, float width) const;

private:
    struct DockResourceRow {
        std::wstring left, right;
    };
    std::vector<DockResourceRow> rows() const;
    static std::wstring formatResourcePercent(double percent);
    static std::wstring formatNetworkRate(uint64_t bytesPerSecond);
    static std::wstring formatFrequency(bool available, double gigahertz);
    static float measureResourceTextWidth(IDWriteFactory* dwrite, IDWriteTextFormat* format,
                                          const std::wstring& text);
    system_monitor::ResourceMonitor resourceMonitor_;
    uint64_t resourceSnapshotRevision_ = 0;
    ULONGLONG nextResourceSnapshotPollMs_ = 0;
    std::wstring resourceCpuText_ = L"—";
    std::wstring resourceMemoryText_ = L"—";
    std::wstring resourceDownloadText_ = L"—";
    std::wstring resourceUploadText_ = L"—";
    std::wstring resourceGpuText_ = L"—";
    std::wstring resourceCpuFrequencyText_ = L"—";
    DockResourceVisibility dockResourceVisibility_{};
    size_t dockResourcePage_ = 0;
    float resourceLeftColumnW_ = 76.0f;
    float resourceRightColumnW_ = 100.0f;
    std::array<float, 2> resourceSingleLineWidths_{};
    bool compact_ = false;
};
