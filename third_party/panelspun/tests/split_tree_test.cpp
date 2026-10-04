// SPDX-License-Identifier: Apache-2.0 OR MIT
#include <stdexcept>
#include <string>
#include <vector>

#include "check.h"
#include "panelspun/split_tree.h"

using panelspun::Axis;
using panelspun::LeafRect;
using panelspun::Rect;
using panelspun::Side;
using panelspun::Size;
using panelspun::SplitTree;

namespace {

Rect rectOf(const std::vector<LeafRect>& leaves, const std::string& id) {
    for (const LeafRect& l : leaves)
        if (l.id == id) return l.rect;
    return Rect{-1, -1, -1, -1};
}

// video | (controls / buttons), the demo's arrangement.
SplitTree threePanels() {
    SplitTree t("video");
    t.dock("controls", "video", Side::Right, 0.3);
    t.dock("buttons", "controls", Side::Bottom, 0.5);
    return t;
}

bool rectsTile(const std::vector<LeafRect>& leaves, const std::vector<panelspun::SplitHandle>& handles, Rect bounds) {
    long long area = 0;
    for (const LeafRect& l : leaves) area += static_cast<long long>(l.rect.w) * l.rect.h;
    for (const panelspun::SplitHandle& h : handles) area += static_cast<long long>(h.rect.w) * h.rect.h;
    return area == static_cast<long long>(bounds.w) * bounds.h;
}

}

TEST_CASE("layout.single-panel-fills-bounds") {
    SplitTree t("only");
    std::vector<LeafRect> leaves = t.layout(Rect{10, 20, 300, 200});
    CHECK_EQ(leaves.size(), 1u);
    CHECK(leaves[0].rect == (Rect{10, 20, 300, 200}));
    CHECK(t.handles().empty());
}

TEST_CASE("layout.three-panels-tile-without-overlap") {
    SplitTree t = threePanels();
    Rect bounds{0, 0, 1004, 604};
    std::vector<LeafRect> leaves = t.layout(bounds);
    CHECK_EQ(leaves.size(), 3u);
    CHECK(rectsTile(leaves, t.handles(), bounds));
    CHECK(rectOf(leaves, "video") == (Rect{0, 0, 700, 604}));
    CHECK(rectOf(leaves, "controls") == (Rect{704, 0, 300, 300}));
    CHECK(rectOf(leaves, "buttons") == (Rect{704, 304, 300, 300}));
    CHECK_EQ(t.handles().size(), 2u);
}

TEST_CASE("layout.tiling-check-rejects-overlap") {
    // Negative control for rectsTile: a duplicated leaf must not pass as a tiling.
    SplitTree t = threePanels();
    Rect bounds{0, 0, 1004, 604};
    std::vector<LeafRect> leaves = t.layout(bounds);
    leaves.push_back(leaves[0]);
    CHECK(!rectsTile(leaves, t.handles(), bounds));
}

TEST_CASE("handles.hit-test-finds-split-and-misses-panel") {
    SplitTree t = threePanels();
    t.layout(Rect{0, 0, 1004, 604});
    int vertical = t.hitTest(702, 100);
    CHECK(vertical > 0);
    CHECK(t.ratio(vertical).has_value());
    CHECK(t.hitTest(850, 302) > 0);
    CHECK(t.hitTest(850, 302) != vertical);
    CHECK_EQ(t.hitTest(300, 300), -1);
    CHECK_EQ(t.hitTest(850, 100), -1);
}

TEST_CASE("resize.drag-moves-split") {
    SplitTree t = threePanels();
    t.layout(Rect{0, 0, 1004, 604});
    int node = t.hitTest(702, 100);
    double r = t.drag(node, 502, 100);
    CHECK(r > 0.49 && r < 0.51);
    std::vector<LeafRect> leaves = t.layout(Rect{0, 0, 1004, 604});
    CHECK_EQ(rectOf(leaves, "video").w, 500);
    CHECK_EQ(rectOf(leaves, "controls").x, 504);
}

TEST_CASE("resize.drag-past-minimum-is-clamped") {
    SplitTree t = threePanels();
    t.setMinSize("video", Size{200, 100});
    t.setMinSize("controls", Size{250, 80});
    t.setMinSize("buttons", Size{0, 120});
    Rect bounds{0, 0, 1004, 604};
    t.layout(bounds);
    int node = t.hitTest(702, 100);
    t.drag(node, 5, 100);
    std::vector<LeafRect> leaves = t.layout(bounds);
    CHECK_EQ(rectOf(leaves, "video").w, 200);
    t.drag(node, 1000, 100);
    leaves = t.layout(bounds);
    CHECK_EQ(rectOf(leaves, "controls").w, 250);
    CHECK_EQ(rectOf(leaves, "video").w, 1000 - 250);
    int horizontal = t.hitTest(850, 302);
    t.drag(horizontal, 850, 600);
    leaves = t.layout(bounds);
    CHECK_EQ(rectOf(leaves, "buttons").h, 120);
    CHECK_EQ(rectOf(leaves, "controls").h, 480);
}

TEST_CASE("resize.minimum-holds-when-window-shrinks") {
    SplitTree t = threePanels();
    t.setMinSize("controls", Size{250, 0});
    std::vector<LeafRect> leaves = t.layout(Rect{0, 0, 404, 300});
    CHECK_EQ(rectOf(leaves, "controls").w, 250);
    CHECK_EQ(rectOf(leaves, "video").w, 150);
}

TEST_CASE("resize.unknown-node-and-leaf-refused") {
    SplitTree t = threePanels();
    t.layout(Rect{0, 0, 1004, 604});
    CHECK_EQ(t.drag(9999, 10, 10), -1.0);
    CHECK(!t.setRatio(9999, 0.5));
    int node = t.hitTest(702, 100);
    CHECK(!t.setRatio(node, 0.0));
    CHECK(!t.setRatio(node, 1.0));
    CHECK(t.setRatio(node, 0.25));
}

TEST_CASE("dock.each-side-places-panel") {
    struct Expect {
        Side side;
        Rect added;
        Rect target;
    };
    std::vector<Expect> cases = {
        {Side::Left, Rect{0, 0, 98, 100}, Rect{102, 0, 98, 100}},
        {Side::Right, Rect{102, 0, 98, 100}, Rect{0, 0, 98, 100}},
        {Side::Top, Rect{0, 0, 200, 48}, Rect{0, 52, 200, 48}},
        {Side::Bottom, Rect{0, 52, 200, 48}, Rect{0, 0, 200, 48}},
    };
    for (const Expect& e : cases) {
        SplitTree t("a");
        CHECK(t.dock("b", "a", e.side));
        std::vector<LeafRect> leaves = t.layout(Rect{0, 0, 200, 100});
        CHECK(rectOf(leaves, "b") == e.added);
        CHECK(rectOf(leaves, "a") == e.target);
    }
}

TEST_CASE("dock.refuses-duplicate-missing-target-and-bad-id") {
    SplitTree t = threePanels();
    CHECK(!t.dock("video", "controls", Side::Left));
    CHECK(!t.dock("new", "absent", Side::Left));
    CHECK(!t.dock("bad id", "video", Side::Left));
    CHECK(!t.dock("", "video", Side::Left));
    CHECK(!t.dock("new", "video", Side::Left, 1.5));
    CHECK_EQ(t.panelCount(), 3u);
}

TEST_CASE("undock.remove-collapses-parent") {
    SplitTree t = threePanels();
    CHECK(t.remove("controls"));
    CHECK_EQ(t.panelCount(), 2u);
    CHECK(!t.contains("controls"));
    std::vector<LeafRect> leaves = t.layout(Rect{0, 0, 1004, 604});
    CHECK(rectOf(leaves, "buttons") == (Rect{704, 0, 300, 604}));
    CHECK_EQ(t.handles().size(), 1u);
    CHECK(t.remove("video"));
    leaves = t.layout(Rect{0, 0, 1004, 604});
    CHECK(rectOf(leaves, "buttons") == (Rect{0, 0, 1004, 604}));
}

TEST_CASE("undock.removing-last-panel-is-refused") {
    SplitTree t("only");
    CHECK(!t.remove("only"));
    CHECK_EQ(t.panelCount(), 1u);
    SplitTree u = threePanels();
    CHECK(!u.remove("absent"));
    CHECK_EQ(u.panelCount(), 3u);
}

TEST_CASE("construct.invalid-first-id-throws") {
    bool threw = false;
    try {
        SplitTree t("has space");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("serialize.round-trip-preserves-layout") {
    SplitTree t = threePanels();
    t.layout(Rect{0, 0, 1004, 604});
    t.drag(t.hitTest(702, 100), 333, 0);
    std::string text = t.serialize();
    std::string err;
    std::optional<SplitTree> back = SplitTree::deserialize(text, &err);
    CHECK(back.has_value());
    CHECK(err.empty());
    if (!back) return;
    CHECK_EQ(back->serialize(), text);
    std::vector<LeafRect> a = t.layout(Rect{0, 0, 1004, 604});
    std::vector<LeafRect> b = back->layout(Rect{0, 0, 1004, 604});
    CHECK_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        CHECK_EQ(a[i].id, b[i].id);
        CHECK(a[i].rect == b[i].rect);
    }
}

TEST_CASE("serialize.format-is-line-based") {
    SplitTree t = threePanels();
    CHECK_EQ(t.serialize(), std::string("panelspun-layout 1\nsplit h 0.7\nleaf video\nsplit v 0.5\nleaf controls\nleaf buttons\n"));
    std::optional<SplitTree> crlf = SplitTree::deserialize("panelspun-layout 1\r\nsplit v 0.25\r\nleaf a\r\nleaf b\r\n");
    CHECK(crlf.has_value());
}

TEST_CASE("serialize.malformed-input-rejected") {
    const char* bad[] = {
        "",
        "panelspun-layout 2\nleaf a\n",
        "leaf a\n",
        "panelspun-layout 1\n",
        "panelspun-layout 1\nsplit h 0.5\nleaf a\n",
        "panelspun-layout 1\nsplit h 0.5\nleaf a\nleaf a\n",
        "panelspun-layout 1\nsplit h 1.5\nleaf a\nleaf b\n",
        "panelspun-layout 1\nsplit h 0\nleaf a\nleaf b\n",
        "panelspun-layout 1\nsplit h nan\nleaf a\nleaf b\n",
        "panelspun-layout 1\nsplit h 0.5x\nleaf a\nleaf b\n",
        "panelspun-layout 1\nsplit d 0.5\nleaf a\nleaf b\n",
        "panelspun-layout 1\nleaf a b\n",
        "panelspun-layout 1\nleaf \n",
        "panelspun-layout 1\nleaf a\nleaf b\n",
        "panelspun-layout 1\nleaf a\n\nleaf b\n",
        "panelspun-layout 1\ntab a\n",
    };
    int rejected = 0;
    for (const char* text : bad) {
        std::string err;
        std::optional<SplitTree> t = SplitTree::deserialize(text, &err);
        CHECK(!t.has_value());
        CHECK(!err.empty());
        if (!t) ++rejected;
    }
    CHECK_EQ(rejected, static_cast<int>(sizeof(bad) / sizeof(bad[0])));
}

TEST_CASE("serialize.deep-nesting-rejected") {
    std::string text = "panelspun-layout 1\n";
    for (int i = 0; i < 300; ++i) text += "split h 0.5\nleaf p" + std::to_string(i) + "\n";
    text += "leaf last\n";
    CHECK(!SplitTree::deserialize(text).has_value());
}

TEST_CASE("control.failing-check-fails") {
    CHECK(false);
}

TEST_CASE("control.empty-case-fails") {}
