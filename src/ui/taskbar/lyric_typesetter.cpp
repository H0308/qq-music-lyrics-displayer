#include "ui/taskbar/lyric_typesetter.h"
#include <algorithm>
#include <cwctype>
#include <utility>
bool LyricTypesetter::isCjkVerticalCharacter(wchar_t ch) {
    return (ch >= 0x2E80 && ch <= 0x2FFF) || (ch >= 0x3000 && ch <= 0x30FF) ||
           (ch >= 0x3400 && ch <= 0x9FFF) || (ch >= 0xAC00 && ch <= 0xD7AF);
}

bool LyricTypesetter::isLatinVerticalCharacter(wchar_t ch) {
    return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') ||
           (ch >= 0x00C0 && ch <= 0x024F) || (ch >= 0x1E00 && ch <= 0x1EFF) ||
           (ch >= 0xFF21 && ch <= 0xFF3A) || (ch >= 0xFF41 && ch <= 0xFF5A);
}

bool LyricTypesetter::isLatinRunStartCharacter(wchar_t ch) {
    return isLatinRunCharacter(ch) && !isLatinRunConnector(ch);
}

bool LyricTypesetter::isLatinRunCharacter(wchar_t ch) {
    return isLatinVerticalCharacter(ch) || (ch >= L'0' && ch <= L'9') ||
           (ch >= 0xFF10 && ch <= 0xFF19) || isLatinRunConnector(ch);
}

wchar_t LyricTypesetter::verticalPunctuationGlyph(wchar_t ch) {
    // 普通括号在逐字竖排时会保持“竖着的原字形”，改用 Unicode 竖排字形，
    // 让括号曲线横向展开。中英文及常见全角/中文成对括号统一处理。
    switch (ch) {
    case L'(':
    case L'（':
        return L'︵';
    case L')':
    case L'）':
        return L'︶';
    case L'[':
    case L'［':
        return L'﹇';
    case L']':
    case L'］':
        return L'﹈';
    case L'{':
    case L'｛':
        return L'︷';
    case L'}':
    case L'｝':
        return L'︸';
    case L'<':
    case L'〈':
    case L'＜':
        return L'︿';
    case L'>':
    case L'〉':
    case L'＞':
        return L'﹀';
    case L'〔':
        return L'︹';
    case L'〕':
        return L'︺';
    case L'【':
        return L'︻';
    case L'】':
        return L'︼';
    case L'《':
        return L'︽';
    case L'》':
        return L'︾';
    case L'「':
        return L'﹁';
    case L'」':
        return L'﹂';
    case L'『':
        return L'﹃';
    case L'』':
        return L'﹄';
    default:
        return ch;
    }
}

std::wstring LyricTypesetter::makeVerticalLyricText(const std::wstring& text,
                                                    bool topToBottom) const {
    std::vector<std::wstring> glyphs;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == L'\r' || text[i] == L'\n') {
            ++i;
            continue;
        }
        size_t units = 1;
        const wchar_t first = text[i];
        if (first >= 0xD800 && first <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 &&
            text[i + 1] <= 0xDFFF) {
            units = 2;
        }
        std::wstring glyph = text.substr(i, units);
        if (units == 1)
            glyph[0] = verticalPunctuationGlyph(glyph[0]);
        glyphs.emplace_back(std::move(glyph));
        i += units;
    }
    if (!topToBottom)
        std::reverse(glyphs.begin(), glyphs.end());

    std::wstring result;
    for (const auto& glyph : glyphs) {
        if (!result.empty())
            result.push_back(L'\n');
        result += glyph;
    }
    return result;
}

bool LyricTypesetter::useRotatedVerticalLyric(const std::wstring& text) const {
    bool hasLatin = false;
    for (size_t i = 0; i < text.size();) {
        const wchar_t c = text[i];
        if (c == L'\r' || c == L'\n') {
            ++i;
            continue;
        }
        if (c >= 0xD800 && c <= 0xDFFF) {
            // 代理项和 CJK 之外的复杂文字仍走逐字布局，避免旋转后破坏字形组合。
            if (c <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 &&
                text[i + 1] <= 0xDFFF)
                i += 2;
            else
                ++i;
            continue;
        }

        if (isCjkVerticalCharacter(c))
            return false;
        hasLatin = hasLatin || isLatinVerticalCharacter(c);
        ++i;
    }
    return hasLatin;
}

bool LyricTypesetter::createVerticalLyricLayout(const std::wstring& text, float layoutWidth,
                                                IDWriteTextLayout** out, float& width,
                                                float& height, bool& rotated) {
    width = 0.0f;
    height = 0.0f;
    rotated = false;
    if (!out)
        return false;
    *out = nullptr;
    if (!factory_ || !resources_.fmtLyric_.Get() || text.empty() || layoutWidth <= 0.0f)
        return false;

    const bool rotateText = useRotatedVerticalLyric(text);
    // 竖向英文统一使用截图中的顺时针 90° 角度；原文本顺序无需反转，
    // 这样视觉上仍按原句从上向下阅读。
    const std::wstring layoutText = rotateText ? text : makeVerticalLyricText(text, true);
    if (layoutText.empty() ||
        FAILED(factory_->CreateTextLayout(
            layoutText.c_str(), static_cast<UINT32>(layoutText.size()), resources_.fmtLyric_.Get(),
            rotateText ? 100000.0f : layoutWidth, 100000.0f, out)) ||
        !*out) {
        return false;
    }

    (*out)->SetTextAlignment(rotateText ? DWRITE_TEXT_ALIGNMENT_LEADING
                                        : DWRITE_TEXT_ALIGNMENT_CENTER);
    (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED((*out)->GetMetrics(&metrics))) {
        (*out)->Release();
        *out = nullptr;
        return false;
    }
    if (rotateText) {
        // 横向整句旋转 90° 后，原文本高度成为竖栏宽度，原文本宽度成为滚动长度。
        width = metrics.height;
        height = metrics.width;
        rotated = true;
    } else {
        // metrics.width 是字符墨宽，而不是布局槽宽；绘制/命中需要保留完整的
        // 任务栏横向槽宽，才能让每个字在窄栏中保持居中。
        width = layoutWidth;
        height = metrics.height;
    }
    return height > 0.0f;
}

void LyricTypesetter::releaseVerticalLyricParts(std::vector<VerticalLyricPart>& parts) {
    parts.clear();
}

bool LyricTypesetter::hasMixedVerticalLyric(const std::wstring& text) const {
    bool hasLatin = false;
    bool hasCjk = false;
    for (size_t i = 0; i < text.size();) {
        const wchar_t c = text[i];
        if (c == L'\r' || c == L'\n') {
            ++i;
            continue;
        }
        if (c >= 0xD800 && c <= 0xDFFF) {
            if (c <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 &&
                text[i + 1] <= 0xDFFF)
                i += 2;
            else
                ++i;
            continue;
        }
        hasLatin = hasLatin || isLatinVerticalCharacter(c);
        hasCjk = hasCjk || isCjkVerticalCharacter(c);
        if (hasLatin && hasCjk)
            return true;
        ++i;
    }
    return false;
}

bool LyricTypesetter::createMixedVerticalLyricLayout(const std::wstring& text, float layoutWidth,
                                                     std::vector<VerticalLyricPart>& parts,
                                                     float& width, float& height) {
    releaseVerticalLyricParts(parts);
    width = layoutWidth;
    height = 0.0f;
    if (!factory_ || !resources_.fmtLyric_.Get() || text.empty() || layoutWidth <= 0.0f)
        return false;

    IDWriteFactory* dwrite = factory_;
    auto appendPart = [&](const std::wstring& partText, bool rotated) {
        if (partText.empty())
            return true;

        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        const HRESULT hr = dwrite->CreateTextLayout(
            partText.c_str(), static_cast<UINT32>(partText.size()), resources_.fmtLyric_.Get(),
            rotated ? 100000.0f : layoutWidth, 100000.0f, layout.GetAddressOf());
        if (FAILED(hr) || !layout) {
            if (layout)
                layout.Reset();
            return false;
        }
        layout->SetTextAlignment(rotated ? DWRITE_TEXT_ALIGNMENT_LEADING
                                         : DWRITE_TEXT_ALIGNMENT_CENTER);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(layout->GetMetrics(&metrics))) {
            layout.Reset();
            return false;
        }
        const float partWidth = rotated ? metrics.height : layoutWidth;
        const float partHeight = rotated ? metrics.width : metrics.height;
        if (partHeight <= 0.0f) {
            layout.Reset();
            return true;
        }
        parts.push_back({layout, partWidth, partHeight, rotated});
        height += partHeight;
        return true;
    };

    bool created = true;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == L'\r' || text[i] == L'\n') {
            ++i;
            continue;
        }

        if (isLatinRunStartCharacter(text[i])) {
            const size_t start = i;
            size_t end = i;
            while (end < text.size()) {
                if (text[end] == L'\r' || text[end] == L'\n')
                    break;
                if (text[end] >= 0xD800 && text[end] <= 0xDFFF)
                    break;
                if (!isLatinRunCharacter(text[end]))
                    break;
                ++end;
            }
            size_t partEnd = end;
            while (partEnd > start && (text[partEnd - 1] == L' ' || text[partEnd - 1] == L'\t'))
                --partEnd;
            if (partEnd > start && !appendPart(text.substr(start, partEnd - start), true)) {
                created = false;
                break;
            }
            i = end;
            continue;
        }

        size_t units = 1;
        const wchar_t first = text[i];
        if (first >= 0xD800 && first <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 &&
            text[i + 1] <= 0xDFFF) {
            units = 2;
        }
        std::wstring glyph = text.substr(i, units);
        if (units == 1)
            glyph[0] = verticalPunctuationGlyph(glyph[0]);
        if (!appendPart(glyph, false)) {
            created = false;
            break;
        }
        i += units;
    }

    if (!created || parts.empty() || height <= 0.0f) {
        releaseVerticalLyricParts(parts);
        width = 0.0f;
        height = 0.0f;
        return false;
    }
    return true;
}

bool LyricTypesetter::isLatinRunConnector(wchar_t ch) {
    switch (ch) {
    case L' ':
    case L'\t':
    case L'\'':
    case L'’':
    case L'-':
    case L'‐':
    case L'‑':
    case L'_':
    case L'&':
    case L'+':
    case L'/':
    case L'.':
    case L',':
    case L'!':
    case L'?':
    case L':':
    case L';':
    case L'%':
    case L'#':
    case L'@':
    case L'=':
    case L'(':
    case L')':
    case L'[':
    case L']':
    case L'{':
    case L'}':
        return true;
    default:
        return false;
    }
}

void LyricTypesetter::releaseOutgoing() {
    resources_.outgoingLyricLayout_.Reset();
    resources_.outgoingVerticalLyricLayout_.Reset();
    resources_.outgoingVerticalLyricParts_.clear();
    resources_.outgoingSecondaryLayout_.Reset();
    resources_.outgoingNextLyricLayout_.Reset();
}
void LyricTypesetter::retainCurrentForTransition(bool doubleLine) {
    releaseOutgoing();
    resources_.outgoingLyricLayout_ = std::move(resources_.lyricLayout_);
    resources_.outgoingVerticalLyricLayout_ = std::move(resources_.verticalLyricLayout_);
    resources_.outgoingVerticalLyricParts_.swap(resources_.verticalLyricParts_);
    if (doubleLine) {
        resources_.outgoingNextLyricLayout_ = std::move(resources_.nextLyricLayout_);
        resources_.secondaryLayout_.Reset();
    } else {
        resources_.nextLyricLayout_.Reset();
        resources_.outgoingSecondaryLayout_ = std::move(resources_.secondaryLayout_);
    }
}

LyricTextMetrics LyricTypesetter::buildContent(const LyricTextInput& input) {
    LyricTextMetrics metrics;
    resources_.lyricLayout_.Reset();
    resources_.verticalLyricLayout_.Reset();
    resources_.verticalLyricParts_.clear();
    resources_.secondaryLayout_.Reset();
    resources_.nextLyricLayout_.Reset();
    if (!factory_)
        return metrics;
    const auto create = [&](const std::wstring& text, IDWriteTextFormat* format,
                            Microsoft::WRL::ComPtr<IDWriteTextLayout>& layout, float& width,
                            float& height) {
        if (text.empty() || !format)
            return;
        factory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                   100000.0f, 100.0f, layout.ReleaseAndGetAddressOf());
        if (layout) {
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TEXT_METRICS value{};
            layout->GetMetrics(&value);
            width = value.width;
            height = value.height;
        }
    };
    create(input.lyric, resources_.fmtLyric_.Get(), resources_.lyricLayout_, metrics.width,
           metrics.height);
    if (input.vertical && !input.lyric.empty()) {
        if (hasMixedVerticalLyric(input.lyric))
            createMixedVerticalLyricLayout(input.lyric, input.verticalWidth,
                                           resources_.verticalLyricParts_, metrics.verticalWidth,
                                           metrics.verticalHeight);
        else
            createVerticalLyricLayout(input.lyric, input.verticalWidth,
                                      resources_.verticalLyricLayout_.ReleaseAndGetAddressOf(),
                                      metrics.verticalWidth, metrics.verticalHeight,
                                      metrics.verticalRotated);
    }
    if (!input.doubleLine)
        create(input.secondary, resources_.fmtSecondary_.Get(), resources_.secondaryLayout_,
               metrics.secondaryWidth, metrics.secondaryHeight);
    else
        create(input.next, resources_.fmtNextLyric_.Get(), resources_.nextLyricLayout_,
               metrics.nextWidth, metrics.nextHeight);
    return metrics;
}
