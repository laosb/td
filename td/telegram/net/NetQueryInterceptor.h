#pragma once

#include "td/telegram/net/NetQuery.h"
#include "td/telegram/net/RpcInterception.h"
#include "td/actor/actor.h"
#include "td/utils/Promise.h"

namespace td {

class NetQueryInterceptor final : public Actor {
 public:
  explicit NetQueryInterceptor(ActorShared<> parent) : parent_(std::move(parent)) {
  }

  void configure(std::shared_ptr<const RpcInterceptionConfiguration> configuration, Promise<Unit> promise);
  void intercept(NetQueryPtr query);
  void complete(int64 id, Result<BufferSlice> result, Promise<Unit> promise);
  void cancel_all();

 private:
  ActorShared<> parent_;
  std::shared_ptr<const RpcInterceptionConfiguration> configuration_;
  RpcInterceptionPending<NetQueryPtr> pending_;

  void timeout_expired() final;
  void tear_down() final;
  void sweep();
  void cancel(int64 id, NetQueryPtr query, Status error);
};

}  // namespace td
