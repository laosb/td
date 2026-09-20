#include "td/telegram/net/NetQueryInterceptor.h"

#include "td/telegram/Global.h"
#include "td/telegram/net/NetQueryDispatcher.h"
#include "td/telegram/Td.h"
#include "td/telegram/td_api.h"
#include "td/telegram/Version.h"
#include "td/utils/Random.h"
#include "td/utils/Time.h"

#include <atomic>

namespace td {
namespace {

// Process-wide monotonic IDs also isolate clients. A random starting point makes
// a stale application's continuation unusable after a process restart.
int64 next_interception_id() {
  static std::atomic<int64> next{Random::secure_int64() & ((int64{1} << 52) - 1)};
  auto id = next.fetch_add(1, std::memory_order_relaxed) + 1;
  return id < (int64{1} << 53) ? id : 0;
}

}  // namespace

void NetQueryInterceptor::configure(std::shared_ptr<const RpcInterceptionConfiguration> configuration,
                                    Promise<Unit> promise) {
  cancel_all();
  configuration_ = std::move(configuration);
  promise.set_value(Unit());
}

void NetQueryInterceptor::intercept(NetQueryPtr query) {
  sweep();
  if (query->rpc_interception_configuration() != configuration_) {
    return cancel(0, std::move(query), Status::Error(400, "RPC_INTERCEPTION_CONFIGURATION_CHANGED"));
  }
  if (query->update_is_ready()) {
    return cancel(0, std::move(query), Status::Error(400, "RPC_INTERCEPTION_CANCELED"));
  }
  if (query->rpc_interception_query().empty() || query->has_verification_prefix() || !query->invoke_after().empty()) {
    return cancel(0, std::move(query), Status::Error(400, "RPC_INTERCEPTION_QUERY_UNSUPPORTED"));
  }
  auto now = Time::now();
  auto deadline = query->rpc_interception_deadline();
  if (deadline <= now) {
    return cancel(0, std::move(query), Status::Error(408, "RPC_INTERCEPTION_EXPIRED"));
  }
  if (!pending_.can_insert()) {
    return cancel(0, std::move(query), Status::Error(429, "RPC_INTERCEPTION_LIMIT"));
  }
  auto id = next_interception_id();
  if (id == 0) {
    return cancel(0, std::move(query), Status::Error(429, "RPC_INTERCEPTION_LIMIT"));
  }
  auto update = td_api::make_object<td_api::updateRpcInterceptionRequired>(
      id, MTPROTO_LAYER, query->tl_constructor(), query->rpc_interception_query().as_slice().str(), deadline - now);
  CHECK(pending_.insert(id, deadline, std::move(query)));
  send_closure(G()->td(), &Td::send_update, std::move(update));
  set_timeout_in(0.1);
}

void NetQueryInterceptor::complete(int64 id, Result<BufferSlice> result, Promise<Unit> promise) {
  if (result.is_ok() && (result.ok().size() < 4 || result.ok().size() > RpcInterceptionConfiguration::MAX_BYTES)) {
    return promise.set_error(400, "Invalid RPC interception response size");
  }
  sweep();
  auto found = pending_.take(id);
  if (found.is_error()) {
    return promise.set_error(found.move_as_error());
  }
  auto query = found.move_as_ok();
  // The weak-reference token can change between the sweep and consuming the ID.
  if (query->update_is_ready()) {
    cancel(id, std::move(query), Status::Error(400, "RPC_INTERCEPTION_CANCELED"));
    return promise.set_error(400, "RPC_INTERCEPTION_CANCELED");
  }
  if (result.is_ok()) {
    query->set_ok(result.move_as_ok());
  } else {
    query->set_error(result.move_as_error());
  }
  // The original handler validates the result and performs its normal lifecycle.
  // A terminal application error must never enter the transport's retry policy.
  G()->net_query_dispatcher().complete_intercepted_query(std::move(query));
  promise.set_value(Unit());
}

void NetQueryInterceptor::cancel(int64 id, NetQueryPtr query, Status error) {
  if (id != 0) {
    send_closure(G()->td(), &Td::send_update, td_api::make_object<td_api::updateRpcInterceptionCanceled>(
                                               id, td_api::make_object<td_api::error>(error.code(), error.message().str())));
  }
  query->set_error(std::move(error));
  G()->net_query_dispatcher().complete_intercepted_query(std::move(query));
}

void NetQueryInterceptor::sweep() {
  auto now = Time::now();
  pending_.remove_if(
      [now](auto &entry) { return entry.value->update_is_ready() || entry.deadline <= now; },
      [this](int64 id, NetQueryPtr query) {
        auto error = query->is_ready() ? Status::Error(400, "RPC_INTERCEPTION_CANCELED")
                                      : Status::Error(408, "RPC_INTERCEPTION_EXPIRED");
        cancel(id, std::move(query), std::move(error));
      });
  if (pending_.empty()) {
    cancel_timeout();
  }
}

void NetQueryInterceptor::timeout_expired() {
  sweep();
  if (!pending_.empty()) {
    set_timeout_in(0.1);
  }
}

void NetQueryInterceptor::cancel_all() {
  configuration_.reset();
  pending_.remove_if([](auto &) { return true; }, [this](int64 id, NetQueryPtr query) {
    cancel(id, std::move(query), Status::Error(400, "RPC_INTERCEPTION_CANCELED"));
  });
  cancel_timeout();
}

void NetQueryInterceptor::tear_down() {
  cancel_all();
  parent_.reset();
}

}  // namespace td
