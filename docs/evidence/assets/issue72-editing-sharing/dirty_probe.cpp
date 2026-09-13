// Throwaway probe for the dirty-query and save-preparation costs exposed by the
// structurally shared document versions (issue #72). Not part of any build.
#include "nemo/core/document/Document.hpp"
#include "nemo/core/session/ProjectSession.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace nemo;
using Clock = std::chrono::steady_clock;

namespace {

double micros(Clock::duration duration) {
    return std::chrono::duration<double, std::micro>(duration).count();
}

double p50(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

void submit(ProjectSession& session, Command command) {
    const EditResult result = session.submit(std::move(command), EditOptions{session.revision(), {}});
    if (!result.committed)
        throw std::runtime_error(result.error ? result.error->message : "rejected");
}

struct Built {
    Document document;
    NodeId target{kInvalidNode};
};

Built buildChain(std::size_t totalNodes, std::size_t parametersPerNode) {
    ProjectSession scratch;
    const NetworkId network = scratch.document().rootNetworkId();
    NodeId previous{kInvalidNode};
    NodeId first{kInvalidNode};
    for (std::size_t i = 0; i < totalNodes; ++i) {
        auto created = std::make_shared<NodeId>(kInvalidNode);
        submit(scratch, addNodeCommand(network, "grade", "n" + std::to_string(i), created));
        if (first == kInvalidNode)
            first = *created;
        if (previous != kInvalidNode)
            submit(scratch, connectCommand(network, PortRef{previous, 0}, PortRef{*created, 0}));
        previous = *created;
    }
    std::vector<ParameterEdit> edits;
    for (std::size_t i = 0; i < parametersPerNode; ++i)
        edits.push_back(
            ParameterEdit{ParameterAddress{network, first, "mix"}, ParameterValue{static_cast<double>(i + 1) * 0.01}});
    submit(scratch, setParametersCommand(std::move(edits)));
    return Built{scratch.snapshot(), first};
}

void probe(const char* label, std::size_t totalNodes, std::size_t parametersPerNode) {
    const Built built = buildChain(totalNodes, parametersPerNode);
    const NetworkId network = built.document.rootNetworkId();
    const std::size_t composition = built.document.network(network).graph().nodes().size();

    ProjectSession session(built.document);
    const auto time = [&](auto&& op, int samples) {
        std::vector<double> measured;
        measured.reserve(static_cast<std::size_t>(samples));
        for (int i = 0; i < samples; ++i) {
            const auto start = Clock::now();
            op();
            measured.push_back(micros(Clock::now() - start));
        }
        return p50(std::move(measured));
    };

    const bool baselineEqual = documentContentEquals(session.document(), built.document);
    const bool dirtyBefore = session.isDirty();
    const double cleanQuery = time([&] { static_cast<void>(session.isDirty()); }, 200);

    // A committed edit makes the session dirty; undoing it returns the exact
    // saved content, so the query must report clean again.
    submit(session, setParamCommand(network, built.target, "mix", ParameterValue{0.375}));
    const bool dirtyAfterEdit = session.isDirty();
    const double dirtyQuery = time([&] { static_cast<void>(session.isDirty()); }, 200);
    static_cast<void>(session.undo(EditOptions{session.revision(), {}}));
    const bool dirtyAfterUndo = session.isDirty();
    const double cleanAfterUndoQuery = time([&] { static_cast<void>(session.isDirty()); }, 200);

    // One edit, undone outside the timer so the session stays at its baseline.
    const double singleEdit = time(
        [&] {
            submit(session, setParamCommand(network, built.target, "mix", ParameterValue{0.625}));
            static_cast<void>(session.undo(EditOptions{session.revision(), {}}));
        },
        200);
    const double prepareSave =
        time([&] { static_cast<void>(session.prepareSave("/tmp/issue72-evidence/out.nemo")); }, 60);

    std::printf("{\"probe\":\"dirty\",\"variant\":\"%s\",\"nodes\":%zu,\"clean_query_us\":%.3f,"
                "\"dirty_query_us\":%.3f,\"clean_after_undo_query_us\":%.3f,\"single_edit_us\":%.3f,"
                "\"prepare_save_us\":%.3f,\"is_dirty_before\":%s,\"is_dirty_after_edit\":%s,"
                "\"is_dirty_after_undo\":%s,\"is_dirty_at_end\":%s,\"baseline_equal\":%s}\n",
                label, composition, cleanQuery, dirtyQuery, cleanAfterUndoQuery, singleEdit, prepareSave,
                dirtyBefore ? "true" : "false", dirtyAfterEdit ? "true" : "false", dirtyAfterUndo ? "true" : "false",
                session.isDirty() ? "true" : "false", baselineEqual ? "true" : "false");
}

}  // namespace

int main() {
    probe("chain.82", 64, 4);
    probe("params.1026", 1024, 8);
    return 0;
}
