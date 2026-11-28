#pragma once
#include <functional>
#include <utility>
#include <vector>
#define ESPDEPRECATED(message, version)
namespace esphome {
template<typename Signature> class CallbackManager;
template<typename... Args> class CallbackManager<void(Args...)> {
 public:
  template<typename F> void add(F &&callback) { callbacks_.emplace_back(std::forward<F>(callback)); }
  void call(Args... args) { for (const auto &callback : callbacks_) callback(args...); }
  size_t size() const { return callbacks_.size(); }
 protected:
  std::vector<std::function<void(Args...)>> callbacks_;
};
}
