#include "test_support.h"

#include "app/presentation_model.h"
#include "graphics/pixel_luminance.h"
#include "media/playback_controller.h"
#include "media/track_identity.h"
#include "settings/app_settings.h"
#include "util/base64.h"

namespace {
class MessageOnlyTimerWindow {
public:
    MessageOnlyTimerWindow() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"QQMusicLyricTestTimerWindow";
        RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                wc.hInstance, nullptr);
    }
    ~MessageOnlyTimerWindow() {
        if (hwnd_)
            DestroyWindow(hwnd_);
    }
    HWND hwnd() const { return hwnd_; }

private:
    HWND hwnd_ = nullptr;
};
} // namespace

TEST_CASE("track_identity.qq_stale_timeline_and_netease_song_id") {
    SmtcSnapshot qqSnap;
    qqSnap.sessionAlive = true;
    qqSnap.player = SmtcPlayerType::QQMusic;
    qqSnap.title = L"月牙湾";
    qqSnap.artist = L"F.I.R.";
    qqSnap.durationMs = 232012;
    qqSnap.timelineStale = true; // 切歌残留时间线不参与曲目身份计算

    const std::wstring staleKey = makeTrackKey(qqSnap);
    EXPECT_EQ(staleKey, std::wstring(L"qq|月牙湾|F.I.R.|-1"));

    qqSnap.timelineStale = false;
    qqSnap.durationMs = 305991;
    const std::wstring validKey = makeTrackKey(qqSnap);
    EXPECT_EQ(validKey, std::wstring(L"qq|月牙湾|F.I.R.|61"));

    // SMTC Changing/Opened 过渡期标题暂时为空时保留当前会话匹配
    SmtcSnapshot transitionSnap = qqSnap;
    transitionSnap.title.clear();
    EXPECT_TRUE(snapshotMatchesTrackKey(transitionSnap, validKey));

    SmtcSnapshot neteaseSnap;
    neteaseSnap.sessionAlive = true;
    neteaseSnap.player = SmtcPlayerType::NetEase;
    neteaseSnap.neteaseSongId = L"1974443814";
    neteaseSnap.title = L"demo";
    EXPECT_EQ(makeTrackKey(neteaseSnap), std::wstring(L"netease|1974443814"));
}

TEST_CASE("presentation_model.scene_resolution_lead_time_and_transition_retention") {
    PresentationModel model;
    AppSettings settings;
    settings.idleEntryEnabled_ = true;
    PlaybackState playback;
    playback.currentKey = L"qq|月牙湾|F.I.R.|61";
    playback.currentTitle = L"月牙湾";
    playback.currentArtist = L"F.I.R.";
    playback.currentLyrics_ = {
        {1000, L"第一句"},
        {2000, L"第二句"},
    };

    SmtcSnapshot snap;
    snap.sessionAlive = true;
    snap.player = SmtcPlayerType::QQMusic;
    snap.title = L"月牙湾";
    snap.artist = L"F.I.R.";
    snap.album = L"爱‧歌姬";
    snap.durationMs = 305991;
    snap.positionMs = 1920; // +100ms 预读后达到 2020ms，应选中第二句（index=1）
    snap.status = PlaybackStatus::Playing;

    IdleFrameInput idle;
    const auto& frame1 = model.update(snap, settings, playback, idle,
                                      {.animateTransition = true, .lyricsChanged = true});
    EXPECT_EQ(frame1.scene, DisplayScene::Lyrics);
    EXPECT_EQ(frame1.currentLine, 1);
    EXPECT_EQ(frame1.media.artist, std::wstring(L"F.I.R. · 爱‧歌姬"));

    // SMTC 过渡事件标题与歌手暂时为空时，保留上一帧的媒体标题与歌手，防止闪空
    SmtcSnapshot transient = snap;
    transient.title.clear();
    transient.artist.clear();
    transient.album.clear();
    const auto& frame2 = model.update(transient, settings, playback, idle, {});
    EXPECT_EQ(frame2.media.title, std::wstring(L"月牙湾"));
    EXPECT_EQ(frame2.media.artist, std::wstring(L"F.I.R. · 爱‧歌姬"));

    // 高频播放进度补丁与专辑主题色更新
    snap.positionMs = 500;
    const PlaybackPatch patch = model.updatePlayback(snap);
    EXPECT_EQ(patch.currentLine, -1);
    EXPECT_EQ(patch.lineSelectionPositionMs, 600);

    const auto& colored = model.updateAlbumColor(RGB(120, 104, 88));
    EXPECT_TRUE(colored.media.hasDominantColor);
    EXPECT_EQ(colored.media.dominantColor, RGB(120, 104, 88));
}

TEST_CASE("playback_controller.stale_payload_rejection_and_duration_completion") {
    MessageOnlyTimerWindow timerWindow;
    SmtcSnapshot currentSnap;
    currentSnap.sessionAlive = true;
    currentSnap.player = SmtcPlayerType::QQMusic;
    currentSnap.sourceAppUserModelId = L"QQMusic.exe";
    currentSnap.title = L"歌曲A";
    currentSnap.artist = L"歌手A";
    currentSnap.durationMs = 240000;
    currentSnap.timelineStale = true; // 首次到达时时间线仍是上一首残留
    auto thumbA = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{1, 2, 3, 4});
    currentSnap.thumbnail = thumbA;

    int publishCount = 0;
    int coverChangedCount = 0;
    PlaybackController controller(
        [&] { return currentSnap; },
        [&](const SmtcSnapshot&, PresentationUpdate) { ++publishCount; },
        [&](const SmtcSnapshot&) {},
        [&] { ++coverChangedCount; });
    controller.setTimerWindow(timerWindow.hwnd());

    // 1. 歌曲 A 首次以残留时间线进入（duration 视为 0），随后补上真实时长
    const TrackChange firstChange = controller.updateTrack(currentSnap, L"QQMusic.exe");
    EXPECT_TRUE(firstChange.trackChanged);
    EXPECT_FALSE(firstChange.durationOnly);
    EXPECT_TRUE(firstChange.songIdentityChanged);
    EXPECT_EQ(controller.state().currentKey, std::wstring(L"qq|歌曲A|歌手A|-1"));
    EXPECT_EQ(controller.state().lastCover_, thumbA);

    currentSnap.timelineStale = false;
    currentSnap.durationMs = 200000;
    const TrackChange durationCompletion = controller.updateTrack(currentSnap, L"QQMusic.exe");
    EXPECT_TRUE(durationCompletion.trackChanged);
    EXPECT_TRUE(durationCompletion.durationOnly);
    EXPECT_FALSE(durationCompletion.songIdentityChanged);
    EXPECT_EQ(controller.state().currentKey, std::wstring(L"qq|歌曲A|歌手A|40"));
    EXPECT_EQ(controller.state().lastCover_, thumbA); // 同曲时长补齐不清空已有封面

    // 2. 手动应用歌词会递增 requestGeneration_，使旧代次的异步歌词结果失效
    const uint64_t staleGeneration = controller.state().requestGeneration_;
    LyricResult manualResult;
    manualResult.ok = true;
    manualResult.fromManual = true;
    manualResult.lines = {{1000, L"手动歌词行", {}, L"翻译行", {}}};
    controller.applyManualLyrics(manualResult, currentSnap);
    EXPECT_TRUE(controller.state().requestGeneration_ > staleGeneration);
    EXPECT_TRUE(controller.state().currentLyricsFromManual_);
    EXPECT_TRUE(controller.state().currentHasTranslation_);
    EXPECT_FALSE(controller.state().currentHasRomanization_);

    auto staleSameTrackPayload = std::make_unique<LyricPayload>();
    staleSameTrackPayload->key = controller.state().currentKey;
    staleSameTrackPayload->requestGeneration = staleGeneration;
    staleSameTrackPayload->result.ok = true;
    staleSameTrackPayload->result.lines = {{1000, L"过期的自动歌词"}};
    controller.onLyricReady(std::move(staleSameTrackPayload));
    EXPECT_EQ(controller.state().currentLyrics_[0].text, std::wstring(L"手动歌词行"));

    // 3. 切换到歌曲 B 后，歌曲 A 的迟到歌词与封面结果必须被丢弃
    const std::wstring keyA = controller.state().currentKey;
    const uint64_t genBeforeSwitch = controller.state().requestGeneration_;
    currentSnap.title = L"歌曲B";
    currentSnap.artist = L"歌手B";
    currentSnap.durationMs = 180000;
    currentSnap.thumbnail.reset();
    controller.updateTrack(currentSnap, L"QQMusic.exe");
    controller.cancelLyricDebounce();
    EXPECT_EQ(controller.state().currentKey, std::wstring(L"qq|歌曲B|歌手B|36"));

    auto lateLyricA = std::make_unique<LyricPayload>();
    lateLyricA->key = keyA;
    lateLyricA->requestGeneration = genBeforeSwitch;
    lateLyricA->result.ok = true;
    lateLyricA->result.lines = {{1000, L"歌曲A的迟到歌词"}};
    controller.onLyricReady(std::move(lateLyricA));
    EXPECT_TRUE(controller.state().currentLyrics_.empty());

    auto lateCoverA = std::make_unique<CoverPayload>();
    lateCoverA->key = keyA;
    lateCoverA->requestGeneration = genBeforeSwitch;
    lateCoverA->source = CoverSource::QqApi;
    lateCoverA->cover = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{9, 9});
    controller.onCoverReady(std::move(lateCoverA));
    EXPECT_TRUE(controller.state().lastCover_ == nullptr);

    // 4. 当前曲目 B 的有效代次封面正常接收；但若 SMTC 已提供缩略图，API 封面不得覆盖 SMTC
    auto validCoverB = std::make_unique<CoverPayload>();
    validCoverB->key = controller.state().currentKey;
    validCoverB->requestGeneration = controller.state().requestGeneration_;
    validCoverB->source = CoverSource::QqApi;
    auto apiCoverBytes = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{7, 8});
    validCoverB->cover = apiCoverBytes;
    controller.onCoverReady(std::move(validCoverB));
    EXPECT_EQ(controller.state().lastCover_, apiCoverBytes);
    EXPECT_EQ(controller.state().lastCoverSource_, CoverSource::QqApi);
}

TEST_CASE("utility_logic.base64_and_backdrop_luminance") {
    EXPECT_EQ(base64Decode("5pyI54mZ5rm+"), std::string("\xE6\x9C\x88\xE7\x89\x99\xE6\xB9\xBE"));
    EXPECT_EQ(base64Decode("aGVs\r\nbG8="), std::string("hello"));
    EXPECT_TRUE(base64Decode("").empty());

    EXPECT_NEAR(graphics::backdropLuminance(nullptr, 10, 10), 0.0, 1e-6);
    const uint8_t blackPixels[16] = {};
    EXPECT_NEAR(graphics::backdropLuminance(blackPixels, 2, 2), 0.0, 1e-6);
    const uint8_t whitePixels[16] = {
        255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255,
    };
    EXPECT_NEAR(graphics::backdropLuminance(whitePixels, 2, 2), 1.0, 1e-4);
}
