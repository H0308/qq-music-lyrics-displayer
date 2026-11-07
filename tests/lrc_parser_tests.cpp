#include "test_support.h"

#include "lyric/lrc_parser.h"
#include "lyric/lyric_provider.h"
#include "lyric/qrc_decoder.h"
#include "ui/taskbar/word_wave_renderer.h"

TEST_CASE("lrc_parser.multi_tag_sorting_and_metadata_filtering") {
    const auto lines = parseLrcText("[00:02.00] later\r\n"
                                   "[ti:demo]\n"
                                   "[00:01.20][00:03.00] first  \n"
                                   "[00:04.00]   \n");

    EXPECT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0].ms, 1200);
    EXPECT_EQ(lines[0].text, std::wstring(L"first"));
    EXPECT_EQ(lines[1].ms, 2000);
    EXPECT_EQ(lines[1].text, std::wstring(L"later"));
    EXPECT_EQ(lines[2].ms, 3000);
    EXPECT_EQ(lines[2].text, std::wstring(L"first"));
    EXPECT_TRUE(parseLrcText("[ar:artist]\n").empty());
}

TEST_CASE("lrc_parser.timestamp_boundaries_and_utf8_text") {
    int64_t ms = -1;
    EXPECT_TRUE(parseLrcTimestamp("01:02.345", ms));
    EXPECT_EQ(ms, 62345);
    EXPECT_TRUE(parseLrcTimestamp("00:00.00", ms));
    EXPECT_EQ(ms, 0);
    EXPECT_FALSE(parseLrcTimestamp(":02.00", ms));
    EXPECT_FALSE(parseLrcTimestamp("ab:02.00", ms));
    EXPECT_FALSE(parseLrcTimestamp("01:", ms));

    const auto lines = parseLrcText("[00:05.50] \t 晴天 - 周杰伦 \t \r\n"
                                    "[invalid] ignored\n"
                                    "[00:01.00unclosed\n");
    EXPECT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].ms, 5500);
    EXPECT_EQ(lines[0].text, std::wstring(L"晴天 - 周杰伦"));
}

TEST_CASE("qrc_decoder.parse_unwrapped_qrc_text_and_zero_duration_clamp") {
    std::vector<LyricLine> out;
    const std::string qrc =
        "[ver:v1]\r\n"
        "[2000,1000]后(2000,400)行(2400,600)\r\n"
        "[1000,800]前(1000,0)字(1300,500)\n"
        "[3000,500](3000,200)\n";
    EXPECT_TRUE(parseQrcLyricsText(qrc, out));
    EXPECT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].ms, 1000);
    EXPECT_EQ(out[0].text, std::wstring(L"前字"));
    EXPECT_EQ(out[0].chars.size(), 2u);
    EXPECT_EQ(out[0].chars[0].startMs, 1000);
    EXPECT_EQ(out[0].chars[0].endMs, 1001); // 0ms 持续时间至少钳制为 1ms
    EXPECT_EQ(out[0].chars[1].startMs, 1300);
    EXPECT_EQ(out[0].chars[1].endMs, 1800);
    EXPECT_EQ(out[1].ms, 2000);
    EXPECT_EQ(out[1].text, std::wstring(L"后行"));
}

TEST_CASE("lyric_provider.make_key_and_find_line_boundaries") {
    EXPECT_EQ(LyricProvider::makeKey(L"月牙湾", L"F.I.R.", 0), std::wstring(L"月牙湾|F.I.R.|-1"));
    EXPECT_EQ(LyricProvider::makeKey(L"月牙湾", L"F.I.R.", -500), std::wstring(L"月牙湾|F.I.R.|-1"));
    EXPECT_EQ(LyricProvider::makeKey(L"月牙湾", L"F.I.R.", 230100),
              LyricProvider::makeKey(L"月牙湾", L"F.I.R.", 234999));
    EXPECT_NE(LyricProvider::makeKey(L"月牙湾", L"F.I.R.", 234999),
              LyricProvider::makeKey(L"月牙湾", L"F.I.R.", 235000));

    const std::vector<LyricLine> empty;
    EXPECT_EQ(LyricProvider::findLine(empty, 0), -1);

    const std::vector<LyricLine> lines{
        {1000, L"line0"},
        {2500, L"line1"},
        {2500, L"line2_duplicate_timestamp"},
        {4000, L"line3"},
    };
    EXPECT_EQ(LyricProvider::findLine(lines, 999), -1);
    EXPECT_EQ(LyricProvider::findLine(lines, 1000), 0);
    EXPECT_EQ(LyricProvider::findLine(lines, 2499), 0);
    EXPECT_EQ(LyricProvider::findLine(lines, 2500), 2);
    EXPECT_EQ(LyricProvider::findLine(lines, 3999), 2);
    EXPECT_EQ(LyricProvider::findLine(lines, 4000), 3);
    EXPECT_EQ(LyricProvider::findLine(lines, 99999), 3);
}

TEST_CASE("word_wave_renderer.timing_and_offset_curves") {
    LyricLine line;
    line.ms = 1000;
    line.text = L"听见";
    line.chars = {
        {1000, 1400, L"听"},
        {1400, 1800, L"见"},
    };

    EXPECT_EQ(WordWaveRenderer::endTime(line), 1800);
    EXPECT_FALSE(WordWaveRenderer::animating(line, 999));
    EXPECT_TRUE(WordWaveRenderer::animating(line, 1000));
    EXPECT_TRUE(WordWaveRenderer::animating(line, 2200)); // 尾波阶段仍在动画中
    EXPECT_FALSE(WordWaveRenderer::animating(line, 3000));

    EXPECT_NEAR(WordWaveRenderer::offset(line.chars[0], line.ms, 999, 16.0f), 0.0, 1e-5);
    const float activeOffset = WordWaveRenderer::offset(line.chars[0], line.ms, 1200, 16.0f);
    EXPECT_TRUE(activeOffset > 0.0f);
    EXPECT_NEAR(WordWaveRenderer::offset(line.chars[0], line.ms, 3000, 16.0f), 0.0, 1e-4);
    EXPECT_NEAR(WordWaveRenderer::lineOffset(1000, 1800, 950, 16.0f, 0.5f), 0.0, 1e-5);
}
