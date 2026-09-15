#pragma once

// 统一释放 Direct2D/DirectWrite 等 COM 接口，避免各组件重复维护相同的
// Release + 置空逻辑。
template <typename T>
inline void releaseCom(T*& value) noexcept {
    if (value) {
        value->Release();
        value = nullptr;
    }
}
