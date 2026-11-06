#pragma once
#include "model/display_types.h"
struct AppSettings;

namespace display_rules {
// 运行环境只产生显示限制，不能据此改写持久化配置。
struct DisplayConstraints {
    bool vertical;
    bool minimal;
    bool spectrumAllowed;
};
DisplayConstraints resolveConstraints(const AppSettings& settings, RenderMode mode, bool vertical);
bool spectrumRequested(const AppSettings& settings, RenderMode mode, bool vertical);

// 后一次用户操作决定冲突双方的优先级；这里只修改配置，不发布界面或保存。
enum class LyricPriority { Secondary, Effects };
bool resolveLyricConflict(AppSettings& settings, LyricPriority priority);
} // namespace display_rules
