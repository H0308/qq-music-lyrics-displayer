#include "ui/taskbar/text_scroll.h"
#include "ui/taskbar/marquee.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr float kTextPadding = 8.0f, kVerticalLyricGap = 6.0f;
constexpr float kInfoScrollSpeed = 10.0f, kLyricScrollSpeed = 15.0f;
constexpr float kKaraokeScrollFollowMs = 100.0f;
constexpr ULONGLONG kOneShotStatusTextHoldMs = 5000;
constexpr int kOneShotStatusTextRounds = 3;
} // namespace
void TextScroll::advance(const TextScrollInput& input) {
    const ULONGLONG now = input.now;
    if (lastTickMs_ == 0)
        lastTickMs_ = now;
    const float dt = static_cast<float>(now - lastTickMs_) / 1000.0f;
    lastTickMs_ = now;
    // 静止跳帧判定：本帧有任何滚动/转场在动就置 animating，供 onTimer 决定是否重绘
    bool animating = input.wasTransitioning;
    auto marquee = [&](float textW, float areaW, float speed, float& offset,
                       float loopGap = kTextPadding * 2.0f) {
        animating = taskbar_detail::advanceMarquee(textW, areaW, speed, dt, loopGap,
                                                   input.animations, offset) ||
                    animating;
    };
    auto finishOneShotStatus = [&]() {
        statusTextOneShot_ = false;
        statusTextOneShotStartMs_ = 0;
        statusTextOneShotRounds_ = 0;
        statusTextCycleCallbackPending_ = true;
    };
    auto oneShotMarquee = [&](float textW, float areaW, float speed, float& offset, float loopGap) {
        if (!statusTextOneShot_) {
            marquee(textW, areaW, speed, offset, loopGap);
            return false;
        }
        if (statusTextOneShotStartMs_ == 0)
            statusTextOneShotStartMs_ = now;
        if (!input.animations || textW <= areaW || areaW <= 0.0f) {
            if (offset != 0.0f) {
                offset = 0.0f;
                animating = true;
            }
            if (now - statusTextOneShotStartMs_ >= kOneShotStatusTextHoldMs) {
                finishOneShotStatus();
                return true;
            }
            return false;
        }

        const float loopW = textW + loopGap;
        const float advance = speed * std::max(dt, 0.0f);
        const float previous = offset;
        const float total = previous + advance;
        const int completedRounds = static_cast<int>(total / loopW);
        offset = std::fmod(total, loopW);
        if (offset < 0.0f)
            offset += loopW;
        animating = true;
        if (completedRounds > 0) {
            statusTextOneShotRounds_ += completedRounds;
        }
        if (statusTextOneShotRounds_ >= kOneShotStatusTextRounds) {
            finishOneShotStatus();
            return true;
        }
        return false;
    };

    if (input.vertical) {
        const float lyricAreaH = input.area;
        if (lyricAreaH <= 0.0f) {
            scrollAnimating_ = animating;
            return;
        }
        const bool holdLyricScroll = input.hold;
        const bool lyricMarqueePlaying = input.playing;
        const bool idleMarquee = input.idle;
        const float verticalLyricExtent = input.verticalExtent;
        if (input.showControls) {
            scrollAnimating_ = animating;
            return;
        }
        if (input.karaoke && input.animations) {
            const float before = lyricScrollOffset_;
            if (!holdLyricScroll) {
                if (verticalLyricExtent > lyricAreaH) {
                    const float sungX = input.karaokeProgress;
                    // 普通竖向跑马灯的 offset 正值代表向下；逐字高亮跟随时，
                    // 只有内容超出底部后才向上回收文本，避免当前字被推出可视区。
                    const float target = std::clamp(
                        lyricAreaH * 0.3f -
                            verticalLyricExtent *
                                (input.lyricWidth > 0.0f ? sungX / input.lyricWidth : 0.0f),
                        lyricAreaH - verticalLyricExtent, 0.0f);
                    const float diff = target - lyricScrollOffset_;
                    const float followDtMs = std::clamp(std::max(dt, 0.0f) * 1000.0f, 0.0f, 250.0f);
                    const float alpha = 1.0f - std::exp(-followDtMs / kKaraokeScrollFollowMs);
                    lyricScrollOffset_ =
                        std::fabs(diff) < 0.5f ? target : lyricScrollOffset_ + diff * alpha;
                } else {
                    lyricScrollOffset_ = 0.0f;
                }
            }
            if (lyricScrollOffset_ != before)
                animating = true;
        } else if (input.karaoke) {
            if (lyricScrollOffset_ != 0.0f) {
                lyricScrollOffset_ = 0.0f;
                animating = true;
            }
        } else if ((lyricMarqueePlaying || idleMarquee) && !holdLyricScroll) {
            if (statusTextOneShot_ && idleMarquee) {
                if (oneShotMarquee(verticalLyricExtent, lyricAreaH, kInfoScrollSpeed,
                                   lyricScrollOffset_, kVerticalLyricGap)) {
                    scrollAnimating_ = false;
                    return;
                }
            } else {
                marquee(verticalLyricExtent, lyricAreaH,
                        idleMarquee ? kInfoScrollSpeed : lyricScrollSpeed_, lyricScrollOffset_,
                        kVerticalLyricGap);
            }
        }
        scrollAnimating_ = animating;
        return;
    }
    if (!input.areaValid) {
        scrollAnimating_ = animating;
        return;
    }
    const float infoW = input.infoWidth;
    const float lyricAreaW = input.area;
    const bool holdLyricScroll = input.hold;
    // 普通横向歌词只在播放中推进；每日一言无播放状态也允许慢速滚动。
    // 暂停歌词时保留偏移，恢复播放后从原位置继续。
    const bool lyricMarqueePlaying = input.playing;
    const bool idleMarquee = input.idle;

    if (input.songInfoVisible) {
        marquee(input.titleWidth, infoW, kInfoScrollSpeed, titleScrollOffset_);
        marquee(input.artistWidth, infoW, kInfoScrollSpeed, artistScrollOffset_);
    }
    // 空闲场景没有内嵌播放控件，悬浮时也应继续滚动每日一言。
    if (input.showInlineControls && !idleMarquee) {
        scrollAnimating_ = animating;
        return;
    }
    // 逐字歌词：滚动位置跟随逐字填充进度，把当前唱到的位置保持在区域 30% 处，
    // 行尾为止不再循环；非逐字歌词保持原有无缝循环滚动
    if (input.karaoke && input.animations) {
        const float before = lyricScrollOffset_;
        if (!holdLyricScroll) {
            if (input.lyricWidth > lyricAreaW) {
                // karaokeProgX_ 由 render 每帧更新；行刚切换时还没对应进度，从头开始
                float sungX = input.karaokeProgress;
                float target =
                    std::clamp(sungX - lyricAreaW * 0.3f, 0.0f, input.lyricWidth - lyricAreaW);
                float diff = target - lyricScrollOffset_;
                // 用时间常数而不是固定帧比例，保证 30/60/120Hz 下观感一致。
                const float followDtMs = std::clamp(std::max(dt, 0.0f) * 1000.0f, 0.0f, 250.0f);
                const float alpha = 1.0f - std::exp(-followDtMs / kKaraokeScrollFollowMs);
                lyricScrollOffset_ =
                    std::fabs(diff) < 0.5f ? target : lyricScrollOffset_ + diff * alpha;
            } else {
                lyricScrollOffset_ = 0.0f;
            }
        }
        if (lyricScrollOffset_ != before)
            animating = true;
    } else if (input.karaoke) {
        if (lyricScrollOffset_ != 0.0f) {
            lyricScrollOffset_ = 0.0f;
            animating = true;
        }
    } else if ((lyricMarqueePlaying || idleMarquee) && !holdLyricScroll) {
        if (statusTextOneShot_ && idleMarquee) {
            if (oneShotMarquee(input.lyricWidth, lyricAreaW, kInfoScrollSpeed, lyricScrollOffset_,
                               kTextPadding * 2.0f)) {
                scrollAnimating_ = false;
                return;
            }
        } else {
            marquee(input.lyricWidth, lyricAreaW,
                    idleMarquee ? kInfoScrollSpeed : lyricScrollSpeed_, lyricScrollOffset_);
        }
    }
    if (lyricMarqueePlaying && !holdLyricScroll)
        marquee(input.secondaryWidth, lyricAreaW, kLyricScrollSpeed, secondaryScrollOffset_);
    scrollAnimating_ = animating;
}
void TextScroll::setLineTiming(const std::vector<LyricLine>& lines, int line, float extent,
                               float gap) {
    lyricScrollSpeed_ = kLyricScrollSpeed;
    if (line >= 0 && static_cast<size_t>(line) + 1 < lines.size() && extent > 0.0f) {
        const int64_t duration = lines[line + 1].ms - lines[line].ms;
        if (duration > 0)
            lyricScrollSpeed_ = std::max(kLyricScrollSpeed,
                                         (extent + gap) / (static_cast<float>(duration) / 1000.0f));
    }
}
