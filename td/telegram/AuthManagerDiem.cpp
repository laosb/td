// Blah's device-authentication transport adapter. Cryptography and private key
// custody stay in the application; these methods never receive a private key.
#include "td/telegram/AuthManager.h"
#include "td/telegram/DiemRawQuery.h"
#include "td/telegram/Global.h"
#include "td/telegram/net/NetQueryCreator.h"
#include "td/telegram/Td.h"
#include "td/utils/Storer.h"

namespace td {

namespace {
string boxed_diem_query(const telegram_api::Function &query) {
  DefaultStorer<telegram_api::Function> storer(query);
  string bytes(storer.size(), '\0');
  storer.store(MutableSlice(bytes).ubegin());
  return bytes;
}
}  // namespace

void AuthManager::get_diem_layer_config(uint64 query_id) {
  if (state_ != State::WaitPhoneNumber) {
    return on_query_error(query_id, Status::Error(400, "Diem login requires an unauthorized session"));
  }
  on_new_query(query_id);
  start_net_query(NetQueryType::DiemLayerConfig, G()->net_query_creator().create_unauth(
      DiemRawQuery(DiemRawQuery::INVOKE_WITH_LAYER, boxed_diem_query(telegram_api::blah_getConfig()))));
}

void AuthManager::prepare_diem_authentication(uint64 query_id, string domain, string contact, bool sign_up,
                                               string first_name, string last_name) {
  if (domain.empty() || domain.size() > 253 || !diem_pending_query_.empty()) {
    return on_query_error(query_id, Status::Error(400, "Invalid Diem authentication request"));
  }
  if (sign_up) {
    if ((state_ != State::WaitCode && state_ != State::WaitRegistration) ||
        send_code_helper_.get_phone_code_hash().empty() || first_name.empty()) {
      return on_query_error(query_id, Status::Error(400, "Diem signup is not ready"));
    }
    diem_pending_query_ = boxed_diem_query(telegram_api::auth_signUp(
        0, false, send_code_helper_.get_phone_number(), send_code_helper_.get_phone_code_hash(),
        first_name, last_name));
  } else {
    if (state_ != State::WaitPhoneNumber) {
      return on_query_error(query_id, Status::Error(400, "Diem code request is not ready"));
    }
    diem_pending_query_ = boxed_diem_query(send_code_helper_.send_code(
        contact.empty() ? domain : contact, SendCodeHelper::Settings(), api_id_, api_hash_));
  }
  diem_pending_signup_ = sign_up;
  on_new_query(query_id);
  start_net_query(NetQueryType::DiemIdentityChallenge,
                  G()->net_query_creator().create_unauth(telegram_api::blah_requestIdentityChallenge(domain)));
}

void AuthManager::submit_diem_authentication(uint64 query_id, string proof) {
  if (diem_pending_query_.empty() || proof.empty() || proof.size() > 262144) {
    return on_query_error(query_id, Status::Error(400, "Invalid Diem identity proof"));
  }
  auto query = std::move(diem_pending_query_);
  diem_pending_query_.clear();
  auto sign_up = diem_pending_signup_;
  diem_pending_signup_ = false;
  on_new_query(query_id);
  start_net_query(sign_up ? NetQueryType::SignUp : NetQueryType::SendCode,
                  G()->net_query_creator().create_unauth(DiemRawQuery(
                      DiemRawQuery::INVOKE_WITH_IDENTITY_PROOF, std::move(query), std::move(proof))));
}

void AuthManager::prepare_diem_invocation(uint64 query_id, string domain, string query) {
  if (state_ != State::Ok || domain.empty() || domain.size() > 253 || query.size() < 4 ||
      query.size() > 131072 || !diem_pending_query_.empty()) {
    return on_query_error(query_id, Status::Error(400, "Invalid Diem invocation request"));
  }
  diem_pending_query_ = std::move(query);
  diem_pending_signup_ = false;
  on_new_query(query_id);
  start_net_query(NetQueryType::DiemIdentityChallenge,
                  G()->net_query_creator().create(telegram_api::blah_requestIdentityChallenge(domain)));
}

void AuthManager::on_diem_layer_config(NetQueryPtr query) {
  auto result = fetch_result<telegram_api::blah_getConfig>(std::move(query));
  if (result.is_error()) { return on_current_query_error(result.move_as_error()); }
  auto value = result.move_as_ok();
  auto id = query_id_;
  query_id_ = 0;
  send_closure(G()->td(), &Td::send_result, id, td_api::make_object<td_api::diemLayerConfig>(
      value->discovery_domain_, value->dc_identity_.as_slice().str(), value->layer_));
}

void AuthManager::on_diem_identity_challenge(NetQueryPtr query) {
  auto auth_key_id = static_cast<int64>(query->response_auth_key_id());
  auto session_id = static_cast<int64>(query->response_session_id());
  auto result = fetch_result<telegram_api::blah_requestIdentityChallenge>(std::move(query));
  if (result.is_error()) {
    diem_pending_query_.clear();
    return on_current_query_error(result.move_as_error());
  }
  if (auth_key_id == 0 || session_id == 0) {
    diem_pending_query_.clear();
    return on_current_query_error(Status::Error(500, "Missing local MTProto channel binding"));
  }
  auto value = result.move_as_ok();
  auto id = query_id_;
  query_id_ = 0;
  send_closure(G()->td(), &Td::send_result, id, td_api::make_object<td_api::diemIdentityChallenge>(
      value->data_.as_slice().str(), diem_pending_query_, auth_key_id, session_id));
  if (state_ == State::Ok) {
    diem_pending_query_.clear();
  }
}

void AuthManager::get_diem_federation_info(uint64 query_id) {
  if (state_ != State::WaitPhoneNumber) {
    return on_query_error(query_id, Status::Error(400, "Diem login requires an unauthorized session"));
  }
  on_new_query(query_id);
  start_net_query(NetQueryType::DiemFederationInfo,
                  G()->net_query_creator().create_unauth(telegram_api::blah_getFederationInfo()));
}

void AuthManager::request_diem_authentication(uint64 query_id, string profile, string device_id, bool sign_up) {
  if (state_ != State::WaitPhoneNumber || profile.empty() || profile.size() > 262144 || device_id.size() != 32) {
    return on_query_error(query_id, Status::Error(400, "Invalid Diem authentication request"));
  }
  on_new_query(query_id);
  start_net_query(NetQueryType::DiemChallenge, G()->net_query_creator().create_unauth(
      telegram_api::blah_getDeviceChallenge(BufferSlice(profile), BufferSlice(device_id), sign_up ? "signUp" : "signIn")));
}

void AuthManager::check_diem_authentication(uint64 query_id, string nonce, string proof, bool sign_up,
                                          string first_name, string last_name) {
  if (state_ != State::WaitPhoneNumber || nonce.size() != 32 || proof.empty() || proof.size() > 262144) {
    return on_query_error(query_id, Status::Error(400, "Invalid Diem authentication proof"));
  }
  on_new_query(query_id);
  if (sign_up) {
    start_net_query(NetQueryType::SignUp, G()->net_query_creator().create_unauth(
        telegram_api::blah_signUpWithDevice(BufferSlice(nonce), BufferSlice(proof), first_name, last_name)));
  } else {
    start_net_query(NetQueryType::SignIn, G()->net_query_creator().create_unauth(
        telegram_api::blah_signInWithDevice(BufferSlice(nonce), BufferSlice(proof))));
  }
}

void AuthManager::on_diem_federation_info(NetQueryPtr query) {
  auto result = fetch_result<telegram_api::blah_getFederationInfo>(std::move(query));
  if (result.is_error()) { return on_current_query_error(result.move_as_error()); }
  auto value = result.move_as_ok();
  auto id = query_id_;
  query_id_ = 0;
  send_closure(G()->td(), &Td::send_result, id,
               td_api::make_object<td_api::diemFederationInfo>(value->domain_, value->public_key_.as_slice().str()));
}

void AuthManager::on_diem_challenge(NetQueryPtr query) {
  auto auth_key_id = static_cast<int64>(query->response_auth_key_id());
  auto session_id = static_cast<int64>(query->response_session_id());
  auto result = fetch_result<telegram_api::blah_getDeviceChallenge>(std::move(query));
  if (result.is_error()) { return on_current_query_error(result.move_as_error()); }
  if (auth_key_id == 0 || session_id == 0) {
    return on_current_query_error(Status::Error(500, "Missing local MTProto channel binding"));
  }
  auto value = result.move_as_ok();
  auto id = query_id_;
  query_id_ = 0;
  send_closure(G()->td(), &Td::send_result, id,
               td_api::make_object<td_api::diemAuthenticationChallenge>(value->payload_.as_slice().str(), auth_key_id, session_id));
}

}  // namespace td
