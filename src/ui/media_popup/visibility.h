#pragma once

// 显隐阶段只描述窗口入场和离场；页面切换、列表展开可以独立推进。
class PopupVisibility {
public:
    enum class Phase { Hidden, Preparing, Opening, Shown, Closing };
    bool visible() const {
        return phase_ == Phase::Opening || phase_ == Phase::Shown || phase_ == Phase::Closing;
    }
    bool opening() const { return phase_ == Phase::Preparing || phase_ == Phase::Opening; }
    bool closing() const { return phase_ == Phase::Closing; }
    bool animating() const { return opening() || closing(); }
    void beginOpen() { phase_ = visible() ? Phase::Opening : Phase::Preparing; }
    void markVisible() { phase_ = Phase::Opening; }
    void finishOpen() {
        if (phase_ == Phase::Preparing)
            phase_ = Phase::Hidden;
        else if (phase_ == Phase::Opening)
            phase_ = Phase::Shown;
    }
    void beginClose() { phase_ = Phase::Closing; }
    void hide() { phase_ = Phase::Hidden; }

private:
    Phase phase_ = Phase::Hidden;
};
