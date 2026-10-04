// SPDX-License-Identifier: Apache-2.0 OR MIT
#include "panelspun/split_tree.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <stdexcept>

namespace panelspun {

struct SplitTree::Node {
    int id = 0;
    bool leaf = true;
    std::string panel;
    Axis axis = Axis::Horizontal;
    double ratio = 0.5;
    Rect rect;
    std::unique_ptr<Node> first;
    std::unique_ptr<Node> second;
};

namespace {

constexpr std::string_view kHeader = "panelspun-layout 1";

std::unique_ptr<SplitTree::Node> cloneNode(const SplitTree::Node& n) {
    std::unique_ptr<SplitTree::Node> c = std::make_unique<SplitTree::Node>();
    c->id = n.id;
    c->leaf = n.leaf;
    c->panel = n.panel;
    c->axis = n.axis;
    c->ratio = n.ratio;
    c->rect = n.rect;
    if (n.first) c->first = cloneNode(*n.first);
    if (n.second) c->second = cloneNode(*n.second);
    return c;
}

SplitTree::Node* findLeaf(SplitTree::Node* n, std::string_view panel) {
    if (!n) return nullptr;
    if (n->leaf) return n->panel == panel ? n : nullptr;
    SplitTree::Node* f = findLeaf(n->first.get(), panel);
    return f ? f : findLeaf(n->second.get(), panel);
}

void collect(const SplitTree::Node& n, std::vector<std::string>& out) {
    if (n.leaf) {
        out.push_back(n.panel);
        return;
    }
    collect(*n.first, out);
    collect(*n.second, out);
}

std::string formatRatio(double r) {
    char buf[64];
    std::to_chars_result res = std::to_chars(buf, buf + sizeof(buf), r);
    return std::string(buf, res.ptr);
}

bool validRatio(double r) { return std::isfinite(r) && r > 0.0 && r < 1.0; }

}

SplitTree::SplitTree() = default;

SplitTree::SplitTree(std::string firstPanel) {
    root_ = std::make_unique<Node>();
    root_->id = nextId_++;
    if (!validId(firstPanel)) throw std::invalid_argument("invalid panel id");
    root_->panel = std::move(firstPanel);
}

SplitTree::SplitTree(const SplitTree& other)
    : root_(other.root_ ? cloneNode(*other.root_) : nullptr),
      mins_(other.mins_),
      handle_(other.handle_),
      nextId_(other.nextId_),
      lastHandles_(other.lastHandles_) {}

SplitTree& SplitTree::operator=(const SplitTree& other) {
    if (this != &other) {
        SplitTree copy(other);
        *this = std::move(copy);
    }
    return *this;
}

SplitTree::SplitTree(SplitTree&&) noexcept = default;
SplitTree& SplitTree::operator=(SplitTree&&) noexcept = default;
SplitTree::~SplitTree() = default;

bool SplitTree::validId(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                  c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

bool SplitTree::contains(std::string_view panel) const { return findLeaf(root_.get(), panel) != nullptr; }

std::vector<std::string> SplitTree::panels() const {
    std::vector<std::string> out;
    if (root_) collect(*root_, out);
    return out;
}

std::size_t SplitTree::panelCount() const { return panels().size(); }

bool SplitTree::dock(const std::string& panel, std::string_view target, Side side, double ratio) {
    if (!validId(panel) || contains(panel) || !validRatio(ratio)) return false;
    Node* leaf = findLeaf(root_.get(), target);
    if (!leaf) return false;

    std::unique_ptr<Node> existing = std::make_unique<Node>();
    existing->id = nextId_++;
    existing->panel = leaf->panel;
    std::unique_ptr<Node> added = std::make_unique<Node>();
    added->id = nextId_++;
    added->panel = panel;

    bool newFirst = side == Side::Left || side == Side::Top;
    leaf->leaf = false;
    leaf->panel.clear();
    leaf->axis = (side == Side::Left || side == Side::Right) ? Axis::Horizontal : Axis::Vertical;
    leaf->ratio = newFirst ? ratio : 1.0 - ratio;
    leaf->first = newFirst ? std::move(added) : std::move(existing);
    leaf->second = newFirst ? std::move(existing) : std::move(added);
    return true;
}

bool SplitTree::remove(std::string_view panel) {
    if (!root_ || root_->leaf) return false;
    std::function<bool(std::unique_ptr<Node>&)> walk = [&](std::unique_ptr<Node>& n) -> bool {
        if (n->leaf) return false;
        for (int i = 0; i < 2; ++i) {
            std::unique_ptr<Node>& child = i == 0 ? n->first : n->second;
            std::unique_ptr<Node>& sibling = i == 0 ? n->second : n->first;
            if (child->leaf && child->panel == panel) {
                std::unique_ptr<Node> keep = std::move(sibling);
                n = std::move(keep);
                return true;
            }
        }
        return walk(n->first) || walk(n->second);
    };
    return walk(root_);
}

void SplitTree::setMinSize(const std::string& panel, Size min) {
    mins_[panel] = Size{std::max(0, min.w), std::max(0, min.h)};
}

Size SplitTree::minSize(std::string_view panel) const {
    std::map<std::string, Size, std::less<>>::const_iterator it = mins_.find(panel);
    return it == mins_.end() ? Size{} : it->second;
}

void SplitTree::setHandleThickness(int px) { handle_ = std::max(1, px); }

Size SplitTree::subtreeMin(const Node& n) const {
    if (n.leaf) return minSize(n.panel);
    Size a = subtreeMin(*n.first);
    Size b = subtreeMin(*n.second);
    if (n.axis == Axis::Horizontal) return Size{a.w + handle_ + b.w, std::max(a.h, b.h)};
    return Size{std::max(a.w, b.w), a.h + handle_ + b.h};
}

void SplitTree::layoutNode(Node& n, Rect r, std::vector<LeafRect>& out) {
    n.rect = r;
    if (n.leaf) {
        out.push_back(LeafRect{n.panel, r});
        return;
    }
    bool horiz = n.axis == Axis::Horizontal;
    int extent = horiz ? r.w : r.h;
    int avail = std::max(0, extent - handle_);
    Size minA = subtreeMin(*n.first);
    Size minB = subtreeMin(*n.second);
    int loA = horiz ? minA.w : minA.h;
    int loB = horiz ? minB.w : minB.h;

    int a = static_cast<int>(std::lround(avail * n.ratio));
    if (loA + loB <= avail) {
        a = std::clamp(a, loA, avail - loB);
    } else if (loA + loB > 0) {
        a = static_cast<int>(std::lround(static_cast<double>(avail) * loA / (loA + loB)));
    }
    int b = avail - a;

    Rect ra = r;
    Rect rb = r;
    Rect rh = r;
    if (horiz) {
        ra.w = a;
        rh.x = r.x + a;
        rh.w = handle_;
        rb.x = r.x + a + handle_;
        rb.w = b;
    } else {
        ra.h = a;
        rh.y = r.y + a;
        rh.h = handle_;
        rb.y = r.y + a + handle_;
        rb.h = b;
    }
    lastHandles_.push_back(SplitHandle{n.id, n.axis, rh});
    layoutNode(*n.first, ra, out);
    layoutNode(*n.second, rb, out);
}

std::vector<LeafRect> SplitTree::layout(Rect bounds) {
    std::vector<LeafRect> out;
    lastHandles_.clear();
    if (root_) layoutNode(*root_, bounds, out);
    return out;
}

int SplitTree::hitTest(int px, int py, int slop) const {
    for (const SplitHandle& h : lastHandles_) {
        Rect grown = h.rect;
        if (h.axis == Axis::Horizontal) {
            grown.x -= slop;
            grown.w += 2 * slop;
        } else {
            grown.y -= slop;
            grown.h += 2 * slop;
        }
        if (grown.contains(px, py)) return h.node;
    }
    return -1;
}

SplitTree::Node* SplitTree::findNode(int id) const {
    std::function<Node*(Node*)> walk = [&](Node* n) -> Node* {
        if (!n) return nullptr;
        if (n->id == id) return n;
        if (n->leaf) return nullptr;
        Node* f = walk(n->first.get());
        return f ? f : walk(n->second.get());
    };
    return walk(root_.get());
}

double SplitTree::drag(int node, int px, int py) {
    Node* n = findNode(node);
    if (!n || n->leaf) return -1.0;
    bool horiz = n->axis == Axis::Horizontal;
    int extent = horiz ? n->rect.w : n->rect.h;
    int avail = extent - handle_;
    if (avail <= 0) return n->ratio;
    double pos = horiz ? (px - n->rect.x) : (py - n->rect.y);
    double r = (pos - handle_ * 0.5) / avail;

    Size minA = subtreeMin(*n->first);
    Size minB = subtreeMin(*n->second);
    double lo = static_cast<double>(horiz ? minA.w : minA.h) / avail;
    double hi = 1.0 - static_cast<double>(horiz ? minB.w : minB.h) / avail;
    if (lo <= hi) r = std::clamp(r, lo, hi);
    else r = lo / (lo + (1.0 - hi));
    r = std::clamp(r, 1e-6, 1.0 - 1e-6);
    n->ratio = r;
    return r;
}

std::optional<double> SplitTree::ratio(int node) const {
    Node* n = findNode(node);
    if (!n || n->leaf) return std::nullopt;
    return n->ratio;
}

bool SplitTree::setRatio(int node, double r) {
    Node* n = findNode(node);
    if (!n || n->leaf || !validRatio(r)) return false;
    n->ratio = r;
    return true;
}

std::string SplitTree::serialize() const {
    std::string out(kHeader);
    out += '\n';
    std::function<void(const Node&)> walk = [&](const Node& n) {
        if (n.leaf) {
            out += "leaf ";
            out += n.panel;
            out += '\n';
            return;
        }
        out += n.axis == Axis::Horizontal ? "split h " : "split v ";
        out += formatRatio(n.ratio);
        out += '\n';
        walk(*n.first);
        walk(*n.second);
    };
    if (root_) walk(*root_);
    return out;
}

std::optional<SplitTree> SplitTree::deserialize(std::string_view text, std::string* error) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.push_back(line);
        start = end + 1;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();

    std::function<bool(const std::string&)> fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };

    if (lines.empty() || lines[0] != kHeader) {
        fail("missing header '" + std::string(kHeader) + "'");
        return std::nullopt;
    }

    SplitTree tree;
    std::size_t cursor = 1;
    std::map<std::string, int, std::less<>> seen;
    std::function<bool(std::unique_ptr<Node>&, int)> parse = [&](std::unique_ptr<Node>& slot, int depth) -> bool {
        if (depth > 256) return fail("nesting deeper than 256");
        if (cursor >= lines.size()) return fail("truncated: expected a node at line " + std::to_string(cursor + 1));
        std::string_view line = lines[cursor];
        std::string lineNo = std::to_string(cursor + 1);
        ++cursor;
        slot = std::make_unique<Node>();
        slot->id = tree.nextId_++;
        if (line.substr(0, 5) == "leaf ") {
            std::string_view id = line.substr(5);
            if (!validId(id)) return fail("invalid panel id at line " + lineNo);
            if (seen.count(id)) return fail("duplicate panel id '" + std::string(id) + "' at line " + lineNo);
            seen.emplace(std::string(id), 1);
            slot->panel = std::string(id);
            return true;
        }
        if (line.size() < 9 || line.substr(0, 6) != "split " || line[7] != ' ' || (line[6] != 'h' && line[6] != 'v'))
            return fail("unrecognised line " + lineNo);
        std::string_view num = line.substr(8);
        double r = 0.0;
        std::from_chars_result res = std::from_chars(num.data(), num.data() + num.size(), r);
        if (res.ec != std::errc() || res.ptr != num.data() + num.size() || !validRatio(r))
            return fail("ratio must be a number strictly between 0 and 1 at line " + lineNo);
        slot->leaf = false;
        slot->axis = line[6] == 'h' ? Axis::Horizontal : Axis::Vertical;
        slot->ratio = r;
        return parse(slot->first, depth + 1) && parse(slot->second, depth + 1);
    };

    if (!parse(tree.root_, 0)) return std::nullopt;
    if (cursor != lines.size()) {
        fail("trailing content at line " + std::to_string(cursor + 1));
        return std::nullopt;
    }
    return tree;
}

}
