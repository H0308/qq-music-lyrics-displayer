#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace runtime_log {

// 运行日志窗口读取的低频状态快照。日志正文与可视化状态分开，避免 UI 刷新
// 把资源采样或高频播放事件直接写入磁盘。
struct RuntimeLogSnapshot {
    bool playbackActive = false;
    std::wstring currentTitle;
    std::wstring currentArtist;
    int64_t durationMs = 0;
    std::wstring lyricSource = L"未加载";
    bool coverLoaded = false;

    // -1 表示当前系统没有提供对应的采样值。
    double cpuPercent = -1.0;
    double gpuPercent = -1.0;
    // 当前专用工作集，口径与任务管理器的进程内存显示一致。
    uint64_t memoryBytes = 0;

    std::wstring logDirectory;
    std::wstring sessionFileName;
    int retentionDays = 30;
};

class RuntimeLogger {
public:
    RuntimeLogger();
    ~RuntimeLogger();

    RuntimeLogger(const RuntimeLogger&) = delete;
    RuntimeLogger& operator=(const RuntimeLogger&) = delete;

    // start 会创建本次运行独立的日志文件：QQMusicLyric-开始时间戳.log。
    void start(const std::wstring& directory, int retentionDays = 30);
    void stop();

    void write(const std::wstring& message);
    void writef(const wchar_t* format, ...);
    void flushSync();

    void setDirectory(const std::wstring& directory);
    void setRetentionDays(int days);
    std::wstring directory() const;
    int retentionDays() const;
    void openDirectory() const;

    void setPlayback(const std::wstring& title, const std::wstring& artist,
                     int64_t durationMs, bool active);
    void setLyricSource(const std::wstring& source);
    void setCoverLoaded(bool loaded);
    RuntimeLogSnapshot snapshot() const;

    static std::wstring defaultDirectory();
    // 上一版本的默认目录，仅用于把历史默认配置迁移到新的临时目录。
    static std::wstring legacyDefaultDirectory();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 供已有模块把原来的控制台诊断输出统一写入当前运行日志。
// logger 尚未启动时仍会输出到调试器，不会阻塞启动流程。
void writef(const wchar_t* format, ...);

} // namespace runtime_log
