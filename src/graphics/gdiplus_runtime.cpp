#include "graphics/gdiplus_runtime.h"

namespace graphics {
namespace {
class GdiplusRuntime {
public:
    GdiplusRuntime() {
        Gdiplus::GdiplusStartupInput input;
        status_ = Gdiplus::GdiplusStartup(&token_, &input, nullptr);
    }
    ~GdiplusRuntime() {
        if (status_ == Gdiplus::Ok)
            Gdiplus::GdiplusShutdown(token_);
    }
    GdiplusRuntime(const GdiplusRuntime&) = delete;
    GdiplusRuntime& operator=(const GdiplusRuntime&) = delete;
    Gdiplus::Status status() const { return status_; }
private:
    ULONG_PTR token_ = 0;
    Gdiplus::Status status_ = Gdiplus::GenericError;
};
} // namespace

Gdiplus::Status gdiplusStatus() {
    static const GdiplusRuntime runtime;
    return runtime.status();
}
} // namespace graphics
