// Blah application adapter. Device keys, semantic audits and approval stay in
// the application. Keeping opaque transport here avoids touching upstream RPCs.
#include "td/telegram/Requests.h"
#include "td/telegram/ChatManager.h"
#include "td/telegram/DialogId.h"
#include "td/telegram/DiemRawQuery.h"
#include "td/telegram/Global.h"
#include "td/telegram/net/NetQueryCreator.h"
#include "td/telegram/StickersManager.h"
#include "td/telegram/Td.h"
#include "td/telegram/UserManager.h"

namespace td {
namespace {

template <class Query>
class GetDiemProfileQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::diemResourceProfile>> promise_;
  int64 resource_id_ = 0;

 public:
  explicit GetDiemProfileQuery(Promise<td_api::object_ptr<td_api::diemResourceProfile>> &&promise)
      : promise_(std::move(promise)) {}

  template <class Input>
  void send(int64 resource_id, telegram_api::object_ptr<Input> input) {
    resource_id_ = resource_id;
    vector<telegram_api::object_ptr<Input>> inputs;
    inputs.push_back(std::move(input));
    send_query(G()->net_query_creator().create(Query(std::move(inputs))));
  }

  void on_result(BufferSlice packet) final {
    auto result = fetch_result<Query>(packet);
    if (result.is_error()) {
      return on_error(result.move_as_error());
    }
    auto profiles = result.move_as_ok();
    if (profiles.empty()) {
      return promise_.set_value(td_api::make_object<td_api::diemResourceProfile>(string()));
    }
    if (profiles.size() != 1 || profiles[0]->id_ != resource_id_ || profiles[0]->profile_.empty()) {
      return on_error(Status::Error(500, "Invalid Diem profile response"));
    }
    promise_.set_value(td_api::make_object<td_api::diemResourceProfile>(profiles[0]->profile_.as_slice().str()));
  }

  void on_error(Status status) final { promise_.set_error(std::move(status)); }
};

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

void Requests::on_request(uint64 id, const td_api::getUserDiemProfile &request) {
  auto promise = create_request_promise<td_api::object_ptr<td_api::diemResourceProfile>>(id);
  auto input_user = td_->user_manager_->get_input_user(UserId(request.user_id_));
  if (input_user.is_error()) {
    return promise.set_error(input_user.move_as_error());
  }
  td_->create_handler<GetDiemProfileQuery<telegram_api::blah_getUserProfile>>(std::move(promise))
      ->send(request.user_id_, input_user.move_as_ok());
}

void Requests::on_request(uint64 id, const td_api::getChatDiemProfile &request) {
  auto promise = create_request_promise<td_api::object_ptr<td_api::diemResourceProfile>>(id);
  DialogId dialog_id(request.chat_id_);
  if (dialog_id.get_type() != DialogType::Channel) {
    return promise.set_error(400, "Chat must be a supergroup or channel");
  }
  auto input_channel = td_->chat_manager_->get_input_channel(dialog_id.get_channel_id());
  if (input_channel == nullptr) {
    return promise.set_error(400, "Supergroup or channel not found");
  }
  td_->create_handler<GetDiemProfileQuery<telegram_api::blah_getChannelProfile>>(std::move(promise))
      ->send(dialog_id.get_channel_id().get(), std::move(input_channel));
}

void Requests::on_request(uint64 id, const td_api::getStickerSetDiemProfile &request) {
  auto promise = create_request_promise<td_api::object_ptr<td_api::diemResourceProfile>>(id);
  auto input_set = td_->stickers_manager_->get_input_sticker_set(StickerSetId(request.set_id_));
  if (input_set == nullptr) {
    return promise.set_error(400, "Sticker set not found");
  }
  td_->create_handler<GetDiemProfileQuery<telegram_api::blah_getStickerSetProfile>>(std::move(promise))
      ->send(request.set_id_, std::move(input_set));
}

void Requests::on_request(uint64 id, td_api::invokeDiemIdentityQuery &request) {
  td_->create_handler<InvokeDiemIdentityQuery>(
      create_request_promise<td_api::object_ptr<td_api::diemQueryResult>>(id))->send(request);
}

}  // namespace td
