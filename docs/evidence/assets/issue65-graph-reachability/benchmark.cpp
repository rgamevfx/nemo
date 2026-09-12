// issue #65 - retained synthetic timing evidence for graph reachability.
// Not a test, not a CTest entry, no timing assertions: numbers are context only.
//
// Workload: a fully reconverging merge DAG, two nodes per layer, plus one
// isolated node. Layer 0 is two "source" nodes; each later layer is two "merge"
// nodes whose A=0/B=1 inputs both take the previous layer's two nodes (4 edges
// per transition). nodes = 2L+1, edges = 4(L-1): 21/37/45 nodes -> 36/68/84
// edges. Graph construction is outside every timed region.
//
// Queries (both unreachable, both must answer false):
//   isolated_to_last  origin=isolated, target=last-layer node: the reverse
//                     traversal walks the graph-owned incoming adjacency back
//                     through every layer; forward from an isolated origin
//                     sees no outgoing edges.
//   root_to_isolated  origin=root, target=isolated: old forward path-enumerates
//                     the reconvergent DAG exponentially; reverse answers at once.
//
// Method: steady_clock; 5 timed batches of `reps` queries per query, reporting
// min/median/max ns per query and a checksum (count of true answers). The
// untimed warm-up and atomic_signal_fence keep calls live; a measured true is a
// failure. Usage: benchmark [reps]  (baseline 3, current 10000; default 3).
//
// Compile from the repo root. Graph.cpp is listed before libnemo_core.a so the
// explicit translation unit replaces the archive's Graph.cpp.o; the archive
// supplies NodeCatalog/ParameterValue/Request. The baseline Graph.cpp needs the
// baseline Graph.hpp, because the changed header declares new non-const private
// lookup helpers:
//
//   mkdir -p /tmp/issue65-baseline/nemo/core/document
//   git show fee41ba:src/nemo/core/document/Graph.cpp > /tmp/issue65-baseline/Graph.cpp
//   git show fee41ba:src/nemo/core/document/Graph.hpp > /tmp/issue65-baseline/nemo/core/document/Graph.hpp
//   CXX="g++ -std=c++20 -O2 -DNDEBUG"
//   B=docs/evidence/assets/issue65-graph-reachability/benchmark.cpp
//   LIB=build/release/src/nemo/libnemo_core.a
//   INC="-I src -I build/release/vcpkg_installed/x64-linux/include"
//   $CXX -I /tmp/issue65-baseline $INC $B /tmp/issue65-baseline/Graph.cpp $LIB -o /tmp/issue65-bench-baseline
//   $CXX $INC $B src/nemo/core/document/Graph.cpp $LIB -o /tmp/issue65-bench-current
//   /tmp/issue65-bench-baseline 3 && /tmp/issue65-bench-current 10000

#include "nemo/core/document/Graph.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr int kBatchCount = 5;
constexpr std::array<int, 3> kSizes{21, 37, 45};

inline double ns(Clock::duration duration) {
    return std::chrono::duration<double, std::nano>(duration).count();
}

struct Workload {
    nemo::Graph graph;
    nemo::NodeId isolated{};
    nemo::NodeId root{};
    nemo::NodeId last{};
    int layers{0};
    std::size_t nodes{0};
    std::size_t edges{0};
};

Workload buildWorkload(int totalNodes) {
    Workload workload;
    workload.layers = (totalNodes - 1) / 2;
    std::vector<std::array<nemo::NodeId, 2>> layers;
    layers.reserve(static_cast<std::size_t>(workload.layers));
    layers.push_back({workload.graph.addNode("source", "source_a"), workload.graph.addNode("source", "source_b")});
    for (int layer = 1; layer < workload.layers; ++layer) {
        const std::string suffix = std::to_string(layer);
        layers.push_back({workload.graph.addNode("merge", "merge_" + suffix + "_a"),
                          workload.graph.addNode("merge", "merge_" + suffix + "_b")});
    }
    for (int layer = 1; layer < workload.layers; ++layer) {
        const auto& previous = layers[static_cast<std::size_t>(layer - 1)];
        const auto& current = layers[static_cast<std::size_t>(layer)];
        (void)workload.graph.connect({previous[0], 0}, {current[0], 0});
        (void)workload.graph.connect({previous[1], 0}, {current[0], 1});
        (void)workload.graph.connect({previous[0], 0}, {current[1], 0});
        (void)workload.graph.connect({previous[1], 0}, {current[1], 1});
    }
    workload.isolated = workload.graph.addNode("source", "isolated_origin");
    workload.root = layers.front()[0];
    workload.last = layers.back()[0];
    workload.nodes = workload.graph.nodes().size();
    workload.edges = workload.graph.edges().size();
    return workload;
}

struct Sample {
    double minNs{0.0};
    double medianNs{0.0};
    double maxNs{0.0};
    long long trueAnswers{0};
};

Sample measure(const nemo::Graph& graph, nemo::NodeId origin, nemo::NodeId target, long long reps) {
    (void)graph.reachable(origin, target);  // warm-up, untimed
    std::array<double, kBatchCount> perQueryNs{};
    long long trueAnswers = 0;
    for (int batch = 0; batch < kBatchCount; ++batch) {
        long long batchTrue = 0;
        const auto start = Clock::now();
        for (long long i = 0; i < reps; ++i) {
            batchTrue += graph.reachable(origin, target) ? 1 : 0;
            std::atomic_signal_fence(std::memory_order_acq_rel);
        }
        const auto finish = Clock::now();
        perQueryNs[static_cast<std::size_t>(batch)] = ns(finish - start) / static_cast<double>(reps);
        trueAnswers += batchTrue;
    }
    std::sort(perQueryNs.begin(), perQueryNs.end());
    return Sample{perQueryNs.front(), perQueryNs[kBatchCount / 2], perQueryNs.back(), trueAnswers};
}

}  // namespace

int main(int argc, char** argv) {
    const long long reps = argc > 1 ? std::atoll(argv[1]) : 3;
    if (reps <= 0) {
        std::fprintf(stderr, "reps must be positive\n");
        return 2;
    }

    std::printf("# issue65 graph reachability synthetic timing (context only, no CI threshold)\n");
    std::printf("# compiler=%s reps=%lld batches=%d\n", __VERSION__, reps, kBatchCount);
    int failures = 0;

    for (const int totalNodes : kSizes) {
        const Workload workload = buildWorkload(totalNodes);
        const std::size_t expectedNodes = static_cast<std::size_t>(2 * workload.layers + 1);
        const std::size_t expectedEdges = static_cast<std::size_t>(4 * (workload.layers - 1));
        const bool shapeOk = workload.nodes == expectedNodes && workload.edges == expectedEdges;
        if (!shapeOk)
            ++failures;
        std::printf("== size=%d nodes=%zu edges=%zu layers=%d shape_ok=%s ==\n", totalNodes, workload.nodes,
                    workload.edges, workload.layers, shapeOk ? "true" : "false");

        const std::array<std::pair<const char*, std::pair<nemo::NodeId, nemo::NodeId>>, 2> queries{{
            {"isolated_to_last", {workload.isolated, workload.last}},
            {"root_to_isolated", {workload.root, workload.isolated}},
        }};
        for (const auto& query : queries) {
            const Sample sample = measure(workload.graph, query.second.first, query.second.second, reps);
            if (sample.trueAnswers != 0)
                ++failures;
            std::printf("timing size=%-3d scenario=%-17s reps=%-6lld batches=%d queries=%-8lld min_ns=%-12.1f "
                        "median_ns=%-12.1f max_ns=%-12.1f checksum=%lld\n",
                        totalNodes, query.first, reps, kBatchCount, reps * kBatchCount, sample.minNs, sample.medianNs,
                        sample.maxNs, sample.trueAnswers);
        }
    }

    std::printf("summary failures=%d (checksum counts unexpected true answers)\n", failures);
    return failures == 0 ? 0 : 1;
}
