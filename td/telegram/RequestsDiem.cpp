// Blah application adapter. Device keys, semantic audits and approval stay in
// the application. Keeping opaque transport here avoids touching upstream RPCs.
#include "td/telegram/Requests.h"
#include "td/telegram/DiemRawQuery.h"
#include "td/telegram/Global.h"
#include "td/telegram/net/NetQueryCreator.h"
#include "td/telegram/Td.h"

namespace td {
namespace {

class InvokeDiemIdentityQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::diemQueryResult>> promise_;

 public:
  explicit InvokeDiemIdentityQuery(Promise<td_api::object_ptr<td_api::diemQueryResult>> &&promise)
      : promise_(std::move(promise)) {}

  void send(const td_api::invokeDiemIdentityQuery &request) {
    if (request.query_.size() < 4 || request.query_.size() > 131072 ||
        request.proof_.empty() || request.proof_.size() > 262144) {
      return on_error(Status::Error(400, "Invalid Diem identity query"));
    }
    send_query(G()->net_query_creator().create(DiemRawQuery(
        DiemRawQuery::INVOKE_WITH_IDENTITY_PROOF, request.query_, request.proof_)));
  }

  void on_result(BufferSlice packet) final {
    promise_.set_value(td_api::make_object<td_api::diemQueryResult>(packet.as_slice().str()));
  }
  void on_error(Status status) final { promise_.set_error(std::move(status)); }
};

}  // namespace

void Requests::on_request(uint64 id, td_api::invokeDiemIdentityQuery &request) {
  td_->create_handler<InvokeDiemIdentityQuery>(
      create_request_promise<td_api::object_ptr<td_api::diemQueryResult>>(id))->send(request);
}

}  // namespace td
