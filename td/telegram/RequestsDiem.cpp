// Blah application adapter. Device keys, semantic audits and approval stay in
// the application. Keeping opaque transport here avoids touching upstream RPCs.
#include "td/telegram/Requests.h"
#include "td/telegram/Global.h"
#include "td/telegram/net/NetQueryCreator.h"
#include "td/telegram/telegram_api.h"
#include "td/telegram/Td.h"
#include "td/utils/tl_helpers.h"

namespace td {
namespace {

class PrepareDiemQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::diemInvocationPreparation>> promise_;

 public:
  explicit PrepareDiemQuery(Promise<td_api::object_ptr<td_api::diemInvocationPreparation>> &&promise)
      : promise_(std::move(promise)) {}

  void send(const td_api::prepareDiemInvocation &request) {
    if (request.layer_ <= 0 || request.query_.size() < 4 || request.query_.size() > 131072 ||
        request.request_id_.size() != 32 || request.device_id_.size() != 32) {
      return on_error(Status::Error(400, "Invalid Diem invocation request"));
    }
    send_query(G()->net_query_creator().create(telegram_api::blah_prepareSignedInvocation(
        request.layer_, BufferSlice(request.query_), BufferSlice(request.request_id_), BufferSlice(request.device_id_))));
  }

  void on_result(BufferSlice packet) final {
    auto result = fetch_result<telegram_api::blah_prepareSignedInvocation>(packet);
    if (result.is_error()) { return on_error(result.move_as_error()); }
    auto value = result.move_as_ok();
    promise_.set_value(td_api::make_object<td_api::diemInvocationPreparation>(
        value->statement_.as_slice().str(), value->manifest_.as_slice().str()));
  }
  void on_error(Status status) final { promise_.set_error(std::move(status)); }
};

class InvokeDiemQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::diemInvocationResult>> promise_;

 public:
  explicit InvokeDiemQuery(Promise<td_api::object_ptr<td_api::diemInvocationResult>> &&promise)
      : promise_(std::move(promise)) {}

  void send(const td_api::invokeDiemSignedQuery &request) {
    if (request.statement_.empty() || request.statement_.size() > 262144 || request.manifest_.empty() ||
        request.manifest_.size() > 65700 || request.proof_.empty() || request.proof_.size() > 262144) {
      return on_error(Status::Error(400, "Invalid Diem invocation proof"));
    }
    send_query(G()->net_query_creator().create(telegram_api::blah_invokeWithSig(
        BufferSlice(request.statement_), BufferSlice(request.manifest_), BufferSlice(request.proof_))));
  }

  void on_result(BufferSlice packet) final {
    auto result = fetch_result<telegram_api::blah_invokeWithSig>(packet);
    if (result.is_error()) { return on_error(result.move_as_error()); }
    auto value = result.move_as_ok();
    promise_.set_value(td_api::make_object<td_api::diemInvocationResult>(
        value->request_id_.as_slice().str(), value->result_.as_slice().str()));
  }
  void on_error(Status status) final { promise_.set_error(std::move(status)); }
};

}  // namespace

void Requests::on_request(uint64 id, td_api::prepareDiemInvocation &request) {
  td_->create_handler<PrepareDiemQuery>(
      create_request_promise<td_api::object_ptr<td_api::diemInvocationPreparation>>(id))->send(request);
}

void Requests::on_request(uint64 id, td_api::invokeDiemSignedQuery &request) {
  td_->create_handler<InvokeDiemQuery>(
      create_request_promise<td_api::object_ptr<td_api::diemInvocationResult>>(id))->send(request);
}

}  // namespace td
