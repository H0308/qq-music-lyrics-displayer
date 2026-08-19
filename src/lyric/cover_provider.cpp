#include "cover_provider.h"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <windows.h>

namespace {

constexpr char kUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/126.0.0.0 Safari/537.36";
constexpr char kReferer[] = "https://y.qq.com/";

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

size_t curlWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::vector<uint8_t>*>(userdata);
    size_t total = size * nmemb;
    size_t oldSize = out->size();
    out->resize(oldSize + total);
    std::memcpy(out->data() + oldSize, ptr, total);
    return total;
}

// 进程退出标志：CoverProvider 析构前置位，curl 进度回调据此立即中断在途下载，
// 避免退出时主线程被 join 堵在最长 10s 的超时上
std::atomic<bool> g_shutdown{false};

int curlXferAbort(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return g_shutdown.load() ? 1 : 0; // 返回非 0：curl 以 CURLE_ABORTED_BY_CALLBACK 立即返回
}

} // namespace

struct CoverProvider::Impl {
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
    };

    std::atomic<uint64_t> generation{0};
    std::mutex mtx;
    std::vector<Worker> workers;

    ~Impl() {
        // 先置退出标志：在途 curl 下载经进度回调立即中断
        g_shutdown.store(true);
        for (auto& w : workers)
            if (w.thread.joinable()) w.thread.join();
    }

    // 回收已结束的线程：在每次新请求入口顺手清理，向量规模稳定在并发中的请求数
    void sweepFinished() {
        std::erase_if(workers, [](Worker& w) {
            if (w.done->load()) {
                w.thread.join();
                return true;
            }
            return false;
        });
    }
};

CoverProvider::CoverProvider() : impl_(std::make_unique<Impl>()) {
    g_shutdown.store(false); // 防御性复位：退出流程只会置位一次
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

CoverProvider::~CoverProvider() = default;

void CoverProvider::requestAsync(const std::wstring& albummid, ReadyCallback cb) {
    if (albummid.empty()) {
        if (cb) cb(nullptr);
        return;
    }

    uint64_t gen = ++impl_->generation;
    Impl* impl = impl_.get();
    Impl::Worker worker;
    auto done = worker.done;
    worker.thread = std::thread([impl, gen, albummid, cb = std::move(cb), done]() mutable {
        // 任何 return 路径都置位，保证 sweepFinished 能回收本线程
        struct Flag {
            std::shared_ptr<std::atomic<bool>> d;
            ~Flag() { d->store(true); }
        } flag{done};

        CURL* curl = curl_easy_init();
        if (!curl) {
            if (cb) cb(nullptr);
            return;
        }

        std::ostringstream url;
        url << "https://y.gtimg.cn/music/photo_new/T002R300x300M000"
            << toUtf8(albummid) << ".jpg";

        std::vector<uint8_t> data;
        curl_easy_reset(curl);
        curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
        curl_easy_setopt(curl, CURLOPT_REFERER, kReferer);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L); // 进度回调用于退出时立即中断
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curlXferAbort);

        CURLcode rc = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        const char* ctype = nullptr;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ctype);
        std::string contentType = ctype ? ctype : "";

        // 打印前 4 字节魔数，方便判断实际格式
        char magic[9] = {};
        if (data.size() >= 4) {
            std::snprintf(magic, sizeof(magic), "%02X %02X %02X %02X",
                          data[0], data[1], data[2], data[3]);
        }

        curl_easy_cleanup(curl);

        bool ok = rc == CURLE_OK && code == 200 && !data.empty();
        if (!ok) {
            std::wprintf(L"[cover] download failed: url=%S, curl=%d, http=%ld, size=%zu, magic=%S\n",
                         url.str().c_str(), (int)rc, code, data.size(), magic);
            data.clear();
        } else {
            std::wprintf(L"[cover] download ok: url=%S, type=%S, size=%zu, magic=%S\n",
                         url.str().c_str(), contentType.empty() ? "(none)" : contentType.c_str(),
                         data.size(), magic);
        }

        {
            std::lock_guard<std::mutex> lk(impl->mtx);
            if (impl->generation != gen) {
                // 已切歌，丢弃过期结果
                if (cb) cb(nullptr);
                return;
            }
        }
        if (cb) cb(ok ? std::make_shared<const std::vector<uint8_t>>(std::move(data)) : nullptr);
    });

    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->sweepFinished();
    impl_->workers.push_back(std::move(worker));
}
