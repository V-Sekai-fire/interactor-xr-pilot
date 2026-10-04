// SPDX-License-Identifier: Apache-2.0 OR MIT
#include "panelspun/window.h"

#include <SDL3/SDL.h>
#include <thorvg.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>

#include "font.h"
#include "vulkan_presenter.h"

namespace panelspun {

struct Window::Impl {
    WindowConfig config;
    SplitTree tree;
    std::map<std::string, std::unique_ptr<Panel>, std::less<>> panels;
    SDL_Window* window = nullptr;
    detail::VulkanPresenter presenter;
    std::unique_ptr<tvg::SwCanvas> canvas;
    bool tvgReady = false;
    std::uint32_t* target = nullptr;
    std::uint32_t targetW = 0;
    std::uint32_t targetH = 0;
    bool dirty = true;
    std::atomic<bool> wakeRequested{false};
    bool closing = false;
    int dragNode = -1;
    int hoverNode = -1;
    std::string capturedPanel;
    std::string hoveredPanel;
    std::string focusedPanel;
    bool relative = false;
    std::vector<LeafRect> leaves;
    SDL_Cursor* arrow = nullptr;
    SDL_Cursor* resizeEW = nullptr;
    SDL_Cursor* resizeNS = nullptr;
    std::string capturePath;
    bool captureOk = false;
    float scale = 1.0f;

    explicit Impl(SplitTree t) : tree(std::move(t)) {}
    ~Impl();

    int headerHeight() const { return static_cast<int>(std::lround(24.0f * scale)); }
    Rect contentRect(const Rect& r) const {
        int hh = std::min(headerHeight(), r.h);
        return Rect{r.x, r.y + hh, r.w, r.h - hh};
    }
    Panel* panelAt(int px, int py, std::string* id, Rect* content);
    bool forward(const std::string& id, PointerAction action, float px, float py, int button, float dx = 0.0f,
                 float dy = 0.0f);
    bool forwardKey(const SDL_KeyboardEvent& k);
    void handle(const SDL_Event& e);
    enum class Frame { Presented, Skipped, Failed };
    Frame render();
};

namespace {

tvg::Shape* rectShape(float x, float y, float w, float h, Color c, float radius = 0.0f) {
    tvg::Shape* s = tvg::Shape::gen();
    s->appendRect(x, y, w, h, radius, radius);
    s->fill(c.r, c.g, c.b, c.a);
    return s;
}

}

Window::Impl::~Impl() {
    panels.clear();
    canvas.reset();
    if (tvgReady) tvg::Initializer::term();
    presenter.shutdown();
    if (arrow) SDL_DestroyCursor(arrow);
    if (resizeEW) SDL_DestroyCursor(resizeEW);
    if (resizeNS) SDL_DestroyCursor(resizeNS);
    if (window) SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

Window::Window(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Window::~Window() = default;

std::unique_ptr<Window> Window::create(const WindowConfig& config, SplitTree layout, std::string* error) {
    std::unique_ptr<Impl> impl = std::make_unique<Impl>(std::move(layout));
    impl->config = config;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        if (error) *error = std::string("SDL_InitSubSystem(VIDEO): ") + SDL_GetError();
        return nullptr;
    }
    impl->window = SDL_CreateWindow(config.title.c_str(), config.width, config.height,
                                    SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!impl->window) {
        if (error) *error = std::string("SDL_CreateWindow: ") + SDL_GetError();
        return nullptr;
    }
    // Where window coordinates are pixels (Windows), the requested size is grown by the display scale.
    float density = SDL_GetWindowPixelDensity(impl->window);
    float extra = density > 0.0f ? SDL_GetWindowDisplayScale(impl->window) / density : 1.0f;
    if (extra > 1.0f) {
        int w = static_cast<int>(config.width * extra);
        int h = static_cast<int>(config.height * extra);
        SDL_Rect usable{0, 0, 0, 0};
        if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(impl->window), &usable) && usable.w > 0 && usable.h > 0) {
            w = std::min(w, usable.w * 9 / 10);
            h = std::min(h, usable.h * 9 / 10);
        }
        SDL_SetWindowSize(impl->window, w, h);
        SDL_SetWindowPosition(impl->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    if (!impl->presenter.init(impl->window, config.vulkanValidation, config.vulkanAllFeatures, error)) return nullptr;

    if (tvg::Initializer::init(0) != tvg::Result::Success) {
        if (error) *error = "ThorVG failed to initialise";
        return nullptr;
    }
    impl->tvgReady = true;
    if (!detail::registerDefaultFont()) {
        if (error) *error = "ThorVG could not load the bundled font";
        return nullptr;
    }
    impl->canvas.reset(tvg::SwCanvas::gen());
    if (!impl->canvas) {
        if (error) *error = "ThorVG could not create a CPU canvas";
        return nullptr;
    }
    impl->arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    impl->resizeEW = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
    impl->resizeNS = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
    return std::unique_ptr<Window>(new Window(std::move(impl)));
}

bool Window::setPanel(const std::string& id, std::unique_ptr<Panel> panel) {
    if (!impl_->tree.contains(id) || !panel) return false;
    impl_->panels[id] = std::move(panel);
    impl_->dirty = true;
    return true;
}

SplitTree& Window::layout() { return impl_->tree; }
const VulkanContext& Window::vulkan() const { return impl_->presenter.context(); }
void Window::requestRedraw() { impl_->dirty = true; }
void Window::requestRedrawFromAnyThread() {
    if (impl_->wakeRequested.exchange(true)) return;
    SDL_Event e{};
    e.type = SDL_EVENT_USER;
    SDL_PushEvent(&e);
}
void Window::requestClose() { impl_->closing = true; }
void Window::setRelativeMouse(bool on) {
    if (SDL_SetWindowRelativeMouseMode(impl_->window, on)) impl_->relative = on;
}
bool Window::relativeMouse() const { return impl_->relative; }
void Window::captureNextFrame(std::string path) {
    impl_->capturePath = std::move(path);
    impl_->captureOk = false;
    impl_->dirty = true;
}
bool Window::lastCaptureSucceeded() const { return impl_->captureOk; }
int Window::validationErrors() const { return impl_->presenter.validationErrors(); }

Panel* Window::Impl::panelAt(int px, int py, std::string* id, Rect* content) {
    for (const LeafRect& l : leaves) {
        Rect c = contentRect(l.rect);
        if (!c.contains(px, py)) continue;
        std::map<std::string, std::unique_ptr<Panel>, std::less<>>::iterator it = panels.find(l.id);
        if (it == panels.end()) return nullptr;
        if (id) *id = l.id;
        if (content) *content = c;
        return it->second.get();
    }
    return nullptr;
}

bool Window::Impl::forward(const std::string& id, PointerAction action, float px, float py, int button, float dx,
                           float dy) {
    std::map<std::string, std::unique_ptr<Panel>, std::less<>>::iterator it = panels.find(id);
    if (it == panels.end()) return false;
    for (const LeafRect& l : leaves) {
        if (l.id != id) continue;
        Rect c = contentRect(l.rect);
        PointerEvent e{action, px - c.x, py - c.y, button, dx, dy};
        return it->second->pointer(e);
    }
    return false;
}

bool Window::Impl::forwardKey(const SDL_KeyboardEvent& k) {
    const std::string& id = focusedPanel.empty() ? hoveredPanel : focusedPanel;
    std::map<std::string, std::unique_ptr<Panel>, std::less<>>::iterator it = panels.find(id);
    if (it == panels.end()) return false;
    KeyEvent e{static_cast<int>(k.scancode), k.down, k.repeat, static_cast<std::uint16_t>(k.mod)};
    return it->second->key(e);
}

void Window::Impl::handle(const SDL_Event& e) {
    float density = SDL_GetWindowPixelDensity(window);
    switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        closing = true;
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
    case SDL_EVENT_WINDOW_RESTORED:
        dirty = true;
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (relative && SDL_SetWindowRelativeMouseMode(window, false)) relative = false;
        for (std::pair<const std::string, std::unique_ptr<Panel>>& p : panels)
            if (p.second) p.second->focusLost();
        dirty = true;
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        dirty = forwardKey(e.key) || dirty;
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        std::string id = focusedPanel;
        float px = e.wheel.mouse_x * density;
        float py = e.wheel.mouse_y * density;
        if (!relative) {
            id.clear();
            panelAt(static_cast<int>(px), static_cast<int>(py), &id, nullptr);
        }
        if (!id.empty()) dirty = forward(id, PointerAction::Wheel, px, py, 0, 0.0f, e.wheel.y) || dirty;
        break;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        if (!hoveredPanel.empty() && capturedPanel.empty()) {
            dirty = forward(hoveredPanel, PointerAction::Leave, 0, 0, 0) || dirty;
            hoveredPanel.clear();
        }
        if (hoverNode != -1 && dragNode == -1) {
            hoverNode = -1;
            dirty = true;
        }
        break;
    case SDL_EVENT_MOUSE_MOTION: {
        float px = e.motion.x * density;
        float py = e.motion.y * density;
        float dx = e.motion.xrel * density;
        float dy = e.motion.yrel * density;
        if (relative && !focusedPanel.empty()) {
            dirty = forward(focusedPanel, PointerAction::Move, px, py, 0, dx, dy) || dirty;
            break;
        }
        if (dragNode != -1) {
            tree.drag(dragNode, static_cast<int>(px), static_cast<int>(py));
            dirty = true;
            break;
        }
        if (!capturedPanel.empty()) {
            dirty = forward(capturedPanel, PointerAction::Move, px, py, 0, dx, dy) || dirty;
            break;
        }
        int node = tree.hitTest(static_cast<int>(px), static_cast<int>(py), static_cast<int>(std::lround(3 * scale)));
        if (node != hoverNode) {
            hoverNode = node;
            dirty = true;
            SDL_Cursor* cursor = arrow;
            for (const SplitHandle& h : tree.handles())
                if (h.node == node) cursor = h.axis == Axis::Horizontal ? resizeEW : resizeNS;
            SDL_SetCursor(cursor);
        }
        std::string id;
        if (node == -1) panelAt(static_cast<int>(px), static_cast<int>(py), &id, nullptr);
        if (id != hoveredPanel) {
            if (!hoveredPanel.empty()) dirty = forward(hoveredPanel, PointerAction::Leave, 0, 0, 0) || dirty;
            hoveredPanel = id;
        }
        if (!id.empty()) dirty = forward(id, PointerAction::Move, px, py, 0, dx, dy) || dirty;
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        float px = e.button.x * density;
        float py = e.button.y * density;
        if (relative && !focusedPanel.empty()) {
            capturedPanel = focusedPanel;
            dirty = forward(focusedPanel, PointerAction::Down, px, py, e.button.button) || dirty;
            break;
        }
        int node = tree.hitTest(static_cast<int>(px), static_cast<int>(py), static_cast<int>(std::lround(3 * scale)));
        if (node != -1 && e.button.button == SDL_BUTTON_LEFT) {
            dragNode = node;
            dirty = true;
            break;
        }
        std::string id;
        if (panelAt(static_cast<int>(px), static_cast<int>(py), &id, nullptr)) {
            capturedPanel = id;
            focusedPanel = id;
            dirty = forward(id, PointerAction::Down, px, py, e.button.button) || dirty;
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        float px = e.button.x * density;
        float py = e.button.y * density;
        if (dragNode != -1) {
            dragNode = -1;
            dirty = true;
        }
        if (!capturedPanel.empty()) {
            dirty = forward(capturedPanel, PointerAction::Up, px, py, e.button.button) || dirty;
            capturedPanel.clear();
        }
        break;
    }
    default:
        break;
    }
}

Window::Impl::Frame Window::Impl::render() {
    int pw = 0;
    int ph = 0;
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    std::string error;
    if (!presenter.ensureSwapchain(pw, ph, &error)) {
        if (!error.empty()) SDL_Log("panelspun: %s", error.c_str());
        return error.empty() ? Frame::Skipped : Frame::Failed;
    }
    presenter.waitFrame();
    scale = SDL_GetWindowDisplayScale(window);
    if (scale <= 0.0f) scale = 1.0f;

    std::uint32_t w = presenter.width();
    std::uint32_t h = presenter.height();
    if (presenter.uiPixels() != target || w != targetW || h != targetH) {
        tvg::ColorSpace cs = presenter.bgra() ? tvg::ColorSpace::ARGB8888 : tvg::ColorSpace::ABGR8888;
        if (canvas->target(presenter.uiPixels(), w, w, h, cs) != tvg::Result::Success) return Frame::Failed;
        target = presenter.uiPixels();
        targetW = w;
        targetH = h;
    }

    const Theme& theme = config.theme;
    leaves = tree.layout(Rect{0, 0, static_cast<int>(w), static_cast<int>(h)});
    canvas->remove();
    canvas->add(rectShape(0, 0, static_cast<float>(w), static_cast<float>(h), theme.background));
    for (const SplitHandle& hd : tree.handles()) {
        bool active = hd.node == dragNode || (dragNode == -1 && hd.node == hoverNode);
        canvas->add(rectShape(static_cast<float>(hd.rect.x), static_cast<float>(hd.rect.y), static_cast<float>(hd.rect.w),
                              static_cast<float>(hd.rect.h), active ? theme.handleActive : theme.handle));
    }

    std::vector<detail::RegionRequest> regions;
    for (const LeafRect& l : leaves) {
        if (l.rect.w <= 0 || l.rect.h <= 0) continue;
        Rect c = contentRect(l.rect);
        float hh = static_cast<float>(c.y - l.rect.y);
        canvas->add(rectShape(static_cast<float>(l.rect.x), static_cast<float>(l.rect.y), static_cast<float>(l.rect.w), hh,
                              theme.header));
        std::map<std::string, std::unique_ptr<Panel>, std::less<>>::iterator it = panels.find(l.id);
        Panel* panel = it == panels.end() ? nullptr : it->second.get();

        tvg::Text* title = tvg::Text::gen();
        title->font(detail::kDefaultFont);
        title->size(13.0f * scale * 0.75f);
        title->text(panel ? panel->title().c_str() : l.id.c_str());
        title->fill(theme.text.r, theme.text.g, theme.text.b);
        title->align(0.0f, 0.5f);
        title->translate(l.rect.x + 8.0f * scale, l.rect.y + hh * 0.5f);
        tvg::Shape* titleClip = rectShape(static_cast<float>(l.rect.x), static_cast<float>(l.rect.y),
                                          static_cast<float>(l.rect.w), hh, theme.header);
        tvg::Scene* titleScene = tvg::Scene::gen();
        titleScene->add(title);
        titleScene->clip(titleClip);
        canvas->add(titleScene);

        if (c.w <= 0 || c.h <= 0) continue;
        canvas->add(rectShape(static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.w),
                              static_cast<float>(c.h), theme.panel));
        if (!panel) continue;
        if (panel->usesVulkanRegion()) {
            regions.push_back(detail::RegionRequest{panel, c});
            continue;
        }
        tvg::Scene* outer = tvg::Scene::gen();
        tvg::Scene* inner = tvg::Scene::gen();
        inner->translate(static_cast<float>(c.x), static_cast<float>(c.y));
        DrawContext ctx{inner, c.w, c.h, scale, detail::kDefaultFont, &theme};
        panel->draw(ctx);
        outer->add(inner);
        outer->clip(rectShape(static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.w),
                              static_cast<float>(c.h), theme.panel));
        canvas->add(outer);
    }
    if (canvas->draw(false) != tvg::Result::Success) return Frame::Failed;
    canvas->sync();

    bool captured = false;
    std::string path = capturePath;
    if (!presenter.present(regions, path, &captured)) return Frame::Failed;
    if (!path.empty()) {
        captureOk = captured;
        capturePath.clear();
    }
    return Frame::Presented;
}

int Window::run(int frameLimit) {
    Impl& s = *impl_;
    int presented = 0;
    const Uint64 tickMs = s.config.tickHz > 0 ? static_cast<Uint64>(std::max(1, 1000 / s.config.tickHz)) : 0;
    Uint64 lastTick = SDL_GetTicks();
    while (!s.closing) {
        SDL_Event e;
        if (frameLimit > 0) s.dirty = true;
        if (!s.dirty && tickMs > 0) {
            if (SDL_WaitEventTimeout(&e, static_cast<Sint32>(tickMs))) s.handle(e);
        } else if (!s.dirty && SDL_WaitEvent(&e)) {
            s.handle(e);
        }
        while (SDL_PollEvent(&e)) s.handle(e);
        if (s.wakeRequested.exchange(false)) s.dirty = true;
        if (tickMs > 0 && SDL_GetTicks() - lastTick >= tickMs) {
            lastTick = SDL_GetTicks();
            s.dirty = true;
        }
        if (s.closing || !s.dirty) continue;
        s.dirty = false;
        s.presenter.waitFrame();
        for (std::pair<const std::string, std::unique_ptr<Panel>>& p : s.panels)
            if (p.second) p.second->update(s.presenter.context());
        Impl::Frame f = s.render();
        if (f == Impl::Frame::Failed) return 1;
        if (f == Impl::Frame::Presented) {
            ++presented;
            s.dirty = s.dirty || s.presenter.stale();
            if (frameLimit > 0 && presented >= frameLimit) break;
        }
    }
    return 0;
}

}
