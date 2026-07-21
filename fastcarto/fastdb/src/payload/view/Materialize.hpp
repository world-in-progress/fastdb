#pragma once

#include "payload/error/Result.hpp"
#include "payload/view/View.hpp"

#include <cstdint>

namespace fastdb::payload::view {

struct MaterializeMetrics final {
    std::uint64_t peak_retained_diagnostic_path_bytes{UINT64_C(0)};
};

error::Result<View> materialize(const View& view);
error::Result<View> materialize_with_metrics(
    const View& view,
    MaterializeMetrics* metrics);
error::Result<View> materialize_graph_with_metrics(
    const View& view,
    MaterializeMetrics* metrics);

}  // namespace fastdb::payload::view
