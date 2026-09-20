// Application-supplied immutable network configuration. An absent document
// retains Telegram defaults; an invalid document never falls back to them.
#pragma once

#include "td/telegram/net/BlahDcConfigData.h"
#include "td/telegram/net/DcId.h"
#include "td/telegram/net/DcOptions.h"
#include "td/telegram/net/PublicRsaKeySharedMain.h"

#include "td/utils/common.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"

#include <cstdlib>
#include <map>
#include <memory>

namespace td {
namespace blah {

struct DcConfig {
  int32 default_dc_id = 0;
  DcOptions dc_options;
  std::map<int32, std::shared_ptr<PublicRsaKeySharedMain>> public_keys;

  bool contains(DcId dc_id) const {
    return dc_id.is_exact() && dc_id.is_internal() && public_keys.count(dc_id.get_raw_id()) != 0;
  }
};

inline Result<std::shared_ptr<const DcConfig>> parse_dc_config(string json) {
  if (json.empty()) {
    return std::shared_ptr<const DcConfig>();
  }
  if (json.size() > 65536) {
    return Status::Error(400, "NETWORK_CONFIGURATION_INVALID: document is too large");
  }
  TRY_RESULT(value, json_decode(json));
  if (value.type() != JsonValue::Type::Object) {
    return Status::Error(400, "NETWORK_CONFIGURATION_INVALID: expected object");
  }
  auto &root = value.get_object();
  TRY_RESULT(dcs, root.extract_optional_field("dcs", JsonValue::Type::Array));
  if (dcs.type() != JsonValue::Type::Array || dcs.get_array().empty() || dcs.get_array().size() > 32) {
    return Status::Error(400, "NETWORK_CONFIGURATION_INVALID: expected 1 to 32 datacenters");
  }
  auto config = std::make_shared<DcConfig>();
  size_t endpoint_count = 0;
  for (auto &dc_value : dcs.get_array()) {
    if (dc_value.type() != JsonValue::Type::Object) {
      return Status::Error(400, "NETWORK_CONFIGURATION_INVALID: invalid datacenter");
    }
    auto &dc = dc_value.get_object();
    TRY_RESULT(id, dc.get_required_int_field("id"));
    if (!DcId::is_valid(id) || config->public_keys.count(id) != 0) {
      return Status::Error(400, "NETWORK_CONFIGURATION_INVALID: invalid or duplicate datacenter id");
    }
    TRY_RESULT(pem, dc.get_required_string_field("rsaPublicKey"));
    if (pem.empty() || pem.size() > 8192) {
      return Status::Error(400, "NETWORK_CONFIGURATION_INVALID: invalid public key size");
    }
    TRY_RESULT(rsa, mtproto::RSA::from_pem_public_key(pem));
    if (rsa.size() != 256) {
      return Status::Error(400, "NETWORK_CONFIGURATION_INVALID: expected 2048-bit RSA key");
    }
    vector<mtproto::PublicRsaKeyInterface::RsaKey> keys;
    auto fingerprint = rsa.get_fingerprint();
    keys.push_back({std::move(rsa), fingerprint});
    config->public_keys.emplace(id, std::make_shared<PublicRsaKeySharedMain>(std::move(keys)));

    TRY_RESULT(endpoints, dc.extract_optional_field("endpoints", JsonValue::Type::Array));
    if (endpoints.type() != JsonValue::Type::Array || endpoints.get_array().empty()) {
      return Status::Error(400, "NETWORK_CONFIGURATION_INVALID: missing endpoints");
    }
    size_t usable_count = 0;
    for (auto &endpoint_value : endpoints.get_array()) {
      if (++endpoint_count > 64 || endpoint_value.type() != JsonValue::Type::Object) {
        return Status::Error(400, "NETWORK_CONFIGURATION_INVALID: invalid or too many endpoints");
      }
      auto &endpoint = endpoint_value.get_object();
      TRY_RESULT(ws_tls_only, endpoint.get_optional_bool_field("wsTlsOnly"));
      TRY_RESULT(ip, endpoint.get_required_string_field("ip"));
      TRY_RESULT(port, endpoint.get_required_int_field("port"));
      if (ip.empty() || port <= 0 || port > 65535) {
        return Status::Error(400, "NETWORK_CONFIGURATION_INVALID: invalid endpoint");
      }
      if (ws_tls_only) {
        continue;  // WebSocket-only endpoints cannot be used by TDLib.
      }
      IPAddress address;
      auto status = ip.find(':') == string::npos ? address.init_ipv4_port(ip, port)
                                                : address.init_ipv6_port(ip, port);
      if (status.is_error()) {
        // Preserve support for application-pinned hostnames in bootstrap documents.
        status = address.init_host_port(ip, port);
      }
      TRY_STATUS(std::move(status));
      config->dc_options.dc_options.emplace_back(DcId::internal(id), address);
      usable_count++;
    }
    if (usable_count == 0) {
      return Status::Error(400, "NETWORK_CONFIGURATION_INVALID: no supported endpoint");
    }
    if (config->default_dc_id == 0) {
      config->default_dc_id = id;
    }
  }
  TRY_RESULT(default_dc_id, root.get_optional_int_field("defaultDcId", config->default_dc_id));
  if (!DcId::is_valid(default_dc_id) || !config->contains(DcId::internal(default_dc_id))) {
    return Status::Error(400, "NETWORK_CONFIGURATION_INVALID: default datacenter is not configured");
  }
  config->default_dc_id = default_dc_id;
  return std::shared_ptr<const DcConfig>(std::move(config));
}

// Legacy defaults are snapshotted once. Explicit per-client documents never
// consult this process environment and never share another client's pins.
inline Result<std::shared_ptr<const DcConfig>> default_dc_config() {
  static const auto result = [] {
    const char *env = std::getenv("BLAH_DC_CONFIG");
    return parse_dc_config(env != nullptr && *env != '\0' ? string(env) : string(DC_CONFIG_BUILTIN));
  }();
  if (result.is_error()) {
    return result.error().clone();
  }
  return result.ok();
}

}  // namespace blah
}  // namespace td
