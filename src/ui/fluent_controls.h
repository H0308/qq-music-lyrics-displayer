#pragma once

#include "ui/lyric_renderer.h"
#include "ui/fluent_theme.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Win11 Fluent 风格的自绘控件，全部为 WS_EX_LAYERED 子窗口，
// 用 D2D 渲染并逐像素透明叠加在 Mica/亚克力背景上。
namespace fluent {

// 分层子窗口基座：窗口创建、D2D 帧管理（beginFrame/endFrame）
class LayeredChild {
public:
    ~LayeredChild();

    HWND hwnd() const { return hwnd_; }
    void move(int x, int y, int w, int h);

protected:
    // layered=false 时创建普通子窗口，帧用 BitBlt 提交（用于内含真控件的 FluentEdit：
    // 分层窗口的子控件不会被系统绘制）
    bool createLayered(HWND parent, const wchar_t* className, WNDPROC proc, int id,
                       bool layered = true);
    // 开始一帧：返回渲染目标（坐标单位为 DIP），wDip/hDip 输出客户区 DIP 尺寸
    ID2D1DCRenderTarget* beginFrame(float* wDip, float* hDip);
    void endFrame(); // EndDraw + UpdateLayeredWindow
    // 取/建单色画刷（随调色板 SetColor 复用）
    ID2D1SolidColorBrush* brush(ID2D1DCRenderTarget* rt);
    // 取/建文本格式
    IDWriteTextFormat* textFormat(float dipSize, int weight = 400, bool center = false);
    // DirectWrite 工厂（用于创建带省略号裁剪的 TextLayout）
    IDWriteFactory* dwrite();
    void renderNow(); // 立即重绘一帧（子类在 render() 中绘制）

    virtual void render(ID2D1DCRenderTarget* rt, float wDip, float hDip) = 0;

    HWND hwnd_ = nullptr;
    bool layered_ = true;
    // 分层窗口对 alpha=0 的像素做点击穿透。需要整面可点的控件把它设为 1/255：
    // 视觉上与全透明无异，但系统不再穿透
    float clearAlpha_ = 0.0f;

private:
    LyricRenderer renderer_;
    ID2D1SolidColorBrush* brush_ = nullptr;
    IDWriteTextFormat* fmt_ = nullptr;
    float fmtSize_ = 0;
    int fmtWeight_ = 0;
    bool fmtCenter_ = false;
};

// 圆角按钮（可强调色变体），点击向父窗口发送 WM_COMMAND/BN_CLICKED
class FluentButton : public LayeredChild {
public:
    bool create(HWND parent, int id, const wchar_t* text, bool accent = false);
    void setAccent(bool accent);
    void setEnabled(bool enabled);

private:
    void render(ID2D1DCRenderTarget* rt, float wDip, float hDip) override;
    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    int id_ = 0;
    std::wstring text_;
    bool accent_ = false;
    bool hover_ = false;
    bool pressed_ = false;
};

// 圆角输入框卡片，内嵌无边框真 EDIT（保留 IME/选中/剪贴板）。
// EDIT 通知（EN_CHANGE 等）原样转发给对话框父窗口。
class FluentEdit : public LayeredChild {
public:
    bool create(HWND parent, int id, const wchar_t* cueBanner);
    std::wstring text() const;
    void setText(const std::wstring& text);
    void focus();

private:
    void render(ID2D1DCRenderTarget* rt, float wDip, float hDip) override;
    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void layoutEdit();

    int id_ = 0;
    HWND hEdit_ = nullptr;
    HFONT editFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    COLORREF editBrushColor_ = 0;
    bool focused_ = false;
};

// 列表（分组标题、圆角选中、悬停高亮、滚轮/键盘/拖动滚动条）。
// 选中变化向父窗口发送 WM_COMMAND，HIWORD = LBN_SELCHANGE；双击发送 LBN_DBLCLK。
struct FluentListItem {
    std::wstring text;
    bool header = false; // 分组标题行：不可选
};

class FluentList : public LayeredChild {
public:
    // 自定义行绘制（如字体预览）；为空时按默认样式绘制文字
    using RowDrawFn = std::function<void(ID2D1DCRenderTarget*, const D2D1_RECT_F&, int row,
                                         bool selected, bool hovered)>;

    bool create(HWND parent, int id);
    void setItems(std::vector<FluentListItem> items);
    void clear();
    int selectedIndex() const { return selected_; }
    void setSelectedIndex(int idx);
    void setRowDraw(RowDrawFn fn) { rowDraw_ = std::move(fn); }
    int itemCount() const { return static_cast<int>(items_.size()); }

private:
    void render(ID2D1DCRenderTarget* rt, float wDip, float hDip) override;
    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    float rowHeight(int row) const;
    float contentHeight() const;
    int rowAt(float yDip) const;
    void ensureVisible(int row);
    void notifySelChange();
    int nextSelectable(int from, int dir) const;
    // 截断行的悬浮 tooltip（原生 tracking tooltip，仅文本被截断时弹出）
    bool rowTextTruncated(int row);
    void showTip(int row);
    void hideTip();

    int id_ = 0;
    std::vector<FluentListItem> items_;
    int selected_ = -1;
    int hover_ = -1;
    float scrollY_ = 0; // DIP
    bool scrollDrag_ = false;
    float scrollDragGrabDy_ = 0;
    int wheelAccum_ = 0;
    RowDrawFn rowDraw_;
    HWND tooltip_ = nullptr;
    int tipRow_ = -1;     // tooltip 正显示的行
    bool tipArmed_ = false; // 悬浮计时中（尚未弹出）
};

// 透明文本标签，直接绘制在 Mica 背景上
class FluentLabel : public LayeredChild {
public:
    bool create(HWND parent, int id, const wchar_t* text, bool secondary = false);
    void setText(const std::wstring& text);

private:
    void render(ID2D1DCRenderTarget* rt, float wDip, float hDip) override;
    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

    std::wstring text_;
    bool secondary_ = false;
};

} // namespace fluent
