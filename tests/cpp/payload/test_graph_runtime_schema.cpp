#include "TestSupport.hpp"

#include "payload/layout/RuntimeSchema.hpp"
#include "payload/spec/CompiledSpec.hpp"
#include "payload/spec/RuntimeTopology.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

using fastdb::payload::layout::RuntimeSchema;
using fastdb::payload::spec::CompiledSpec;
using fastdb::payload::spec::RuntimeTopology;
using fastdb::payload::spec::StorageRole;
using fastdb::payload::spec::TypeNode;
using fastdb::payload::spec::ValueContext;
using fastdb::payload::spec::derive_runtime_topology;

constexpr std::string_view graph_topology_spec = R"({
  "schema":"fastdb.payload.v1",
  "profile":"object_graph.v1",
  "entries":[
    {"id":"a_root","cardinality":"one","type":{"kind":"component","id":"IdentityInline"}},
    {"id":"b_root_list","cardinality":"one","type":{"kind":"list","items":{"kind":"component","id":"ListedRoot"}}},
    {"id":"c_ref","cardinality":"one","type":{"kind":"ref","target":"RefOnly"}},
    {"id":"d_ref_list","cardinality":"many","type":{"kind":"list","items":{"kind":"ref","target":"MutualA"}}},
    {"id":"e_scalar","cardinality":"one","type":{"kind":"u8"}}
  ],
  "components":[
    {"id":"ByValueOnly","kind":"record","fields":[
      {"id":"value","type":{"kind":"u32"}}
    ]},
    {"id":"IdentityInline","kind":"record","fields":[
      {"id":"inner","type":{"kind":"component","id":"ByValueOnly"}},
      {"id":"inner_list","type":{"kind":"list","items":{"kind":"component","id":"ByValueOnly"}}},
      {"id":"shared_a","type":{"kind":"ref","target":"RefOnly","nullable":true}},
      {"id":"shared_b","type":{"kind":"ref","target":"RefOnly"}}
    ]},
    {"id":"ListedRoot","kind":"record","fields":[
      {"id":"embedded","type":{"kind":"component","id":"IdentityInline"}}
    ]},
    {"id":"MutualA","kind":"record","fields":[
      {"id":"self","type":{"kind":"ref","target":"MutualA"}},
      {"id":"peer","type":{"kind":"ref","target":"MutualB"}}
    ]},
    {"id":"MutualB","kind":"record","fields":[
      {"id":"back","type":{"kind":"ref","target":"MutualA"}}
    ]},
    {"id":"RefOnly","kind":"record","fields":[
      {"id":"payload","type":{"kind":"str"}}
    ]},
    {"id":"Unused","kind":"record","fields":[
      {"id":"ignored","type":{"kind":"list","items":{"kind":"bytes"}}}
    ]}
  ]
})";

std::uint32_t topology_id(const RuntimeTopology& topology,
                          const TypeNode& source) {
    for (std::size_t index = 0U; index < topology.types.size(); ++index) {
        if (topology.types[index].source == &source) {
            return static_cast<std::uint32_t>(index);
        }
    }
    return UINT32_MAX;
}

void append_chain(std::vector<const TypeNode*>& expected,
                  const TypeNode& root) {
    const TypeNode* current = &root;
    while (current != nullptr) {
        expected.push_back(current);
        current = current->items.get();
    }
}

int test_roles_identity_reachability_and_layout() {
    auto compiled = CompiledSpec::compile(graph_topology_spec);
    require(compiled.has_value());
    auto derived = derive_runtime_topology(compiled.value().resolved());
    require(derived.has_value());
    const RuntimeTopology& topology = derived.value();

    std::vector<const TypeNode*> expected_types;
    for (const auto& entry : compiled.value().resolved().entries()) {
        append_chain(expected_types, entry.type);
    }
    for (const auto& component : compiled.value().resolved().components()) {
        for (const auto& field : component.fields) {
            append_chain(expected_types, field.type);
        }
    }
    require(topology.types.size() == expected_types.size());
    require(topology.types.size() == 20U);
    for (std::size_t index = 0U; index < expected_types.size(); ++index) {
        require(topology.types[index].source == expected_types[index]);
    }
    require(topology.reachable_type_count == UINT32_C(18));
    require(topology.reachable_component_count == UINT32_C(6));
    require(topology.reachable_list_type_count == UINT32_C(3));
    require(topology.has_utf8);
    require(!topology.has_utf16le);
    require(!topology.has_bytes);
    require(topology.has_list_items);
    require(topology.has_objects);
    require(topology.has_references);
    require(topology.has_roots);

    const auto& entries = compiled.value().resolved().entries();
    const std::uint32_t root_id = topology_id(topology, entries[0].type);
    const std::uint32_t root_list_id = topology_id(topology, entries[1].type);
    const std::uint32_t listed_root_id =
        topology_id(topology, *entries[1].type.items);
    const std::uint32_t entry_ref_id = topology_id(topology, entries[2].type);
    const std::uint32_t ref_list_id = topology_id(topology, entries[3].type);
    const std::uint32_t listed_ref_id =
        topology_id(topology, *entries[3].type.items);
    require(root_id != UINT32_MAX);
    require(root_list_id != UINT32_MAX);
    require(listed_root_id != UINT32_MAX);
    require(entry_ref_id != UINT32_MAX);
    require(ref_list_id != UINT32_MAX);
    require(listed_ref_id != UINT32_MAX);
    require(topology.types[root_id].context == ValueContext::entry_value);
    require(topology.types[root_id].storage_role ==
            StorageRole::object_root_id);
    require(topology.types[root_list_id].storage_role ==
            StorageRole::list_descriptor);
    require(topology.types[listed_root_id].context ==
            ValueContext::entry_value);
    require(topology.types[listed_root_id].storage_role ==
            StorageRole::object_root_id);
    require(topology.types[entry_ref_id].storage_role ==
            StorageRole::reference_id);
    require(topology.types[ref_list_id].storage_role ==
            StorageRole::list_descriptor);
    require(topology.types[listed_ref_id].context ==
            ValueContext::entry_value);
    require(topology.types[listed_ref_id].storage_role ==
            StorageRole::reference_id);

    const auto by_value = compiled.value().component_index("ByValueOnly");
    const auto identity_inline =
        compiled.value().component_index("IdentityInline");
    const auto listed_root = compiled.value().component_index("ListedRoot");
    const auto mutual_a = compiled.value().component_index("MutualA");
    const auto mutual_b = compiled.value().component_index("MutualB");
    const auto ref_only = compiled.value().component_index("RefOnly");
    const auto unused = compiled.value().component_index("Unused");
    require(by_value.has_value());
    require(identity_inline.has_value());
    require(listed_root.has_value());
    require(mutual_a.has_value());
    require(mutual_b.has_value());
    require(ref_only.has_value());
    require(unused.has_value());

    require(topology.reachable_components[*by_value] != UINT8_C(0));
    require(topology.identity_components[*by_value] == UINT8_C(0));
    require(topology.identity_components[*identity_inline] != UINT8_C(0));
    require(topology.identity_components[*listed_root] != UINT8_C(0));
    require(topology.identity_components[*mutual_a] != UINT8_C(0));
    require(topology.identity_components[*mutual_b] != UINT8_C(0));
    require(topology.identity_components[*ref_only] != UINT8_C(0));
    require(topology.reachable_components[*unused] == UINT8_C(0));
    require(topology.identity_components[*unused] == UINT8_C(0));

    const auto& components = compiled.value().resolved().components();
    const auto& identity_fields = components[*identity_inline].fields;
    const std::uint32_t inline_id =
        topology_id(topology, identity_fields[0].type);
    const std::uint32_t inline_list_id =
        topology_id(topology, identity_fields[1].type);
    const std::uint32_t inline_list_item_id =
        topology_id(topology, *identity_fields[1].type.items);
    const std::uint32_t shared_a_id =
        topology_id(topology, identity_fields[2].type);
    const std::uint32_t shared_b_id =
        topology_id(topology, identity_fields[3].type);
    require(inline_id != UINT32_MAX);
    require(inline_list_id != UINT32_MAX);
    require(inline_list_item_id != UINT32_MAX);
    require(shared_a_id != UINT32_MAX);
    require(shared_b_id != UINT32_MAX);
    require(topology.types[inline_id].context ==
            ValueContext::component_field);
    require(topology.types[inline_id].storage_role ==
            StorageRole::inline_component);
    require(topology.types[inline_list_id].storage_role ==
            StorageRole::list_descriptor);
    require(topology.types[inline_list_item_id].context ==
            ValueContext::component_field);
    require(topology.types[inline_list_item_id].storage_role ==
            StorageRole::inline_component);
    require(topology.types[shared_a_id].storage_role ==
            StorageRole::reference_id);
    require(topology.types[shared_b_id].storage_role ==
            StorageRole::reference_id);
    const auto& listed_fields = components[*listed_root].fields;
    const std::uint32_t identity_used_inline_id =
        topology_id(topology, listed_fields[0].type);
    require(identity_used_inline_id != UINT32_MAX);
    require(topology.types[identity_used_inline_id].storage_role ==
            StorageRole::inline_component);

    const auto& unused_fields = components[*unused].fields;
    const std::uint32_t unused_list_id =
        topology_id(topology, unused_fields[0].type);
    const std::uint32_t unused_bytes_id =
        topology_id(topology, *unused_fields[0].type.items);
    require(unused_list_id != UINT32_MAX);
    require(unused_bytes_id == unused_list_id + UINT32_C(1));
    require(!topology.types[unused_list_id].reachable);
    require(!topology.types[unused_bytes_id].reachable);

    auto runtime = RuntimeSchema::compile(compiled.value());
    require(runtime.has_value());
    const RuntimeSchema& schema = runtime.value();
    require(schema.type_count() == topology.types.size());
    require(schema.runtime_id(entries[0].type) == root_id);
    require(schema.storage_role(root_id).has_value());
    require(*schema.storage_role(root_id) == StorageRole::object_root_id);
    require(schema.find_type(root_id)->slot.stride == UINT32_C(8));
    require(schema.find_type(root_id)->slot.alignment == UINT32_C(8));
    require(schema.find_type(entry_ref_id)->slot.stride == UINT32_C(8));
    require(schema.find_type(entry_ref_id)->slot.alignment == UINT32_C(8));
    require(schema.find_type(shared_a_id)->slot.stride == UINT32_C(8));
    require(schema.find_type(shared_a_id)->slot.alignment == UINT32_C(8));
    require(schema.find_type(root_list_id)->slot.stride == UINT32_C(16));
    require(schema.find_type(root_list_id)->slot.alignment == UINT32_C(8));
    require(schema.find_type(inline_id)->slot.stride == UINT32_C(4));
    require(schema.find_type(inline_id)->slot.alignment == UINT32_C(4));
    require(schema.find_type(inline_list_item_id)->slot.stride ==
            UINT32_C(4));
    const auto* identity_layout = schema.component(*identity_inline);
    require(identity_layout != nullptr);
    require(schema.find_type(identity_used_inline_id)->slot.stride ==
            identity_layout->stride);
    require(identity_layout->validity_bytes == UINT32_C(1));
    require(identity_layout->fields[2].runtime_type_id == shared_a_id);
    require(identity_layout->fields[2].slot_stride == UINT32_C(8));
    require(identity_layout->fields[2].alignment == UINT32_C(8));
    require(identity_layout->fields[2].validity_bit == UINT32_C(0));
    require(identity_layout->fields[3].runtime_type_id == shared_b_id);
    require(identity_layout->fields[3].slot_stride == UINT32_C(8));
    require(identity_layout->fields[3].validity_bit == UINT32_MAX);
    require(schema.component_identity_bearing(*identity_inline));
    require(!schema.component_identity_bearing(*by_value));
    require(!schema.component_identity_bearing(*unused));
    require(schema.component(*unused) == nullptr);
    require(schema.components().size() == 6U);
    const std::vector<std::uint32_t> expected_identity_components{
        *identity_inline, *listed_root, *mutual_a, *mutual_b, *ref_only};
    require(schema.identity_components() == expected_identity_components);
    require(schema.list_nodes().size() == 3U);
    bool found_ref_list = false;
    for (const auto& list : schema.list_nodes()) {
        if (list.owner_runtime_type_id == ref_list_id) {
            found_ref_list = true;
            require(list.item_runtime_type_id == listed_ref_id);
            require(list.item_stride == UINT32_C(8));
            require(list.item_alignment == UINT32_C(8));
        }
    }
    require(found_ref_list);
    return EXIT_SUCCESS;
}

std::string cycle_component_id(std::uint32_t index) {
    std::string digits = std::to_string(index);
    return "c" + std::string(5U - digits.size(), '0') + digits;
}

std::string large_ref_cycle_spec() {
    constexpr std::uint32_t component_count = UINT32_C(20000);
    std::string source;
    source.reserve(3000000U);
    source +=
        "{\"schema\":\"fastdb.payload.v1\","
        "\"profile\":\"object_graph.v1\","
        "\"entries\":[{\"id\":\"root\",\"cardinality\":\"one\","
        "\"type\":{\"kind\":\"component\",\"id\":\"c00000\"}}],"
        "\"components\":[";
    for (std::uint32_t index = UINT32_C(0); index < component_count;
         ++index) {
        if (index != UINT32_C(0)) {
            source += ',';
        }
        const std::uint32_t next =
            (index + UINT32_C(1)) % component_count;
        source += "{\"id\":\"" + cycle_component_id(index) +
                  "\",\"kind\":\"record\",\"fields\":[{\"id\":\"next\","
                  "\"type\":{\"kind\":\"ref\",\"target\":\"" +
                  cycle_component_id(next) + "\"}}]}";
    }
    source += "]}";
    return source;
}

int test_large_reference_cycle_is_iterative() {
    constexpr std::uint32_t component_count = UINT32_C(20000);
    const std::string source = large_ref_cycle_spec();
    auto compiled = CompiledSpec::compile(source);
    require(compiled.has_value());
    auto derived = derive_runtime_topology(compiled.value().resolved());
    require(derived.has_value());
    require(derived.value().types.size() ==
            static_cast<std::size_t>(component_count) + 1U);
    require(derived.value().reachable_type_count ==
            component_count + UINT32_C(1));
    require(derived.value().reachable_component_count == component_count);
    for (const std::uint8_t reachable :
         derived.value().reachable_components) {
        require(reachable != UINT8_C(0));
    }
    for (const std::uint8_t identity :
         derived.value().identity_components) {
        require(identity != UINT8_C(0));
    }
    auto runtime = RuntimeSchema::compile(compiled.value());
    require(runtime.has_value());
    require(runtime.value().components().size() == component_count);
    require(runtime.value().identity_components().size() == component_count);
    require(runtime.value().component(UINT32_C(0))->stride == UINT32_C(8));
    require(runtime.value()
                .component(component_count - UINT32_C(1))
                ->stride == UINT32_C(8));
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    if (test_roles_identity_reachability_and_layout() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return test_large_reference_cycle_is_iterative();
}
