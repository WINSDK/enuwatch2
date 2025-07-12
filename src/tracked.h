#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <list>
#include <mutex>
#include <print>

namespace enu {

namespace chrono = std::chrono;

static constexpr auto UPDATE_INTERVAL_MS = chrono::milliseconds{80};

template <typename M>
class TrackedNode;

template <typename M>
using RenderFn = std::function<size_t(TrackedNode<M>* root, double elapsed_s)>;

template <typename M>
struct TrackedRoot {
  std::mutex mutex_;

  RenderFn<M> render_fn_;
  TrackedNode<M>* root_;
  chrono::steady_clock::time_point start_, last_draw_{start_};
  size_t last_drawn_rows_ = 0;

  std::atomic<size_t> sum_done_ = 0;
  size_t sum_total_ = 0;

  explicit TrackedRoot(RenderFn<M> render_fn, TrackedNode<M>* root)
    : render_fn_(std::move(render_fn)), root_(root), start_(chrono::steady_clock::now()) {}
};

template <typename M>
class TrackedNode {
  using LinkedList = std::list<TrackedNode<M>>;

  std::mutex mutex_;
  std::condition_variable cv_;
  bool ready_ = false;

  TrackedRoot<M>* root_meta_;

  LinkedList children_;
  LinkedList::iterator self_iterator_;

  std::atomic<size_t> done_ = 0;
  size_t total_ = 0;

 public:
  TrackedNode<M>* parent = nullptr;
  M meta;

  explicit TrackedNode(M metadata, RenderFn<M> render_fn)
    : root_meta_(new TrackedRoot{render_fn, this}), meta(std::move(metadata)) {}

  explicit TrackedNode(M metadata, TrackedNode<M>* parent_)
    : root_meta_(parent_->root_meta_), parent(parent_), meta(std::move(metadata)) {}

  TrackedNode(const TrackedNode&) = delete;
  TrackedNode& operator=(const TrackedNode&) = delete;

  ~TrackedNode() {
    if (is_root()) {
      delete root_meta_;
    }
  }

  [[nodiscard]] TrackedNode<M>* add_child(M metadata) {
    std::scoped_lock lock(mutex_);

    TrackedNode<M>& child = children_.emplace_back(std::move(metadata), this);
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

  void iter_children(std::function<void(TrackedNode<M>* node)> f) {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return ready_; });

    for (TrackedNode<M>& child : children_)
      f(&child);
  }

  void tick(size_t delta) {
    done_ += delta;
    root_meta_->sum_done_ += delta;

    // We don't *have* to try and report progress on every thread.
    if (!root_meta_->mutex_.try_lock())
      return;

    // Only draw every `UPDATE_INTERVAL_MS`.
    chrono::time_point now = chrono::steady_clock::now();
    if (now - root_meta_->last_draw_ >= UPDATE_INTERVAL_MS) {
      root_meta_->last_draw_ = now;
      double elapsed_s = chrono::duration<double>(now - root_meta_->start_).count();

      root_meta_->last_drawn_rows_ = root_meta_->render_fn_(root_meta_->root_, elapsed_s);
    }

    root_meta_->mutex_.unlock();
  }

  void shift_total(size_t delta) {
    total_ += delta;
    root_meta_->sum_total_ += delta;
  }

  void remove() {
    assert(parent != nullptr);
    std::scoped_lock lock(parent->mutex_);
    parent->children_.erase(self_iterator_);
  }

  size_t child_count() {
    return children_.size();
  }
  bool is_root() const {
    return parent == nullptr;
  }
  size_t done() const {
    return done_;
  }
  size_t total() const {
    return total_;
  }
  size_t sum_done() const {
    return root_meta_->sum_done_;
  }
  size_t sum_total() const {
    return root_meta_->sum_total_;
  }
  size_t drawn_rows() {
    return root_meta_->last_drawn_rows_;
  }
};

} // namespace enu
