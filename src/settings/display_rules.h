#pragma once
struct AppSettings;

namespace display_rules {
// 后一次用户操作决定冲突双方的优先级；这里只修改配置，不发布界面或保存。
enum class LyricPriority { Secondary, Effects };
bool resolveLyricConflict(AppSettings& settings, LyricPriority priority);
} // namespace display_rules
