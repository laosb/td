//
// BLAH: the datacenter list and RSA public keys this client trusts.
//
// Upstream TDLib hardcodes Telegram's addresses and keys. A Blah build takes
// both from a C3 control plane: CMake/BlahDcConfig.cmake fetches C3's
// /api/dcs document at configure time and embeds it in BlahDcConfigData.h,
// and this header parses it once, lazily, on first use.
//
// BLAH_DC_CONFIG overrides the embedded document at runtime with the same JSON
// shape. That is how the server's end-to-end suite points a released build at
// throwaway datacenters on 127.0.0.1 without rebuilding the library. It is read
// on first use rather than at load time, so a host may allocate its ports and
// mint its keys after the library is loaded, but before the first client.
//
// An empty document leaves every call site on upstream behaviour, so a build
// configured against no C3 is a stock TDLib.
//
#pragma once

#include "td/telegram/net/BlahDcConfigData.h"
#include "td/telegram/net/DcId.h"
#include "td/telegram/net/DcOptions.h"

#include "td/utils/common.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/logging.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/Slice.h"

#include <cstdlib>

namespace td {
namespace blah {

struct DcConfig {
  // False for a stock build, and for a document that described no usable
  // datacenter. Every Blah call site is a no-op unless this is true.
  bool is_active = false;
  // The datacenter a fresh client starts on: C3 lists the initial DC first.
  int32 default_dc_id = 0;
  DcOptions dc_options;
  // One PEM per datacenter, in C3's order.
  vector<string> rsa_public_keys;
};

inline DcConfig parse_dc_config(string json, Slice source) {
  DcConfig config;
  if (json.empty()) {
    return config;
  }

  auto r_value = json_decode(json);
  if (r_value.is_error()) {
    LOG(ERROR) << "BLAH: " << source << " is not JSON: " << r_value.error();
    return config;
  }
  auto value = r_value.move_as_ok();
  if (value.type() != JsonValue::Type::Object) {
    LOG(ERROR) << "BLAH: " << source << " is not a JSON object";
    return config;
  }
  auto &root = value.get_object();

  auto r_dcs = root.extract_optional_field("dcs", JsonValue::Type::Array);
  if (r_dcs.is_error()) {
    LOG(ERROR) << "BLAH: " << source << " has no dcs array: " << r_dcs.error();
    return config;
  }
  auto dcs = r_dcs.move_as_ok();
  if (dcs.type() != JsonValue::Type::Array) {
    LOG(ERROR) << "BLAH: " << source << " has no dcs array";
    return config;
  }

  for (auto &dc_value : dcs.get_array()) {
    if (dc_value.type() != JsonValue::Type::Object) {
      continue;
    }
    auto &dc = dc_value.get_object();

    auto r_id = dc.get_required_int_field("id");
    if (r_id.is_error() || !DcId::is_valid(r_id.ok())) {
      LOG(ERROR) << "BLAH: skipping a datacenter from " << source << " with an unusable id";
      continue;
    }
    auto dc_id = r_id.move_as_ok();

    // A datacenter that has not reported its key cannot complete a handshake,
    // so it is not worth dialing.
    auto r_pem = dc.get_required_string_field("rsaPublicKey");
    if (r_pem.is_error() || r_pem.ok().empty()) {
      LOG(ERROR) << "BLAH: skipping datacenter " << dc_id << " from " << source << ": no RSA public key";
      continue;
    }

    size_t endpoint_count = 0;
    auto r_endpoints = dc.extract_optional_field("endpoints", JsonValue::Type::Array);
    if (r_endpoints.is_ok() && r_endpoints.ok().type() == JsonValue::Type::Array) {
      for (auto &endpoint_value : r_endpoints.ok_ref().get_array()) {
        if (endpoint_value.type() != JsonValue::Type::Object) {
          continue;
        }
        auto &endpoint = endpoint_value.get_object();
        // wsTlsOnly endpoints are reachable only as WebSocket-over-TLS, which
        // is the web client's transport, not this library's.
        auto r_ws_tls_only = endpoint.get_optional_bool_field("wsTlsOnly");
        if (r_ws_tls_only.is_error() || r_ws_tls_only.ok()) {
          continue;
        }
        auto r_ip = endpoint.get_required_string_field("ip");
        auto r_port = endpoint.get_required_int_field("port");
        if (r_ip.is_error() || r_port.is_error()) {
          continue;
        }
        auto ip = r_ip.move_as_ok();
        auto port = r_port.move_as_ok();

        IPAddress ip_address;
        auto status = ip.find(':') == string::npos ? ip_address.init_ipv4_port(ip, port)
                                                   : ip_address.init_ipv6_port(ip, port);
        if (status.is_error()) {
          // C3 calls the field "ip", but a deployment may advertise a hostname.
          status = ip_address.init_host_port(ip, port);
        }
        if (status.is_error()) {
          LOG(ERROR) << "BLAH: datacenter " << dc_id << " has an unusable endpoint " << ip << ':' << port << ": "
                     << status.error();
          continue;
        }
        config.dc_options.dc_options.emplace_back(DcId::internal(dc_id), ip_address);
        endpoint_count++;
      }
    }
    if (endpoint_count == 0) {
      LOG(ERROR) << "BLAH: skipping datacenter " << dc_id << " from " << source << ": no reachable endpoint";
      continue;
    }

    config.rsa_public_keys.push_back(r_pem.move_as_ok());
    if (config.default_dc_id == 0) {
      config.default_dc_id = dc_id;
    }
  }

  // An explicit choice wins over "the first datacenter C3 listed"; the e2e
  // harness uses it to start a client on a datacenter other than the initial
  // one.
  auto r_default_dc_id = root.get_optional_int_field("defaultDcId");
  if (r_default_dc_id.is_ok() && DcId::is_valid(r_default_dc_id.ok())) {
    config.default_dc_id = r_default_dc_id.move_as_ok();
  }

  if (config.default_dc_id == 0) {
    LOG(ERROR) << "BLAH: " << source << " described no usable datacenter";
    return config;
  }

  config.is_active = true;
  LOG(INFO) << "BLAH: using " << config.rsa_public_keys.size() << " datacenter(s) from " << source << ", starting on "
            << config.default_dc_id;
  return config;
}

inline const DcConfig &get_dc_config() {
  static const DcConfig config = [] {
    const char *env = std::getenv("BLAH_DC_CONFIG");
    if (env != nullptr && *env != '\0') {
      return parse_dc_config(string(env), Slice("BLAH_DC_CONFIG"));
    }
    return parse_dc_config(string(DC_CONFIG_BUILTIN), Slice(DC_CONFIG_BUILD_SOURCE));
  }();
  return config;
}

// True when this build (or this process) has a Blah datacenter list. Every
// upstream call site this fork touches is guarded by it.
inline bool is_active() {
  return get_dc_config().is_active;
}

}  // namespace blah
}  // namespace td
