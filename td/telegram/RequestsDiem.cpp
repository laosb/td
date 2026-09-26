// Blah application adapter. Device keys, semantic audits and approval stay in
// the application. Keeping opaque transport here avoids touching upstream RPCs.
#include "td/telegram/Requests.h"
#include "td/telegram/AccessRights.h"
#include "td/telegram/AuthManager.h"
#include "td/telegram/ChatManager.h"
#include "td/telegram/DiemRawQuery.h"
#include "td/telegram/DialogId.h"
#include "td/telegram/Global.h"
#include "td/telegram/misc.h"
#include "td/telegram/net/NetQueryCreator.h"
#include "td/telegram/telegram_api.h"
#include "td/telegram/Td.h"
#include "td/telegram/UserManager.h"
#include "td/utils/tl_helpers.h"

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

class PublishProfileQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit PublishProfileQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {}

  void send(const string &profile) {
    send_query(G()->net_query_creator().create(telegram_api::blah_publishProfile(BufferSlice(profile))));
  }

  void on_result(BufferSlice packet) final {
    auto result = fetch_result<telegram_api::blah_publishProfile>(packet);
    if (result.is_error()) { return on_error(result.move_as_error()); }
    if (!result.ok()) { return on_error(Status::Error(400, "PROFILE_INVALID")); }
    promise_.set_value(Unit());
  }
  void on_error(Status status) final { promise_.set_error(std::move(status)); }
};

class BindResourceIdentityQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit BindResourceIdentityQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {}

  void send(telegram_api::object_ptr<telegram_api::blah_InputResource> resource, const string &profile) {
    send_query(G()->net_query_creator().create(
        telegram_api::blah_bindResourceIdentity(std::move(resource), BufferSlice(profile))));
  }

  void on_result(BufferSlice packet) final {
    auto result = fetch_result<telegram_api::blah_bindResourceIdentity>(packet);
    if (result.is_error()) { return on_error(result.move_as_error()); }
    if (!result.ok()) { return on_error(Status::Error(400, "RESOURCE_INVALID")); }
    promise_.set_value(Unit());
  }
  void on_error(Status status) final { promise_.set_error(std::move(status)); }
};

bool is_valid_profile(const string &profile) {
  return !profile.empty() && profile.size() <= 262144;
}

}  // namespace

void Requests::on_request(uint64 id, td_api::invokeDiemIdentityQuery &request) {
  td_->create_handler<InvokeDiemIdentityQuery>(
      create_request_promise<td_api::object_ptr<td_api::diemQueryResult>>(id))->send(request);
}

void Requests::on_request(uint64 id, td_api::publishDiemProfile &request) {
  auto promise = create_ok_request_promise(id);
  if (!is_valid_profile(request.profile_)) {
    return promise.set_error(400, "Invalid Diem profile");
  }
  td_->create_handler<PublishProfileQuery>(std::move(promise))->send(request.profile_);
}

// The DC checks ownership, the roster and the home; these checks only keep an
// unknown or unowned peer from being named at all.
void Requests::on_request(uint64 id, td_api::bindChatDiemIdentity &request) {
  if (td_->auth_manager_->is_bot()) {
    return send_error_raw(id, 400, "The method is not available to bots");
  }
  auto promise = create_ok_request_promise(id);
  if (!is_valid_profile(request.profile_)) {
    return promise.set_error(400, "Invalid Diem profile");
  }
  DialogId dialog_id(request.chat_id_);
  telegram_api::object_ptr<telegram_api::InputPeer> input_peer;
  switch (dialog_id.get_type()) {
    case DialogType::Channel:
      if (td_->chat_manager_->get_channel_status(dialog_id.get_channel_id()).is_creator()) {
        input_peer = td_->chat_manager_->get_input_peer_channel(dialog_id.get_channel_id(), AccessRights::Write);
      }
      break;
    case DialogType::User: {
      auto bot_data = td_->user_manager_->get_bot_data(dialog_id.get_user_id());
      if (bot_data.is_ok() && bot_data.ok().can_be_edited) {
        input_peer = td_->user_manager_->get_input_peer_user(dialog_id.get_user_id(), AccessRights::Read);
      }
      break;
    }
    default:
      break;
  }
  if (input_peer == nullptr) {
    return promise.set_error(400, "RESOURCE_INVALID");
  }
  td_->create_handler<BindResourceIdentityQuery>(std::move(promise))
      ->send(telegram_api::make_object<telegram_api::blah_inputResourcePeer>(std::move(input_peer)),
             request.profile_);
}

void Requests::on_request(uint64 id, td_api::bindStickerSetDiemIdentity &request) {
  if (td_->auth_manager_->is_bot()) {
    return send_error_raw(id, 400, "The method is not available to bots");
  }
  auto promise = create_ok_request_promise(id);
  if (!clean_input_string(request.name_) || request.name_.empty()) {
    return promise.set_error(400, "Invalid sticker set name");
  }
  if (!is_valid_profile(request.profile_)) {
    return promise.set_error(400, "Invalid Diem profile");
  }
  td_->create_handler<BindResourceIdentityQuery>(std::move(promise))
      ->send(telegram_api::make_object<telegram_api::blah_inputResourceStickerSet>(
                 telegram_api::make_object<telegram_api::inputStickerSetShortName>(request.name_)),
             request.profile_);
}

void Requests::on_request(uint64 id, td_api::prepareDiemInvocation &request) {
  td_->create_handler<PrepareDiemQuery>(
      create_request_promise<td_api::object_ptr<td_api::diemInvocationPreparation>>(id))->send(request);
}

void Requests::on_request(uint64 id, td_api::invokeDiemSignedQuery &request) {
  td_->create_handler<InvokeDiemQuery>(
      create_request_promise<td_api::object_ptr<td_api::diemInvocationResult>>(id))->send(request);
}

}  // namespace td
