#pragma once
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include <vector>
struct VerticalLyricPart {
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    float width = 0.0f;
    float height = 0.0f;
    bool rotated = false;
};

struct LyricTextInput {
    std::wstring lyric;
    std::wstring secondary;
    std::wstring next;
    bool doubleLine = false;
    bool vertical = false;
    float verticalWidth = 0.0f;
};
struct LyricTextMetrics {
    float width = 0, height = 0;
    float verticalWidth = 0, verticalHeight = 0;
    bool verticalRotated = false;
    float secondaryWidth = 0, secondaryHeight = 0;
    float nextWidth = 0, nextHeight = 0;
};

// 当前行和离场行均由排版组件持有，转场/特效只借用 Layout 指针。
class LyricTypesetter {
public:
    struct Resources {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtTitle_;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtArtist_;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtLyric_;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtNextLyric_;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtSecondary_;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtDragPreview_;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtClockTime_;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtClockDate_;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtResource_;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> titleLayout_;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> artistLayout_;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> lyricLayout_;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> verticalLyricLayout_;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> nextLyricLayout_;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> secondaryLayout_;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> outgoingLyricLayout_;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> outgoingVerticalLyricLayout_;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> outgoingSecondaryLayout_;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> outgoingNextLyricLayout_;
        std::vector<VerticalLyricPart> verticalLyricParts_;
        std::vector<VerticalLyricPart> outgoingVerticalLyricParts_;
    };
    // 协调层在重建或移交旧行时使用写入端；绘制组件接收只读快照。
    Resources& resources() { return resources_; }
    const Resources& resources() const { return resources_; }
    LyricTextMetrics buildContent(const LyricTextInput& input);
    void retainCurrentForTransition(bool doubleLine);
    void releaseOutgoing();
    void setFactory(IDWriteFactory* factory) { factory_ = factory; }
    void discardDeviceResources() {
        resources_ = {};
        factory_ = nullptr;
    }
    static bool isCjkVerticalCharacter(wchar_t ch);
    static bool isLatinVerticalCharacter(wchar_t ch);
    static bool isLatinRunConnector(wchar_t ch);
    static bool isLatinRunStartCharacter(wchar_t ch);
    static bool isLatinRunCharacter(wchar_t ch);
    static wchar_t verticalPunctuationGlyph(wchar_t ch);
    std::wstring makeVerticalLyricText(const std::wstring& text, bool topToBottom) const;
    bool useRotatedVerticalLyric(const std::wstring& text) const;
    bool createVerticalLyricLayout(const std::wstring& text, float layoutWidth,
                                   IDWriteTextLayout** out, float& width, float& height,
                                   bool& rotated);
    void releaseVerticalLyricParts(std::vector<VerticalLyricPart>& parts);
    bool hasMixedVerticalLyric(const std::wstring& text) const;
    bool createMixedVerticalLyricLayout(const std::wstring& text, float layoutWidth,
                                        std::vector<VerticalLyricPart>& parts, float& width,
                                        float& height);

private:
    IDWriteFactory* factory_ = nullptr; // renderer 持有，仅排版调用期间借用
    Resources resources_;
};
