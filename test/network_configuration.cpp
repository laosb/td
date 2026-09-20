#include "td/telegram/Client.h"
#include "td/telegram/DatabaseNamespace.h"
#include "td/telegram/logevent/LogEvent.h"
#include "td/telegram/net/BlahDcConfig.h"
#include "td/telegram/StateManager.h"
#include "td/telegram/td_api.h"

#include "td/db/BinlogKeyValue.h"
#include "td/utils/filesystem.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/port/path.h"
#include "td/utils/port/Stat.h"
#include "td/utils/tests.h"
#include "td/utils/Time.h"

namespace {
const td::string KEY_0 = "-----BEGIN RSA PUBLIC KEY-----\n"
              "MIIBCgKCAQEAyMEdY1aR+sCR3ZSJrtztKTKqigvO/vBfqACJLZtS7QMgCGXJ6XIR\n"
              "yy7mx66W0/sOFa7/1mAZtEoIokDP3ShoqF4fVNb6XeqgQfaUHd8wJpDWHcR2OFwv\n"
              "plUUI1PLTktZ9uW2WE23b+ixNwJjJGwBDJPQEQFBE+vfmH0JP503wr5INS1poWg/\n"
              "j25sIWeYPHYeOrFp/eXaqhISP6G+q2IeTaWTXpwZj4LzXq5YOpk4bYEQ6mvRq7D1\n"
              "aHWfYmlEGepfaYR8Q0YqvvhYtMte3ITnuSJs171+GDqpdKcSwHnd6FudwGO4pcCO\n"
              "j4WcDuXc2CTHgH8gFTNhp/Y8/SpDOhvn9QIDAQAB\n"
              "-----END RSA PUBLIC KEY-----";
const td::string KEY_1 = "-----BEGIN RSA PUBLIC KEY-----\n"
              "MIIBCgKCAQEA6LszBcC1LGzyr992NzE0ieY+BSaOW622Aa9Bd4ZHLl+TuFQ4lo4g\n"
              "5nKaMBwK/BIb9xUfg0Q29/2mgIR6Zr9krM7HjuIcCzFvDtr+L0GQjae9H0pRB2OO\n"
              "62cECs5HKhT5DZ98K33vmWiLowc621dQuwKWSQKjWf50XYFw42h21P2KXUGyp2y/\n"
              "+aEyZ+uVgLLQbRA1dEjSDZ2iGRy12Mk5gpYc397aYp438fsJoHIgJ2lgMv5h7WY9\n"
              "t6N/byY9Nw9p21Og3AoXSL2q/2IJ1WRUhebgAdGVMlV1fkuOQoEzR7EdpqtQD9Cs\n"
              "5+bfo3Nhmcyvk5ftB0WkJ9z6bNZ7yxrP8wIDAQAB\n"
              "-----END RSA PUBLIC KEY-----";

td::string config(const td::string &key, int port = 10001, int dc = 1) {
  return "{\"dcs\":[{\"id\":" + td::to_string(dc) + ",\"rsaPublicKey\":" +
         td::json_encode<td::string>(td::JsonString(key)) +
         ",\"endpoints\":[{\"ip\":\"127.0.0.1\",\"port\":" + td::to_string(port) + "}]}]}";
}

td::td_api::object_ptr<td::td_api::Object> receive(td::Client &client, td::uint64 id) {
  auto deadline = td::Time::now() + 10;
  while (td::Time::now() < deadline) {
    auto response = client.receive(0.1);
    if (response.id == id) {
      return std::move(response.object);
    }
  }
  UNREACHABLE();
}

void expect_database_error(const td::string &directory, const td::string &tag, const td::string &expected,
                           td::string network_configuration = config(KEY_0)) {
  td::Client client;
  client.send({1, td::td_api::make_object<td::td_api::setNetworkType>(
                      td::td_api::make_object<td::td_api::networkTypeNone>())});
  auto parameters = td::td_api::make_object<td::td_api::setTdlibParameters>();
  parameters->database_directory_ = directory;
  parameters->database_namespace_ = tag;
  parameters->network_configuration_ = std::move(network_configuration);
  parameters->use_file_database_ = true;
  parameters->api_id_ = 1;
  parameters->api_hash_ = "test";
  parameters->system_language_code_ = "en";
  parameters->device_model_ = "test";
  parameters->application_version_ = "test";
  client.send({2, std::move(parameters)});
  auto result = receive(client, 2);
  ASSERT_EQ(td::td_api::error::ID, result->get_id());
  const auto &error = static_cast<const td::td_api::error &>(*result);
  ASSERT_EQ(400, error.code_);
  ASSERT_TRUE(error.message_.find(expected) == 0);
  client.send({3, td::td_api::make_object<td::td_api::close>()});
  ASSERT_EQ(td::td_api::ok::ID, receive(client, 3)->get_id());
}
}  // namespace

TEST(NetworkConfiguration, IndependentConfigurationsAndDatacenterPins) {
  auto a = td::blah::parse_dc_config(config(KEY_0)).move_as_ok();
  auto b = td::blah::parse_dc_config(config(KEY_1, 10002)).move_as_ok();
  auto first = td::mtproto::RSA::from_pem_public_key(KEY_0).move_as_ok().get_fingerprint();
  auto second = td::mtproto::RSA::from_pem_public_key(KEY_1).move_as_ok().get_fingerprint();
  ASSERT_TRUE(a->public_keys.at(1)->get_rsa_key({first}).is_ok());
  ASSERT_TRUE(a->public_keys.at(1)->get_rsa_key({second}).is_error());
  ASSERT_TRUE(b->public_keys.at(1)->get_rsa_key({second}).is_ok());
  ASSERT_TRUE(b->public_keys.at(1)->get_rsa_key({first}).is_error());
  ASSERT_EQ(10001, a->dc_options.dc_options[0].get_ip_address().get_port());
  ASSERT_EQ(10002, b->dc_options.dc_options[0].get_ip_address().get_port());
  ASSERT_TRUE(!a->contains(td::DcId::internal(2)));
  ASSERT_TRUE(!a->contains(td::DcId::external(1)));
  auto combined = config(KEY_0);
  auto other = config(KEY_1, 10002, 2);
  combined = combined.substr(0, combined.size() - 2) + "," + other.substr(8);
  auto c = td::blah::parse_dc_config(combined).move_as_ok();
  ASSERT_TRUE(c->public_keys.at(1)->get_rsa_key({second}).is_error());
  ASSERT_TRUE(c->public_keys.at(2)->get_rsa_key({first}).is_error());
}

TEST(NetworkConfiguration, InvalidDocumentsFailClosed) {
  for (auto json : {td::string("{"), td::string("[]"), td::string("{}"), td::string("{\"dcs\":[]}"),
                    config("invalid key"), config(KEY_0, 0), config(KEY_0, 65536), config(KEY_0, 1, 0),
                    td::string(65537, ' ')}) {
    ASSERT_TRUE(td::blah::parse_dc_config(json).is_error());
  }
  auto json = config(KEY_0);
  json.insert(1, "\"defaultDcId\":2,");
  ASSERT_TRUE(td::blah::parse_dc_config(json).is_error());
  json = config(KEY_0);
  json = json.substr(0, json.size() - 2) + "," + json.substr(8);
  ASSERT_TRUE(td::blah::parse_dc_config(json).is_error());
  ASSERT_TRUE(td::blah::parse_dc_config("").move_as_ok() == nullptr);
}

TEST(NetworkConfiguration, NamespaceGuardPrecedesDestructiveOpen) {
  auto directory = td::mkdtemp(td::CSlice(), "td_namespace_").move_as_ok();
  auto binlog_path = directory + "/td.binlog";
  auto sqlite_path = directory + "/db.sqlite";
  {
    td::BinlogKeyValue<td::Binlog> pmc;
    pmc.init(binlog_path, td::DbKey::raw_key("cucumber"), -1, td::LogEvent::HandlerType::BinlogPmcMagic).ensure();
    pmc.set("database_namespace", "original");
    pmc.force_sync(td::Auto(), "namespace test");
  }
  // No auth record: without the guard TdDb destroys this file before opening it.
  td::write_file(sqlite_path, "preserve unauthenticated SQLite cache").ensure();
  auto sqlite_before = td::read_file_str(sqlite_path).move_as_ok();
  auto binlog_before = td::read_file_str(binlog_path).move_as_ok();
  expect_database_error(directory, "different", "DATABASE_NAMESPACE_MISMATCH");
  ASSERT_EQ(sqlite_before, td::read_file_str(sqlite_path).move_as_ok());
  ASSERT_EQ(binlog_before, td::read_file_str(binlog_path).move_as_ok());
  // A second attempt also proves the refusal released the binlog lock.
  expect_database_error(directory, "", "DATABASE_NAMESPACE_MISMATCH", "");
  ASSERT_EQ(sqlite_before, td::read_file_str(sqlite_path).move_as_ok());
  ASSERT_EQ(binlog_before, td::read_file_str(binlog_path).move_as_ok());
  td::rmrf(directory).ensure();
}

TEST(NetworkConfiguration, PopulatedUnboundCacheCannotBeClaimed) {
  auto directory = td::mkdtemp(td::CSlice(), "td_namespace_").move_as_ok();
  auto path = directory + "/db.sqlite";
  td::write_file(path, "unbound cache").ensure();
  expect_database_error(directory, "new", "NETWORK_CONFIGURATION_INVALID", "{");
  ASSERT_TRUE(td::stat(directory + "/td.binlog").is_error());
  ASSERT_EQ("unbound cache", td::read_file_str(path).move_as_ok());
  expect_database_error(directory, "new", "DATABASE_NAMESPACE_REQUIRED");
  ASSERT_EQ("unbound cache", td::read_file_str(path).move_as_ok());
  td::rmrf(directory).ensure();
  ASSERT_TRUE(td::check_database_namespace("same", "same", true).is_ok());
  ASSERT_TRUE(td::check_database_namespace("", "", true).is_ok());
  ASSERT_TRUE(td::check_database_namespace("new", "", false).is_ok());
}

TEST(NetworkConfiguration, PopulatedUnboundBinlogCannotBeClaimed) {
  auto directory = td::mkdtemp(td::CSlice(), "td_namespace_").move_as_ok();
  auto path = directory + "/td.binlog";
  {
    td::BinlogKeyValue<td::Binlog> pmc;
    pmc.init(path, td::DbKey::raw_key("cucumber"), -1, td::LogEvent::HandlerType::BinlogPmcMagic).ensure();
    pmc.set("legacy_value", "existing account state");
    pmc.force_sync(td::Auto(), "namespace test");
  }
  auto before = td::read_file_str(path).move_as_ok();
  expect_database_error(directory, "new", "DATABASE_NAMESPACE_REQUIRED");
  ASSERT_EQ(before, td::read_file_str(path).move_as_ok());
  ASSERT_TRUE(td::stat(directory + "/db.sqlite").is_error());
  td::rmrf(directory).ensure();
}

TEST(NetworkConfiguration, ProxyRejectsUnconfiguredDcBeforeResolution) {
  auto directory = td::mkdtemp(td::CSlice(), "td_namespace_").move_as_ok();
  {
    td::Client client;
    client.send({1, td::td_api::make_object<td::td_api::setNetworkType>(
                        td::td_api::make_object<td::td_api::networkTypeNone>())});
    auto parameters = td::td_api::make_object<td::td_api::setTdlibParameters>();
    parameters->database_directory_ = directory;
    parameters->database_namespace_ = "proxy-check";
    parameters->network_configuration_ = config(KEY_0);
    parameters->api_id_ = 1;
    parameters->api_hash_ = "test";
    parameters->system_language_code_ = "en";
    parameters->device_model_ = "test";
    parameters->application_version_ = "test";
    client.send({2, std::move(parameters)});
    ASSERT_EQ(td::td_api::ok::ID, receive(client, 2)->get_id());
    // Resolving this proxy would fail for a different reason. The configured
    // DC membership check must win, without DNS resolution or opening a socket.
    auto proxy = td::td_api::make_object<td::td_api::proxy>(
        "not a host", 12345, td::td_api::make_object<td::td_api::proxyTypeSocks5>("", ""));
    client.send({3, td::td_api::make_object<td::td_api::testProxy>(std::move(proxy), 2, 1.0)});
    auto result = receive(client, 3);
    ASSERT_EQ(td::td_api::error::ID, result->get_id());
    auto &error = static_cast<const td::td_api::error &>(*result);
    ASSERT_EQ(400, error.code_);
    ASSERT_EQ("NETWORK_DC_UNCONFIGURED", error.message_);
    client.send({4, td::td_api::make_object<td::td_api::close>()});
    ASSERT_EQ(td::td_api::ok::ID, receive(client, 4)->get_id());
  }
  td::rmrf(directory).ensure();
}

TEST(NetworkConfiguration, OfflineStateIsFirstObservableState) {
  class Callback final : public td::StateManager::Callback {
   public:
    explicit Callback(td::NetType expected) : expected_(expected) {}
    bool on_network(td::NetType type, td::uint32) final {
      ASSERT_EQ(static_cast<int>(expected_), static_cast<int>(type));
      return true;
    }
    bool on_state(td::ConnectionState state) final {
      ASSERT_EQ(static_cast<int>(expected_ == td::NetType::None ? td::ConnectionState::WaitingForNetwork
                                                               : td::ConnectionState::Connecting),
                static_cast<int>(state));
      return true;
    }
   private:
    td::NetType expected_;
  };
  td::StateManager offline({}, td::NetType::None);
  offline.add_callback(td::make_unique<Callback>(td::NetType::None));
  td::StateManager ordinary({});
  ordinary.add_callback(td::make_unique<Callback>(td::NetType::Unknown));
}
