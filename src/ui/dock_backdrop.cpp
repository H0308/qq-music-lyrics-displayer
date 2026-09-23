#include "dock_backdrop.h"

#include "logging/runtime_logger.h"

#include <DispatcherQueue.h>
#include <d2d1effects.h>
#include <dwmapi.h>
#include <windows.graphics.effects.interop.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <wrl.h>

#include <algorithm>
#include <cwchar>

namespace {

namespace abi_effects = ABI::Windows::Graphics::Effects;
namespace abi_composition_desktop = ABI::Windows::UI::Composition::Desktop;

constexpr float kMaxDockBlurAmount = 48.0f;
constexpr wchar_t kBlurProperty[] = L"DockGaussianBlur.BlurAmount";
constexpr GUID kGaussianBlurEffectId{
    0x1feb6d69, 0x2fe6, 0x4ac9, {0x8c, 0x58, 0x1d, 0x7f, 0x93, 0xe7, 0xa6, 0xa5}};

winrt::Windows::UI::Color compositionColor(COLORREF color) {
    winrt::Windows::UI::Color result{};
    result.A = 255;
    result.R = GetRValue(color);
    result.G = GetGValue(color);
    result.B = GetBValue(color);
    return result;
}

bool ensureDispatcherQueue() {
    if (winrt::Windows::System::DispatcherQueue::GetForCurrentThread())
        return true;

    static winrt::Windows::System::DispatcherQueueController controller{nullptr};
    if (!controller) {
        DispatcherQueueOptions options{};
        options.dwSize = static_cast<DWORD>(sizeof(options));
        options.threadType = DQTYPE_THREAD_CURRENT;
        options.apartmentType = DQTAT_COM_NONE;

        PDISPATCHERQUEUECONTROLLER rawController = nullptr;
        winrt::check_hresult(CreateDispatcherQueueController(options, &rawController));
        controller = winrt::Windows::System::DispatcherQueueController{
            rawController, winrt::take_ownership_from_abi};
    }
    return static_cast<bool>(controller);
}

class GaussianBlurEffect final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRtClassicComMix>,
          abi_effects::IGraphicsEffect,
          abi_effects::IGraphicsEffectSource,
          abi_effects::IGraphicsEffectD2D1Interop> {
    InspectableClass(L"QQMusicLyric.DockGaussianBlur", BaseTrust);

public:
    IFACEMETHODIMP get_Name(HSTRING* name) noexcept override {
        if (!name)
            return E_POINTER;
        return WindowsCreateString(L"DockGaussianBlur", 16, name);
    }

    IFACEMETHODIMP put_Name(HSTRING) noexcept override { return S_OK; }

    IFACEMETHODIMP GetEffectId(GUID* id) noexcept override {
        if (!id)
            return E_POINTER;
        *id = kGaussianBlurEffectId;
        return S_OK;
    }

    IFACEMETHODIMP GetNamedPropertyMapping(
        LPCWSTR name, UINT* index,
        abi_effects::GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) noexcept override {
        if (!name || !index || !mapping)
            return E_POINTER;
        if (wcscmp(name, L"BlurAmount") == 0)
            *index = D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION;
        else if (wcscmp(name, L"Optimization") == 0)
            *index = D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION;
        else if (wcscmp(name, L"BorderMode") == 0)
            *index = D2D1_GAUSSIANBLUR_PROP_BORDER_MODE;
        else
            return E_INVALIDARG;
        *mapping = abi_effects::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
        return S_OK;
    }

    IFACEMETHODIMP GetPropertyCount(UINT* count) noexcept override {
        if (!count)
            return E_POINTER;
        *count = 3;
        return S_OK;
    }

    IFACEMETHODIMP GetProperty(
        UINT index, ABI::Windows::Foundation::IPropertyValue** value) noexcept override {
        if (!value)
            return E_POINTER;
        *value = nullptr;
        try {
            winrt::Windows::Foundation::IInspectable propertyValue{nullptr};
            switch (index) {
            case D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION:
                propertyValue = winrt::Windows::Foundation::PropertyValue::CreateSingle(
                    blurAmount_);
                break;
            case D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION:
                propertyValue = winrt::Windows::Foundation::PropertyValue::CreateUInt32(
                    D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
                break;
            case D2D1_GAUSSIANBLUR_PROP_BORDER_MODE:
                propertyValue = winrt::Windows::Foundation::PropertyValue::CreateUInt32(
                    D2D1_BORDER_MODE_SOFT);
                break;
            default:
                return E_INVALIDARG;
            }
            auto typedPropertyValue =
                propertyValue.as<winrt::Windows::Foundation::IPropertyValue>();
            *value = reinterpret_cast<ABI::Windows::Foundation::IPropertyValue*>(
                winrt::detach_abi(typedPropertyValue));
            return S_OK;
        } catch (winrt::hresult_error const& error) {
            return error.code();
        } catch (...) {
            return E_FAIL;
        }
    }

    IFACEMETHODIMP GetSource(
        UINT index, abi_effects::IGraphicsEffectSource** source) noexcept override {
        if (!source)
            return E_POINTER;
        *source = nullptr;
        if (index != 0 || !source_)
            return E_INVALIDARG;
        return source_.CopyTo(source);
    }

    IFACEMETHODIMP GetSourceCount(UINT* count) noexcept override {
        if (!count)
            return E_POINTER;
        *count = 1;
        return S_OK;
    }

    void SetSource(abi_effects::IGraphicsEffectSource* source) noexcept {
        source_ = source;
    }

    void SetBlurAmount(float amount) noexcept { blurAmount_ = amount; }

private:
    Microsoft::WRL::ComPtr<abi_effects::IGraphicsEffectSource> source_;
    float blurAmount_ = 0.0f;
};

} // namespace

struct DockBackdrop::Impl {
    HWND hwnd = nullptr;
    bool dwmBackdropEnabled = false;
    bool isAvailable = false;
    bool isVisible = false;
    int adjustmentMode = 0;
    int blurPercent = 100;
    COLORREF solidColor = RGB(32, 32, 32);
    winrt::Windows::UI::Composition::Compositor compositor{nullptr};
    winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget target{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual visual{nullptr};
    winrt::Windows::UI::Composition::CompositionEffectBrush effectBrush{nullptr};
    winrt::Windows::UI::Composition::CompositionColorBrush solidBrush{nullptr};
    bool solidBrushActive = false;

    void update() {
        if (!isAvailable || !effectBrush || !visual)
            return;

        const int value = std::clamp(blurPercent, 0, 100);
        if (adjustmentMode == 1) {
            if (!solidBrushActive) {
                visual.Brush(solidBrush);
                solidBrushActive = true;
            }
            visual.Opacity(static_cast<float>(value) / 100.0f);
            visual.IsVisible(isVisible && value > 0);
            return;
        }

        if (solidBrushActive) {
            visual.Brush(effectBrush);
            solidBrushActive = false;
        }
        const float blurScale = static_cast<float>(value) / 100.0f;
        effectBrush.Properties().InsertScalar(
            kBlurProperty, kMaxDockBlurAmount * blurScale);
        visual.Opacity(static_cast<float>(value) / 100.0f);
        visual.IsVisible(isVisible && value > 0);
    }
};

DockBackdrop::DockBackdrop() : impl_(std::make_unique<Impl>()) {}

DockBackdrop::~DockBackdrop() { reset(); }

bool DockBackdrop::initialize(HWND hwnd) {
    reset();
    if (!hwnd || !IsWindow(hwnd))
        return false;

    impl_->hwnd = hwnd;
    const wchar_t* stage = L"compositor-create";
    try {
        const BOOL enableBackdrop = TRUE;
        const HRESULT backdropHr = DwmSetWindowAttribute(
            hwnd, DWMWA_USE_HOSTBACKDROPBRUSH, &enableBackdrop, sizeof(enableBackdrop));
        if (FAILED(backdropHr)) {
            runtime_log::writef(
                L"[dock-backdrop] init result=failed stage=host-backdrop hr=0x%08X",
                static_cast<unsigned>(backdropHr));
            reset();
            return false;
        }
        impl_->dwmBackdropEnabled = true;

        if (!ensureDispatcherQueue()) {
            runtime_log::writef(L"[dock-backdrop] init result=failed stage=dispatcher-queue");
            reset();
            return false;
        }

        impl_->compositor = winrt::Windows::UI::Composition::Compositor{};
        stage = L"desktop-interop";
        auto desktopInterop =
            impl_->compositor.as<abi_composition_desktop::ICompositorDesktopInterop>();
        ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget* rawTarget = nullptr;
        stage = L"desktop-target";
        winrt::check_hresult(
            desktopInterop->CreateDesktopWindowTarget(hwnd, FALSE, &rawTarget));
        impl_->target = winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget{
            rawTarget, winrt::take_ownership_from_abi};

        stage = L"backdrop-brush";
        auto backdrop = impl_->compositor.CreateBackdropBrush();
        winrt::Windows::UI::Composition::CompositionEffectSourceParameter sourceParameter{
            L"Backdrop"};
        auto effectSource = sourceParameter.as<winrt::Windows::Graphics::Effects::IGraphicsEffectSource>();

        stage = L"blur-effect";
        auto blur = Microsoft::WRL::Make<GaussianBlurEffect>();
        if (!blur)
            winrt::throw_hresult(E_OUTOFMEMORY);
        blur->SetSource(reinterpret_cast<abi_effects::IGraphicsEffectSource*>(
            winrt::get_abi(effectSource)));
        blur->SetBlurAmount(kMaxDockBlurAmount);

        Microsoft::WRL::ComPtr<abi_effects::IGraphicsEffect> rawEffect;
        winrt::check_hresult(blur.As(&rawEffect));
        winrt::Windows::Graphics::Effects::IGraphicsEffect graphicsEffect{
            rawEffect.Detach(), winrt::take_ownership_from_abi};
        stage = L"effect-factory";
        auto effectFactory = impl_->compositor.CreateEffectFactory(
            graphicsEffect, {winrt::hstring{kBlurProperty}});
        stage = L"effect-brush";
        impl_->effectBrush = effectFactory.CreateBrush();
        impl_->effectBrush.SetSourceParameter(L"Backdrop", backdrop);

        stage = L"solid-color-brush";
        impl_->solidBrush = impl_->compositor.CreateColorBrush(
            compositionColor(impl_->solidColor));

        stage = L"sprite-visual";
        impl_->visual = impl_->compositor.CreateSpriteVisual();
        impl_->visual.RelativeSizeAdjustment({1.0f, 1.0f});
        impl_->visual.Brush(impl_->effectBrush);
        stage = L"target-root";
        impl_->target.Root(impl_->visual);
        impl_->isAvailable = true;
        impl_->update();
        runtime_log::writef(L"[dock-backdrop] init result=ready hwnd=%p topmost=0",
                            hwnd);
        return true;
    } catch (winrt::hresult_error const& error) {
        const HRESULT hr = error.code();
        reset();
        runtime_log::writef(
            L"[dock-backdrop] init result=failed stage=%s hr=0x%08X",
            stage, static_cast<unsigned>(hr));
    } catch (...) {
        reset();
        runtime_log::writef(L"[dock-backdrop] init result=failed stage=%s hr=unknown",
                            stage);
    }
    return false;
}

void DockBackdrop::reset() noexcept {
    if (!impl_)
        return;
    try {
        if (impl_->target)
            impl_->target.Root(nullptr);
    } catch (...) {
    }
    impl_->visual = nullptr;
    impl_->effectBrush = nullptr;
    impl_->solidBrush = nullptr;
    impl_->solidBrushActive = false;
    impl_->target = nullptr;
    impl_->compositor = nullptr;
    if (impl_->hwnd && impl_->dwmBackdropEnabled && IsWindow(impl_->hwnd)) {
        const BOOL disableBackdrop = FALSE;
        DwmSetWindowAttribute(impl_->hwnd, DWMWA_USE_HOSTBACKDROPBRUSH,
                              &disableBackdrop, sizeof(disableBackdrop));
    }
    impl_->hwnd = nullptr;
    impl_->dwmBackdropEnabled = false;
    impl_->isAvailable = false;
    impl_->isVisible = false;
}

void DockBackdrop::setBlurPercent(int percent) noexcept {
    if (!impl_)
        return;
    impl_->blurPercent = std::clamp(percent, 0, 100);
    try {
        impl_->update();
    } catch (winrt::hresult_error const& error) {
        const HRESULT hr = error.code();
        runtime_log::writef(L"[dock-backdrop] update result=failed hr=0x%08X",
                            static_cast<unsigned>(hr));
        reset();
    } catch (...) {
        runtime_log::writef(L"[dock-backdrop] update result=failed hr=unknown");
        reset();
    }
}

void DockBackdrop::setAdjustmentMode(int mode) noexcept {
    if (!impl_)
        return;
    impl_->adjustmentMode = std::clamp(mode, 0, 1);
    setBlurPercent(impl_->blurPercent);
}

void DockBackdrop::setSolidColor(COLORREF color) noexcept {
    if (!impl_ || impl_->solidColor == color)
        return;
    impl_->solidColor = color;
    try {
        if (impl_->solidBrush)
            impl_->solidBrush.Color(compositionColor(color));
    } catch (winrt::hresult_error const& error) {
        const HRESULT hr = error.code();
        runtime_log::writef(L"[dock-backdrop] solid-color result=failed hr=0x%08X",
                            static_cast<unsigned>(hr));
        reset();
    } catch (...) {
        runtime_log::writef(L"[dock-backdrop] solid-color result=failed hr=unknown");
        reset();
    }
}

void DockBackdrop::setVisible(bool visible) noexcept {
    if (!impl_)
        return;
    impl_->isVisible = visible;
    try {
        impl_->update();
    } catch (winrt::hresult_error const& error) {
        const HRESULT hr = error.code();
        runtime_log::writef(L"[dock-backdrop] visibility result=failed hr=0x%08X",
                            static_cast<unsigned>(hr));
        reset();
    } catch (...) {
        runtime_log::writef(L"[dock-backdrop] visibility result=failed hr=unknown");
        reset();
    }
}

bool DockBackdrop::available() const noexcept {
    return impl_ && impl_->isAvailable;
}
