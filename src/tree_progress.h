#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <list>
#include <mutex>
#include <print>

namespace enu {

namespace chrono = std::chrono;

std::pair<double, std::string_view> scale(double bytes);

static constexpr auto UPDATE_INTERVAL_MS = chrono::milliseconds{80};

template <typename M>
class ProgressNode;

template <typename M>
using RenderFn = std::function<size_t(ProgressNode<M>* root, double elapsed_s)>;

template <typename M>
struct ProgressGlobalMeta {
  RenderFn<M> render_fn_;
  ProgressNode<M>* root_;
  chrono::steady_clock::time_point start_, last_draw_{start_};
  size_t last_drawn_rows_ = 0;

  std::atomic<size_t> sum_done_ = 0;
  std::atomic<size_t> sum_total_ = 0;

  explicit ProgressGlobalMeta(RenderFn<M> render_fn, ProgressNode<M>* root)
    : render_fn_(std::move(render_fn)), root_(root), start_(chrono::steady_clock::now()) {}
};

template <typename M>
class ProgressNode {
  using LinkedList = std::list<ProgressNode<M>>;

  std::mutex mutex_;
  std::condition_variable cv_;
  bool ready_ = false;

  ProgressGlobalMeta<M>* global_meta_;

  LinkedList children_;
  LinkedList::iterator self_iterator_;

  std::atomic<size_t> done_ = 0;
  std::atomic<size_t> total_ = 0;

 public:
  ProgressNode<M>* parent = nullptr;
  M meta;

  explicit ProgressNode(M metadata, RenderFn<M> render_fn)
    : global_meta_(new ProgressGlobalMeta{render_fn, this}), meta(std::move(metadata)) {}

  explicit ProgressNode(M metadata, ProgressNode<M>* parent_)
    : global_meta_(parent_->global_meta_), parent(parent_), meta(std::move(metadata)) {}

  ProgressNode(const ProgressNode&) = delete;
  ProgressNode& operator=(const ProgressNode&) = delete;

  ~ProgressNode() {
    if (is_root()) {
      delete global_meta_;
    }
  }

  [[nodiscard]] ProgressNode<M>* add_child(M metadata) {
    std::scoped_lock lock(mutex_);

    ProgressNode<M>& child = children_.emplace_back(std::move(metadata), this);
    child.self_iterator_ = std::prev(children_.end());

    return &child;
  }

  void unlock() {
    {
      std::scoped_lock lock(mutex_);
      ready_ = true;
    }
    cv_.notify_all();
  }

  void iter_children(std::function<void(ProgressNode<M>* node)> f) {
    // We drop mutex here because it only exists to make the caller wait.
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] { return ready_; });
    }

    for (ProgressNode<M>& child : children_)
      f(&child);
  }

  // Not (fully) atomic!
  void tick(size_t delta) {
    done_ += delta;
    global_meta_->sum_done_ += delta;

    // Only draw every `UPDATE_INTERVAL_MS`.
    chrono::time_point now = chrono::steady_clock::now();
    if (now - global_meta_->last_draw_ < UPDATE_INTERVAL_MS)
      return;

    global_meta_->last_draw_ = now;
    double elapsed_s = chrono::duration<double>(now - global_meta_->start_).count();

    global_meta_->last_drawn_rows_ = global_meta_->render_fn_(global_meta_->root_, elapsed_s);
  }

  // Atomic!
  void shift_total(size_t delta) {
    total_ += delta;
    global_meta_->sum_total_ += delta;
  }

  void remove() {
    if (parent)
      parent->children_.erase(self_iterator_);
  }

  size_t child_count() {
    return children_.size();
  }
  bool is_root() const {
    return parent == nullptr;
  }
  size_t done() const {
    return done_.load();
  }
  size_t total() const {
    return total_.load();
  }
  size_t sum_done() const {
    return global_meta_->sum_done_.load();
  }
  size_t sum_total() const {
    return global_meta_->sum_total_.load();
  }
  size_t drawn_rows() {
    return global_meta_->last_drawn_rows_;
  }
};

} // namespace enu
