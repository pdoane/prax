#include "pr/webview.h"
#include "pr/webview_backend.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

extern "C" int prTestWebViewCApi(void);

namespace pr_impl {

namespace {

// A view that records the calls made to it.
struct FakeView {
    PrWebView view;
    std::vector<std::string> calls;
};

FakeView& fakeOf(PrWebView* view) {
    return *reinterpret_cast<FakeView*>(view);
}

void createFake(FakeView& fake) {
    fake.view.destroyFn = [](PrWebView* view) { fakeOf(view).calls.push_back("destroy"); };
    fake.view.resizeFn = [](PrWebView* view, PrUVec2) { fakeOf(view).calls.push_back("resize"); };
    fake.view.loadFn = [](PrWebView* view, PrStringView, PrWebViewDoneFn, void*) { fakeOf(view).calls.push_back("load"); };
    fake.view.evaluateFn = [](PrWebView* view, PrStringView, PrWebViewResultFn, void*) {
        fakeOf(view).calls.push_back("evaluate");
    };
    fake.view.captureFn = [](PrWebView* view, PrRect, PrUVec2, PrWebViewCaptureFn, void*) {
        fakeOf(view).calls.push_back("capture");
    };
    fake.view.showFn = [](PrWebView* view, const PrAppSurface*, PrRect, PrRect) { fakeOf(view).calls.push_back("show"); };
    fake.view.hideFn = [](PrWebView* view) { fakeOf(view).calls.push_back("hide"); };
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public header compiles as C") {
    CHECK(prTestWebViewCApi() == 0);
}

TEST_CASE("the interface passes calls through, and an engine without printing or processes reports none") {
    FakeView fake{};
    createFake(fake);
    PrWebView* view = &fake.view;
    prWebViewResize(view, PrUVec2{10, 10});
    prWebViewLoad(view, "about:blank", [](void*, PrStatus) {}, nullptr);
    prWebViewEvaluate(view, "1", [](void*, PrStatus, PrStringView) {}, nullptr);
    prWebViewCapture(view, PrRect{0, 0, 1, 1}, PrUVec2{1, 1}, [](void*, PrStatus, PrPixelView) {}, nullptr);
    PrAppSurface host{};
    prWebViewShow(view, &host, PrRect{}, PrRect{});
    prWebViewHide(view);
    CHECK(prWebViewPrint(view, "page.pdf", PrVec2{}, [](void*, PrStatus) {}, nullptr) == PrStatus_NotSupported);
    CHECK(prWebViewProcesses(view, nullptr, 0) == 0);
    prWebViewDestroy(view);
    prWebViewDestroy(nullptr);
    CHECK(fake.calls == std::vector<std::string>{"resize", "load", "evaluate", "capture", "show", "hide", "destroy"});
}

TEST_CASE("backends are named in lowercase, and a backend that the build lacks creates nothing") {
    CHECK(prWebViewBackendName(PrWebViewBackend_WebKit) == "webkit");
    CHECK(prWebViewBackendFromName("WebView2") == PrWebViewBackend_WebView2);
    CHECK(prWebViewBackendFromName("gecko") == PrWebViewBackend_Count);
    for (PrWebViewBackend backend = PrWebViewBackend_WebKit; backend < PrWebViewBackend_Count; ++backend) {
        if (prWebViewBackendIsBuilt(backend))
            continue;
        PrWebView* view = nullptr;
        PrWebViewDesc desc{.viewport = PrUVec2{10, 10}};
        CHECK(prWebViewCreate(backend, &desc, &view) == PrStatus_NotSupported);
        CHECK(view == nullptr);
    }
}
