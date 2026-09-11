#include <gtest/gtest.h>

#include <array>
#include <barrier>
#include <thread>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/session/ProjectSession.hpp"

using namespace nemo;

namespace {
Graph& rootGraph(Document& document) {
    return document.network(document.rootNetworkId()).graph();
}

const Graph& rootGraph(const Document& document) {
    return document.network(document.rootNetworkId()).graph();
}
Document emptyDocument() {
    Document document;
    rootGraph(document).removeNode(rootGraph(document).nodeByName("Output")->id);
    return document;
}
struct ObserverCount {
    int calls{};
};

void countObserver(void* context) noexcept {
    ++static_cast<ObserverCount*>(context)->calls;
}

struct ReentrantObserver {
    ProjectSession* session{};
    ProjectSession::Subscription* subscription{};
    int calls{};
    bool mutationRejected{};
};

void reentrantObserver(void* context) noexcept {
    auto& observer = *static_cast<ReentrantObserver*>(context);
    ++observer.calls;
    const auto rejected = observer.session->submit(
        addNodeCommand(observer.session->document().rootNetworkId(), "testpattern", "reentrant"),
        EditOptions{observer.session->revision(), "reentrant"});
    observer.mutationRejected = rejected.error && rejected.error->code == EditErrorCode::ReentrantMutation;
    if (observer.subscription) {
        *observer.subscription = ProjectSession::Subscription{};
        observer.subscription = nullptr;
    }
}

EditOptions current(ProjectSession& session) {
    return EditOptions{session.revision(), {}};
}
}  // namespace

TEST(CommandStackTest, PushAppliesImmediately) {
    Document doc = emptyDocument();
    CommandStack stack(doc);
    const NodeId plate = rootGraph(doc).addNode("testpattern", "plate");
    stack.push(setParamCommand(doc.rootNetworkId(), plate, "gain", "2.0"));
    ASSERT_NE(rootGraph(doc).node(plate), nullptr);
    EXPECT_EQ(rootGraph(doc).node(plate)->params.at("gain"), "2.0");
    EXPECT_EQ(stack.depth(), 1u);
}

TEST(CommandStackTest, UndoRestoresAndRedoReapplies) {
    Document doc = emptyDocument();
    CommandStack stack(doc);
    const NodeId plate = rootGraph(doc).addNode("testpattern", "plate");

    stack.push(setParamCommand(doc.rootNetworkId(), plate, "gain", "2.0"));
    stack.push(setParamCommand(doc.rootNetworkId(), plate, "gain", "4.0"));
    EXPECT_EQ(rootGraph(doc).node(plate)->params.at("gain"), "4.0");

    EXPECT_TRUE(stack.undo());
    EXPECT_EQ(rootGraph(doc).node(plate)->params.at("gain"), "2.0");
    EXPECT_TRUE(stack.undo());
    EXPECT_EQ(rootGraph(doc).node(plate)->params.count("gain"), 0u);
    EXPECT_FALSE(stack.undo());

    EXPECT_TRUE(stack.redo());
    EXPECT_EQ(rootGraph(doc).node(plate)->params.at("gain"), "2.0");
    EXPECT_TRUE(stack.redo());
    EXPECT_EQ(rootGraph(doc).node(plate)->params.at("gain"), "4.0");
    EXPECT_FALSE(stack.redo());
}

TEST(CommandStackTest, NewCommandDropsRedoBranch) {
    Document doc = emptyDocument();
    CommandStack stack(doc);
    const NodeId plate = rootGraph(doc).addNode("testpattern", "plate");
    stack.push(setParamCommand(doc.rootNetworkId(), plate, "gain", "2.0"));
    ASSERT_TRUE(stack.undo());

    stack.push(setParamCommand(doc.rootNetworkId(), plate, "gain", "8.0"));
    EXPECT_FALSE(stack.canRedo());
    EXPECT_EQ(rootGraph(doc).node(plate)->params.at("gain"), "8.0");
}

TEST(CommandStackTest, FailingApplyLeavesDocumentUntouched) {
    Document doc = emptyDocument();
    CommandStack stack(doc);
    const NodeId plate = rootGraph(doc).addNode("testpattern", "plate");
    const std::size_t nodeCountBefore = rootGraph(doc).nodes().size();
    const auto revisionBefore = doc.stateRevision();
    EXPECT_THROW(stack.push(setParamCommand(doc.rootNetworkId(), 999, "gain", "1.0")), std::runtime_error);
    EXPECT_EQ(rootGraph(doc).nodes().size(), nodeCountBefore);
    EXPECT_EQ(doc.stateRevision(), revisionBefore);
    EXPECT_EQ(stack.depth(), 0u);
    EXPECT_EQ(rootGraph(doc).node(plate)->params.count("gain"), 0u);
}

TEST(CommandStackTest, FailedTransactionIsAtomic) {
    Document doc = emptyDocument();
    CommandStack stack(doc);
    const NodeId source = rootGraph(doc).addNode("testpattern", "source");
    const NodeId merge = rootGraph(doc).addNode("merge", "merge");
    const auto revisionBefore = doc.stateRevision();

    auto transaction =
        transactionCommand("invalid batch", {setParamCommand(doc.rootNetworkId(), source, "authored", "value"),
                                             connectCommand(doc.rootNetworkId(), {source, 0}, {merge, 9})});
    EXPECT_THROW(stack.push(std::move(transaction)), GraphException);
    EXPECT_EQ(doc.stateRevision(), revisionBefore);
    EXPECT_EQ(stack.depth(), 0u);
    EXPECT_TRUE(rootGraph(doc).node(source)->params.empty());
    EXPECT_TRUE(rootGraph(doc).edges().empty());
}

TEST(CommandStackTest, TransactionUsesOneUndoStepAndStableCreatedIdentity) {
    Document doc = emptyDocument();
    CommandStack stack(doc);
    auto created = std::make_shared<NodeId>();
    auto transaction =
        transactionCommand("create", {addNodeCommand(doc.rootNetworkId(), "testpattern", "created", created)});
    stack.push(std::move(transaction));
    const NodeId id = *created;
    ASSERT_NE(rootGraph(doc).node(id), nullptr);
    ASSERT_EQ(stack.depth(), 1u);
    ASSERT_TRUE(stack.undo());
    EXPECT_EQ(rootGraph(doc).node(id), nullptr);
    ASSERT_TRUE(stack.redo());
    EXPECT_NE(rootGraph(doc).node(id), nullptr);
    EXPECT_EQ(rootGraph(doc).node(id)->name, "created");
}

TEST(CommandStackTest, HistoryBoundedByCapacity) {
    Document doc = emptyDocument();
    CommandStack stack(doc, /*capacity=*/2);
    const NodeId plate = rootGraph(doc).addNode("testpattern", "plate");
    stack.push(setParamCommand(doc.rootNetworkId(), plate, "gain", "1.0"));
    stack.push(setParamCommand(doc.rootNetworkId(), plate, "gain", "2.0"));
    stack.push(setParamCommand(doc.rootNetworkId(), plate, "gain", "3.0"));
    EXPECT_EQ(stack.depth(), 2u);
    EXPECT_TRUE(stack.undo());
    EXPECT_TRUE(stack.undo());
    EXPECT_FALSE(stack.undo());
    EXPECT_EQ(rootGraph(doc).node(plate)->params.at("gain"), "1.0");
}

TEST(ProjectSessionTest, SharesDocumentHistoryAndPreservesSnapshots) {
    ProjectSession session(emptyDocument());
    const auto created =
        session.submit(addNodeCommand(session.document().rootNetworkId(), "testpattern", "plate"), current(session));
    ASSERT_TRUE(created.committed);
    const NodeId plate = created.createdNodeIds.front().id;
    const Document beforeEdit = session.snapshot();
    const auto edit =
        session.submit(setParamCommand(session.document().rootNetworkId(), plate, "gain", "2.0"), current(session));
    ASSERT_TRUE(edit.committed);
    EXPECT_EQ(rootGraph(session.document()).node(plate)->params.at("gain"), "2.0");
    EXPECT_EQ(rootGraph(beforeEdit).node(plate)->params.count("gain"), 0u);
    EXPECT_TRUE(session.undo(current(session)).committed);
    EXPECT_EQ(rootGraph(session.document()).node(plate)->params.count("gain"), 0u);
    EXPECT_TRUE(session.redo(current(session)).committed);
    EXPECT_EQ(rootGraph(session.snapshot()).node(plate)->params.at("gain"), "2.0");
}

TEST(ProjectSessionTest, RejectsStaleRevisionWithoutChangingDocument) {
    ProjectSession session(emptyDocument());
    const auto first =
        session.submit(addNodeCommand(session.document().rootNetworkId(), "testpattern", "plate"), current(session));
    ASSERT_TRUE(first.committed);
    const auto stale = session.submit(addNodeCommand(session.document().rootNetworkId(), "testpattern", "second"),
                                      EditOptions{1, "stale"});
    EXPECT_FALSE(stale.committed);
    ASSERT_TRUE(stale.error.has_value());
    EXPECT_EQ(stale.error->code, EditErrorCode::RevisionConflict);
    EXPECT_EQ(stale.revision, session.revision());
    EXPECT_EQ(rootGraph(session.document()).nodes().size(), 1u);
}

TEST(ProjectSessionTest, NotifiesObserversOnlyForSuccessfulMutations) {
    ProjectSession session(emptyDocument());
    ObserverCount count;
    auto subscription = session.subscribe(&count, &countObserver);

    const auto rejected =
        session.submit(setParamCommand(session.document().rootNetworkId(), 999, "gain", "1.0"), current(session));
    EXPECT_FALSE(rejected.committed);
    EXPECT_EQ(count.calls, 0);

    const auto added =
        session.submit(addNodeCommand(session.document().rootNetworkId(), "testpattern", "plate"), current(session));
    EXPECT_TRUE(added.committed);
    EXPECT_EQ(count.calls, 1);
    EXPECT_TRUE(session.undo(current(session)).committed);
    EXPECT_EQ(count.calls, 2);
    EXPECT_FALSE(session.undo(current(session)).committed);
    EXPECT_EQ(count.calls, 2);
    EXPECT_TRUE(session.redo(current(session)).committed);
    EXPECT_EQ(count.calls, 3);
    EXPECT_FALSE(session.redo(current(session)).committed);
    EXPECT_EQ(count.calls, 3);

    subscription = ProjectSession::Subscription{};
    static_cast<void>(
        session.submit(addNodeCommand(session.document().rootNetworkId(), "testpattern", "second"), current(session)));
    EXPECT_EQ(count.calls, 3);
}

TEST(ProjectSessionTest, ObserverCanUnsubscribeAndCannotReenterMutation) {
    ProjectSession session(emptyDocument());
    ReentrantObserver reentrant{.session = &session};
    auto subscription = session.subscribe(&reentrant, &reentrantObserver);
    reentrant.subscription = &subscription;
    ObserverCount survivor;
    auto survivorSubscription = session.subscribe(&survivor, &countObserver);

    ASSERT_TRUE(
        session.submit(addNodeCommand(session.document().rootNetworkId(), "testpattern", "plate"), current(session))
            .committed);
    EXPECT_EQ(reentrant.calls, 1);
    EXPECT_TRUE(reentrant.mutationRejected);
    EXPECT_EQ(survivor.calls, 1);
    EXPECT_EQ(rootGraph(session.document()).nodes().size(), 1u);

    ASSERT_TRUE(
        session.submit(addNodeCommand(session.document().rootNetworkId(), "testpattern", "second"), current(session))
            .committed);
    EXPECT_EQ(reentrant.calls, 1);
    EXPECT_EQ(survivor.calls, 2);
    static_cast<void>(survivorSubscription);
}

TEST(ProjectSessionTest, RequestReplayDeduplicatesAcrossLaterEdits) {
    ProjectSession session(emptyDocument());
    EditOptions firstOptions{session.revision(), "create-once"};
    const auto first =
        session.submit(addNodeCommand(session.document().rootNetworkId(), "testpattern", "plate"), firstOptions);
    ASSERT_TRUE(first.committed);
    ASSERT_EQ(
        session.submit(addNodeCommand(session.document().rootNetworkId(), "testpattern", "other"), current(session))
            .committed,
        true);
    const auto replay = session.submit(addNodeCommand(session.document().rootNetworkId(), "testpattern", "plate"),
                                       EditOptions{firstOptions.expectedRevision, firstOptions.requestId});
    EXPECT_TRUE(replay.committed);
    EXPECT_EQ(replay.revision, first.revision);
    EXPECT_EQ(rootGraph(session.document()).nodes().size(), 2u);
}

TEST(ProjectSessionTest, ChangesSinceReportsBoundedHistoryAndResync) {
    ProjectSession session({}, 2);
    ASSERT_TRUE(
        session.submit(addNodeCommand(session.document().rootNetworkId(), "testpattern", "one"), current(session))
            .committed);
    ASSERT_TRUE(
        session.submit(addNodeCommand(session.document().rootNetworkId(), "testpattern", "two"), current(session))
            .committed);
    ASSERT_TRUE(
        session.submit(addNodeCommand(session.document().rootNetworkId(), "testpattern", "three"), current(session))
            .committed);

    const auto history = session.changesSince(2);
    EXPECT_FALSE(history.resyncRequired);
    ASSERT_EQ(history.events.size(), 2u);
    EXPECT_EQ(history.events.back().revision, session.revision());
    EXPECT_EQ(history.events.back().createdNodeIds.size(), 1u);
    EXPECT_TRUE(session.changesSince(1).resyncRequired);
}
TEST(CommandStackTest, FailedPublicationPreparationPreservesUndoAndIdentityHighWatermarks) {
    Document doc = emptyDocument();
    CommandStack stack(doc);
    stack.push(addNodeCommand(doc.rootNetworkId(), "testpattern", "first"));
    const auto first = rootGraph(doc).nodes().front().id;
    const auto revision = doc.stateRevision();
    EXPECT_THROW(stack.undo([](const Document&, const Document&) { throw std::bad_alloc(); }), std::bad_alloc);
    EXPECT_EQ(doc.stateRevision(), revision);
    EXPECT_NE(rootGraph(doc).node(first), nullptr);
    EXPECT_TRUE(stack.canUndo());
    EXPECT_FALSE(stack.canRedo());
    ASSERT_TRUE(stack.undo());
    EXPECT_NE(doc.stateRevision(), revision);
    stack.push(addNodeCommand(doc.rootNetworkId(), "testpattern", "replacement"));
    EXPECT_GT(rootGraph(doc).nodes().front().id, first);
}

TEST(ProjectSessionTest, BatchFailurePreservesHistoryAndValidBatchUndoesEveryEdit) {
    Document doc = emptyDocument();
    const auto source = rootGraph(doc).addNode("testpattern", "source");
    const auto output = rootGraph(doc).addNode("output", "out");
    ProjectSession session(std::move(doc));
    auto commands = std::vector<Command>{setParamCommand(session.document().rootNetworkId(), source, "note", "batch"),
                                         connectCommand(session.document().rootNetworkId(), {source, 0}, {output, 0})};
    const auto committed = session.submit(transactionCommand("valid", std::move(commands)), {1, "batch"});
    ASSERT_TRUE(committed.committed);
    ASSERT_EQ(committed.createdEdgeIds.size(), 1u);
    const auto edge = committed.createdEdgeIds.front().id;
    const auto rejected = session.submit(
        transactionCommand("invalid", {renameNodeCommand(session.document().rootNetworkId(), source, "wrong"),
                                       connectCommand(session.document().rootNetworkId(), {source, 0}, {output, 0})}),
        current(session));
    ASSERT_TRUE(rejected.error);
    EXPECT_EQ(rejected.error->graphError, GraphError::PortOccupied);
    EXPECT_EQ(rejected.revision, committed.revision);
    EXPECT_EQ(rootGraph(session.document()).node(source)->name, "source");
    ASSERT_TRUE(session.undo(current(session)).committed);
    EXPECT_TRUE(rootGraph(session.document()).edges().empty());
    EXPECT_TRUE(rootGraph(session.document()).node(source)->params.empty());
    EXPECT_FALSE(session.canUndo());
    ASSERT_TRUE(session.redo(current(session)).committed);
    EXPECT_EQ(rootGraph(session.document()).edges().front().id, edge);
    EXPECT_EQ(rootGraph(session.document()).node(source)->params.at("note"), "batch");
}

TEST(ProjectSessionTest, SourceEventsAndHistoryRetriesCannotResurrectOldFreshness) {
    ProjectSession session(emptyDocument());
    ObserverCount first, second;
    auto firstSubscription = session.subscribe(&first, &countObserver);
    auto secondSubscription = session.subscribe(&second, &countObserver);
    const auto original = session.snapshot().stateRevision();
    SourceReference reference;
    reference.path = "plate.exr";
    const auto added = session.submit(setSourceCommand("plate", reference), {1, "source"});
    ASSERT_TRUE(added.committed);
    EXPECT_EQ(added.changedSourceIds, (std::vector<std::string>{"plate"}));
    const auto changed = session.snapshot().stateRevision();
    const auto undone = session.undo({added.revision, "undo-once"});
    ASSERT_TRUE(undone.committed);
    EXPECT_TRUE(session.document().sources.empty());
    EXPECT_NE(session.snapshot().stateRevision(), original);
    EXPECT_NE(session.snapshot().stateRevision(), changed);
    ASSERT_TRUE(session.redo(current(session)).committed);
    const auto replay = session.undo({added.revision, "undo-once"});
    EXPECT_EQ(replay.revision, undone.revision);
    EXPECT_TRUE(session.document().sources.contains("plate"));
    EXPECT_EQ(first.calls, 3);
    EXPECT_EQ(second.calls, 3);
    const auto events = session.changesSince(1);
    ASSERT_EQ(events.events.size(), 3u);
    for (const auto& event : events.events)
        EXPECT_EQ(event.changedSourceIds, (std::vector<std::string>{"plate"}));
}

TEST(ProjectSessionTest, MissingJournalAndFutureRevisionRequireResynchronization) {
    ProjectSession session({}, 0);
    ASSERT_TRUE(
        session.submit(addNodeCommand(session.document().rootNetworkId(), "testpattern", "node"), current(session))
            .committed);
    EXPECT_TRUE(session.changesSince(1).resyncRequired);
    EXPECT_FALSE(session.changesSince(session.revision()).resyncRequired);
    EXPECT_TRUE(session.changesSince(UINT64_MAX).resyncRequired);
}

TEST(ProjectSessionTest, FilteredPagesPreserveSparseIdentitiesAndValueBoundaries) {
    Document doc = emptyDocument();
    rootGraph(doc).addNodeWithId(90, "testpattern", "plate-last");
    rootGraph(doc).addNodeWithId(7, "testpattern", "plate-first", {{"a", "one"}, {"b", "two"}});
    rootGraph(doc).addNodeWithId(20, "testpattern", "unrelated");
    ProjectSession session(std::move(doc));
    const NetworkId network = session.document().rootNetworkId();
    const auto first = session.queryNodes(network, "plate", 1);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first.front().id, 7u);
    const auto next = session.queryNodes(network, "plate", 1, first.front().id);
    ASSERT_EQ(next.size(), 1u);
    EXPECT_EQ(next.front().id, 90u);
    EXPECT_TRUE(session.queryNodes(network, "plate", 1, next.front().id).empty());
    const auto values = session.queryValues(network, 7, {}, 1, "a");
    ASSERT_EQ(values.size(), 1u);
    EXPECT_EQ(values.front().key, "b");
    EXPECT_EQ(values.front().value, "two");
    EXPECT_TRUE(session.queryValues(network, 7, {}, 0).empty());
}

TEST(ProjectSessionTest, SharedWorkerSnapshotsRemainImmutableAcrossOwnerEdits) {
    Document document;
    const auto id = rootGraph(document).addNode("testpattern", "plate");
    ProjectSession session(std::move(document));
    const Document snapshot = session.snapshot();
    const Document independentReference = snapshot;
    const auto expected = independentReference.stateRevision();
    std::barrier start(3);
    std::array<bool, 2> unchanged{true, true};
    const auto readSnapshot = [&](std::size_t worker) {
        start.arrive_and_wait();
        for (int read = 0; read < 256; ++read)
            unchanged[worker] = unchanged[worker] && snapshot.stateRevision() == expected &&
                                rootGraph(snapshot).node(id)->name == "plate";
    };
    std::jthread first(readSnapshot, 0);
    std::jthread second(readSnapshot, 1);
    start.arrive_and_wait();
    const auto edited =
        session.submit(renameNodeCommand(session.document().rootNetworkId(), id, "renamed"), current(session));
    first.join();
    second.join();
    ASSERT_TRUE(edited.committed);
    EXPECT_TRUE(unchanged[0]);
    EXPECT_TRUE(unchanged[1]);
    EXPECT_EQ(rootGraph(session.document()).node(id)->name, "renamed");
    EXPECT_NE(session.document().stateRevision(), expected);
}
