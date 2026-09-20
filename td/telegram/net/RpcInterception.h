// Application-owned RPC execution. No method semantics or signing keys live here.
#pragma once

#include "td/utils/common.h"
#include "td/utils/Status.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <utility>

namespace td {

// One budget belongs to the client, including across configuration changes.
// Reservations follow queries through the sequencer and their result handlers.
class RpcInterceptionBudget {
 public:
  static constexpr size_t MAX_PENDING = 64;
  class Reservation {
   public:
    explicit Reservation(std::shared_ptr<std::atomic<size_t>> count) : count_(std::move(count)) {
    }
    ~Reservation() {
      count_->fetch_sub(1, std::memory_order_relaxed);
    }
    Reservation(const Reservation &) = delete;
    Reservation &operator=(const Reservation &) = delete;

   private:
    std::shared_ptr<std::atomic<size_t>> count_;
  };

  std::unique_ptr<Reservation> reserve() const {
    auto count = count_->load(std::memory_order_relaxed);
    while (count < MAX_PENDING) {
      if (count_->compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) {
        return std::make_unique<Reservation>(count_);
      }
    }
    return nullptr;
  }

 private:
  std::shared_ptr<std::atomic<size_t>> count_ = std::make_shared<std::atomic<size_t>>(0);
};

struct RpcInterceptionConfiguration {
  static constexpr size_t MAX_PENDING = RpcInterceptionBudget::MAX_PENDING;
  static constexpr size_t MAX_BYTES = 131072;
  vector<int32> constructors;
  int32 timeout = 60;
  std::shared_ptr<RpcInterceptionBudget> budget;

  bool contains(int32 constructor) const {
    return std::binary_search(constructors.begin(), constructors.end(), constructor);
  }

  static Result<std::shared_ptr<const RpcInterceptionConfiguration>> create(
      vector<int32> constructors, int32 timeout,
      std::shared_ptr<RpcInterceptionBudget> budget = std::make_shared<RpcInterceptionBudget>()) {
    if (constructors.size() > 64 || timeout < 1 || timeout > 120) {
      return Status::Error(400, "Invalid RPC interception configuration");
    }
    std::sort(constructors.begin(), constructors.end());
    constructors.erase(std::unique(constructors.begin(), constructors.end()), constructors.end());
    auto result = std::make_shared<RpcInterceptionConfiguration>();
    result->constructors = std::move(constructors);
    result->timeout = timeout;
    result->budget = std::move(budget);
    return std::shared_ptr<const RpcInterceptionConfiguration>(std::move(result));
  }
};

// The actor supplies time and handles completion; this container enforces bounded,
// consume-once ownership independently of the transport and scheduler.
template <class T, class Id = int64>
class RpcInterceptionPending {
 public:
  struct Entry {
    double deadline;
    T value;
  };
  using Entries = std::map<Id, Entry>;

  bool can_insert() const {
    return entries_.size() < RpcInterceptionConfiguration::MAX_PENDING;
  }
  bool insert(Id id, double deadline, T value) {
    if (!can_insert() || id <= 0 || entries_.count(id) != 0) {
      return false;
    }
    entries_.emplace(id, Entry{deadline, std::move(value)});
    return true;
  }
  Result<T> take(Id id) {
    auto it = entries_.find(id);
    if (it == entries_.end()) {
      return Status::Error(400, "RPC_INTERCEPTION_NOT_FOUND");
    }
    auto value = std::move(it->second.value);
    entries_.erase(it);
    return std::move(value);
  }
  void erase(Id id) {
    entries_.erase(id);
  }
  template <class Predicate, class Finish>
  void remove_if(Predicate predicate, Finish finish) {
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (predicate(it->second)) {
        auto id = it->first;
        auto value = std::move(it->second.value);
        it = entries_.erase(it);
        finish(id, std::move(value));
      } else {
        ++it;
      }
    }
  }
  bool empty() const {
    return entries_.empty();
  }

 private:
  Entries entries_;
};

}  // namespace td
