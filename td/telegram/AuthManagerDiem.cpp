// Blah's device-authentication transport adapter. Cryptography and private key
// custody stay in the application; these methods never receive a private key.
#include "td/telegram/AuthManager.h"
#include "td/telegram/Global.h"
#include "td/telegram/net/NetQueryCreator.h"
#include "td/telegram/Td.h"

namespace td {

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
