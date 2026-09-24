#include "@HEADER@"

#include <cstdint>
#include <string_view>
#include <utility>

namespace generated = fastdb_payload_@DIGEST@;
using fastdb::payload::v1::Builder;
using fastdb::payload::v1::BuildPolicy;
using fastdb::payload::v1::CompiledSpec;
using fastdb::payload::v1::PayloadError;

template <typename Callback>
bool digest_mismatch(Callback &&callback, std::string_view path) {
  try {
    std::forward<Callback>(callback)();
  } catch (const PayloadError &error) {
    return error.code() == FDB_PAYLOAD_E_DIGEST_MISMATCH &&
           error.symbol() == "DIGEST_MISMATCH" && error.path() == path &&
           error.details_json().find("\"reason\":\"spec_digest_mismatch\"") !=
               std::string_view::npos;
  }
  return false;
}

int main() {
  const CompiledSpec wrong_spec = CompiledSpec::compile(
      R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"other","cardinality":"one","type":{"kind":"component","id":"Other"}}],"components":[{"id":"Other","kind":"record","fields":[{"id":"different","type":{"kind":"u8"}}]}]})");
  Builder wrong_builder = Builder::create(wrong_spec);
  if (!digest_mismatch(
          [&] {
            static_cast<void>(
                generated::fdb_cpp_id_726f6f74_builder_entry_begin(
                    wrong_builder, UINT64_C(1)));
          },
          "/builder/spec_sha256")) {
    return 1;
  }
  wrong_builder.entry_begin(UINT32_C(0), UINT64_C(1))
      .value_component_begin()
      .value_u8(UINT8_C(9));
  auto wrong_built = wrong_builder.freeze().execute(BuildPolicy::allow_staging);
  if (!digest_mismatch(
          [&] {
            static_cast<void>(generated::fdb_cpp_id_726f6f74_from_payload(
                wrong_built.payload));
          },
          "/payload/spec_sha256")) {
    return 2;
  }
  auto wrong_view = wrong_built.payload.entry_view(UINT32_C(0)).at(UINT64_C(0));
  if (!digest_mismatch(
          [&] {
            static_cast<void>(
                generated::FdbCppType_fdb_cpp_id_4974656d_View::try_from_view(
                    wrong_view));
          },
          "/view/spec_sha256")) {
    return 3;
  }

  const CompiledSpec spec = generated::compile_spec();
  Builder builder = Builder::create(spec);
  generated::fdb_cpp_id_726f6f74_builder_entry_begin(builder, UINT64_C(1))
      .value_component_begin()
      .value_u8(UINT8_C(7));
  auto built = builder.freeze().execute(BuildPolicy::allow_staging);
  auto entry = generated::fdb_cpp_id_726f6f74_from_payload(built.payload);
  auto component =
      generated::FdbCppType_fdb_cpp_id_4974656d_View::try_from_view(
          entry.at(UINT64_C(0)));
  return component.has_value() &&
                 component->fdb_cpp_id_76616c7565_value() == UINT8_C(7)
             ? 0
             : 4;
}
