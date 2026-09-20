#include "td/telegram/Requests.h"

#include "td/telegram/Global.h"
#include "td/telegram/net/NetQueryDispatcher.h"
#include "td/utils/utf8.h"

namespace td {

void Requests::on_request(uint64 id, td_api::setRpcInterception &request) {
  G()->net_query_dispatcher().set_rpc_interception(std::move(request.constructor_ids_), request.timeout_,
                                                  create_ok_request_promise(id));
}

void Requests::on_request(uint64 id, td_api::completeRpcInterception &request) {
  G()->net_query_dispatcher().complete_rpc_interception(request.interception_id_, BufferSlice(request.result_),
                                                       create_ok_request_promise(id));
}

void Requests::on_request(uint64 id, td_api::failRpcInterception &request) {
  auto promise = create_ok_request_promise(id);
  if (!request.error_ || request.error_->code_ < 100 || request.error_->code_ > 599 ||
      (request.error_->code_ >= 202 && request.error_->code_ <= 204) ||
      request.error_->message_.size() > 1024 || !check_utf8(request.error_->message_)) {
    return promise.set_error(400, "Invalid RPC interception error");
  }
  G()->net_query_dispatcher().complete_rpc_interception(
      request.interception_id_, Status::Error(request.error_->code_, request.error_->message_), std::move(promise));
}

}  // namespace td
