#include <atomic>
#include <chrono>
#include <list>
#include <print>
#include <string>

namespace enu {

namespace chrono = std::chrono;

struct ProgressNode;

class ProgressNodeRef {
  ProgressNode* ptr_;

 public:
  explicit ProgressNodeRef(ProgressNode* p) : ptr_(p) {}

  ProgressNodeRef(const ProgressNodeRef&) = delete;
  ProgressNodeRef& operator=(const ProgressNodeRef&) = delete;

  inline ProgressNode* operator->() {
    return ptr_;
  }
  inline const ProgressNode* operator->() const {
    return ptr_;
  }
};

struct DrawContext {
  static constexpr auto UPDATE_INTERVAL_MS = chrono::milliseconds{80};

  chrono::steady_clock::time_point start_, last_draw_{start_};
  ProgressNode* root_{nullptr};

  explicit DrawContext(ProgressNode* root) : start_(chrono::steady_clock::now()), root_(root) {}

  void render();
};

struct ProgressNode {
  std::string name_;

  std::list<ProgressNode> children_;
  DrawContext* draw_ctx_ = nullptr;
  ProgressNode* parent_ = nullptr;
  std::list<ProgressNode>::iterator self_iterator_;

  std::atomic<size_t> done_ = 0;
  std::atomic<size_t> total_ = 0;

  // Constructor for adding a child.
  explicit ProgressNode(std::string name, size_t total) : name_(std::move(name)), total_(total) {}

  // Constructor for defining a root node.
  explicit ProgressNode(std::string name) : name_(std::move(name)) {
    draw_ctx_ = new DrawContext{this};
  }

  ProgressNode(const ProgressNode&) = delete;
  ProgressNode& operator=(const ProgressNode&) = delete;

  ~ProgressNode() {
    // The root node destroys the draw context.
    if (!parent_)
      delete draw_ctx_;
  }

  template <typename... Args>
  ProgressNodeRef add_child(Args&&... args) {
    ProgressNode& child = children_.emplace_back(std::forward<Args>(args)...);
    child.draw_ctx_ = draw_ctx_;
    child.parent_ = this;
    child.self_iterator_ = std::prev(children_.end());
    ++total_; // Treat a child as one unit of work.
    return ProgressNodeRef{&child};
  }

  void tick(size_t delta) {
    done_ += delta;
    draw_ctx_->render();
  }

  // You have to manually call this on the root
  void finish() {
    if (!parent_) {
      // Clear and show cursor when destroying the last root
      std::print("\033[J\n\033[?25h");
      return;
    }

    ++parent_->done_;
    parent_->children_.erase(self_iterator_);
  }

  // Increments the total goal atomically.
  void shift_goal(size_t delta) {
    total_ += delta;
  }

  bool all_done() const {
    if (done_ < total_)
      return false;
    for (const ProgressNode& child : children_)
      if (!child.all_done())
        return false;
    return true;
  }
};

inline std::pair<double, std::string_view> scale(double bytes) {
  const std::array<std::string_view, 5> UNITS = {"B", "KiB", "MiB", "GiB"};

  size_t idx = 0;
  while (bytes >= 1024.0 && idx + 1 < UNITS.size()) {
    bytes /= 1024.0;
    ++idx;
  }
  return {bytes, UNITS[idx]};
}

inline void DrawContext::render() {
  const size_t COL_START = 53;
  const size_t BAR_WIDTH = 40;

  chrono::time_point now = chrono::steady_clock::now();
  if (now - last_draw_ < UPDATE_INTERVAL_MS)
    return;

  last_draw_ = now;
  double elapsed_s = chrono::duration<double>(now - start_).count();

  std::string out;
  size_t sum_done = 0, sum_total = 0, line_count = 0;

  auto total_stats = [&](size_t done, size_t total) {
    double ratio = total ? static_cast<double>(done) / total : 1.0;
    size_t filled = static_cast<size_t>(ratio * BAR_WIDTH);

    auto [done_val, done_unit] = scale(static_cast<double>(done));
    auto [speed_val, speed_unit] = scale(elapsed_s > 0.0 ? done / elapsed_s : 0.0);

    return std::format(
      "[{}{}] [{:5.1f}% {:5.1f} {} {:5.1f} {}/s ]\n",
      std::string(filled, '='),
      std::string(BAR_WIDTH - filled, ' '),
      std::min(100.0, ratio * 100.0),
      done_val,
      done_unit,
      speed_val,
      speed_unit);
  };

  auto node_stats = [](size_t done, size_t total) {
    const double pct = total ? 100. * done / total : 100.;
    auto [done_val, done_unit] = scale(static_cast<double>(done));
    return std::format("[{:5.1f}% {:5.1f}{:3.1}]", pct, done_val, done_unit);
  };

  auto visit = [&](const auto& self, const ProgressNode* n, std::string prefix, bool last) -> void {
    if (n->parent_) {
      out += prefix;
      out += last ? "+- " : "|- ";
      prefix += last ? "   " : "|  ";
      // Utf-8 versions:
      // out += last ? "└─ " : "├─ ";
      // prefix += last ? "   " : "│  ";
    }

    std::string_view name = n->name_;
    out += name;

    if (prefix.size() + name.size() >= COL_START) {
      out += ' ';
    } else {
      size_t pad = COL_START - (prefix.size() + name.size());
      out.append(pad, ' ');
    }

    out += node_stats(n->done_, n->total_);
    out += '\n';
    ++line_count;

    sum_done += n->done_;
    sum_total += n->total_;

    size_t idx = 0;
    for (const ProgressNode& c : n->children_)
      self(self, &c, prefix, ++idx == n->children_.size());
  };

  visit(visit, root_, "", true);
  out += total_stats(sum_done, sum_total);
  ++line_count;

  // Start sync update.
  std::print("\x1b[?2026h");
  // Clear below cursor.
  std::print("\033[J");
  // Draw frame.
  std::print("{}", out);
  // Move cursor up.
  std::print("\033[{}A", line_count);
  // End sync update.
  std::print("\x1b[?2026l");
  std::fflush(stdout);
}

} // namespace enu
