// SPDX-License-Identifier: Apache-2.0 OR MIT
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace panelspun {

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    bool operator==(const Rect& o) const = default;
};

struct Size {
    int w = 0;
    int h = 0;
    bool operator==(const Size& o) const = default;
};

// Horizontal places the two children side by side; Vertical stacks them.
enum class Axis : std::uint8_t { Horizontal, Vertical };

enum class Side : std::uint8_t { Left, Right, Top, Bottom };

struct LeafRect {
    std::string id;
    Rect rect;
};

struct SplitHandle {
    int node = -1;
    Axis axis = Axis::Horizontal;
    Rect rect;
};

class SplitTree {
public:
    explicit SplitTree(std::string firstPanel);
    SplitTree(const SplitTree& other);
    SplitTree& operator=(const SplitTree& other);
    SplitTree(SplitTree&&) noexcept;
    SplitTree& operator=(SplitTree&&) noexcept;
    ~SplitTree();

    static bool validId(std::string_view id);

    bool contains(std::string_view panel) const;
    std::vector<std::string> panels() const;
    std::size_t panelCount() const;

    bool dock(const std::string& panel, std::string_view target, Side side, double ratio = 0.5);
    bool remove(std::string_view panel);

    void setMinSize(const std::string& panel, Size min);
    Size minSize(std::string_view panel) const;

    void setHandleThickness(int px);
    int handleThickness() const { return handle_; }

    // Lays the tree into bounds and remembers each split's rect for hitTest and drag.
    std::vector<LeafRect> layout(Rect bounds);
    const std::vector<SplitHandle>& handles() const { return lastHandles_; }

    // Returns the split whose handle (grown by slop pixels each side) holds the point, or -1.
    int hitTest(int px, int py, int slop = 3) const;

    // Moves a split's handle centre to the pointer and returns the clamped ratio, or -1 if unknown.
    double drag(int node, int px, int py);

    std::optional<double> ratio(int node) const;
    bool setRatio(int node, double ratio);

    std::string serialize() const;
    static std::optional<SplitTree> deserialize(std::string_view text, std::string* error = nullptr);

    struct Node;

private:
    SplitTree();
    Size subtreeMin(const Node& n) const;
    void layoutNode(Node& n, Rect r, std::vector<LeafRect>& out);
    Node* findNode(int id) const;

    std::unique_ptr<Node> root_;
    std::map<std::string, Size, std::less<>> mins_;
    int handle_ = 4;
    int nextId_ = 1;
    std::vector<SplitHandle> lastHandles_;
};

}
