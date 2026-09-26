#pragma once

#include "td/telegram/telegram_api.h"
#include "td/utils/Slice.h"
#include "td/utils/tl_storers.h"

namespace td {

// The application signs the exact inner boxed TL bytes. Generic !X wrappers
// are not emitted as C++ functions by the TDLib schema generator, so this
// Function stores their two fixed prefixes and appends the supplied query.
class DiemRawQuery final : public telegram_api::Function {
 public:
  static constexpr int32 INVOKE_WITH_LAYER = static_cast<int32>(0xfb8f651a);
  static constexpr int32 INVOKE_WITH_IDENTITY_PROOF = 0x525b7c28;

  DiemRawQuery(int32 constructor, string query, string proof = {}, int32 layer = 1)
      : constructor_(constructor), query_(std::move(query)), proof_(std::move(proof)), layer_(layer) {
  }

  int32 get_id() const final { return constructor_; }

  void store(TlStorerCalcLength &storer) const final { store_fields(storer); }
  void store(TlStorerUnsafe &storer) const final { store_fields(storer); }
  void store(TlStorerToString &, const char *) const final {}

 private:
  template <class Storer>
  void store_fields(Storer &storer) const {
    storer.store_binary(constructor_);
    if (constructor_ == INVOKE_WITH_LAYER) {
      storer.store_int(layer_);
    } else {
      storer.store_string(proof_);
    }
    storer.store_slice(Slice(query_));
  }

  int32 constructor_;
  string query_;
  string proof_;
  int32 layer_;
};

}  // namespace td
