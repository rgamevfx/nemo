#include <algorithm>
#include <gtest/gtest.h>

#include <array>
#include <barrier>
#include <thread>

#include "nemo/core/commands/AnimationCommands.hpp"
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
    stack.push(setParamCommand(doc.rootNetworkId(), plate, "gain", ParameterValue{std::string{"2.0"}}));
    ASSERT_NE(rootGraph(doc).node(plate), nullptr);
    EXPECT_EQ(rootGraph(doc).node(plate)->params.at("gain"), ParameterValue{std::string{"2.0"}});
    EXPECT_EQ(stack.depth(), 1u);
}

TEST(CommandStackTest, UndoRestoresAndRedoReapplies) {
    Document doc = emptyDocument();
    CommandStack stack(doc);
    const NodeId plate = rootGraph(doc).addNode("testpattern", "plate");

    stack.push(setParamCommand(doc.rootNetworkId(), plate, "gain", ParameterValue{std::string{"2.0"}}));
    stack.push(setParamCommand(doc.rootNetworkId(), plate, "gain", ParameterValue{std::string{"4.0"}}));
    EXPECT_EQ(rootGraph(doc).node(plate)->params.at("gain"), ParameterValue{std::string{"4.0"}});

    EXPECT_TRUE(stack.undo());
    EXPECT_EQ(rootGraph(doc).node(plate)->params.at("gain"), ParameterValue{std::string{"2.0"}});
    EXPECT_TRUE(stack.undo());
    EXPECT_EQ(rootGraph(doc).node(plate)->params.count("gain"), 0u);
    EXPECT_FALSE(stack.undo());

    EXPECT_TRUE(stack.redo());
    EXPECT_EQ(rootGraph(doc).node(plate)->params.at("gain"), ParameterValue{std::string{"2.0"}});
    EXPECT_TRUE(stack.redo());
    EXPECT_EQ(rootGraph(doc).node(plate)->params.at("gain"), ParameterValue{std::string{"4.0"}});
    EXPECT_FALSE(stack.redo());
}

TEST(CommandStackTest, NewCommandDropsRedoBranch) {
    Document doc = emptyDocument();
    CommandStack stack(doc);
    const NodeId plate = rootGraph(doc).addNode("testpattern", "plate");
    stack.push(setParamCommand(doc.rootNetworkId(), plate, "gain", ParameterValue{std::string{"2.0"}}));
    ASSERT_TRUE(stack.undo());

    stack.push(setParamCommand(doc.rootNetworkId(), plate, "gain", ParameterValue{std::string{"8.0"}}));
    EXPECT_FALSE(stack.canRedo());
    EXPECT_EQ(rootGraph(doc).node(plate)->params.at("gain"), ParameterValue{std::string{"8.0"}});
}

TEST(CommandStackTest, FailingApplyLeavesDocumentUntouched) {
    Document doc = emptyDocument();
    CommandStack stack(doc);
    const NodeId plate = rootGraph(doc).addNode("testpattern", "plate");
    const std::size_t nodeCountBefore = rootGraph(doc).nodes().size();
    const auto revisionBefore = doc.stateRevision();
    EXPECT_THROW(stack.push(setParamCommand(doc.rootNetworkId(), 999, "gain", ParameterValue{std::string{"1.0"}})),
                 std::runtime_error);
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

    auto transaction = transactionCommand(
        "invalid batch",
        {setParamCommand(doc.rootNetworkId(), source, "authored", ParameterValue{std::string{"value"}}),
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
    stack.push(setParamCommand(doc.rootNetworkId(), plate, "gain", ParameterValue{std::string{"1.0"}}));
    stack.push(setParamCommand(doc.rootNetworkId(), plate, "gain", ParameterValue{std::string{"2.0"}}));
    stack.push(setParamCommand(doc.rootNetworkId(), plate, "gain", ParameterValue{std::string{"3.0"}}));
    EXPECT_EQ(stack.depth(), 2u);
    EXPECT_TRUE(stack.undo());
    EXPECT_TRUE(stack.undo());
    EXPECT_FALSE(stack.undo());
    EXPECT_EQ(rootGraph(doc).node(plate)->params.at("gain"), ParameterValue{std::string{"1.0"}});
}

TEST(ProjectSessionTest, SharesDocumentHistoryAndPreservesSnapshots) {
    ProjectSession session(emptyDocument());
    const auto created =
        session.submit(addNodeCommand(session.document().rootNetworkId(), "testpattern", "plate"), current(session));
    ASSERT_TRUE(created.committed);
    const NodeId plate = created.createdNodeIds.front().id;
    const Document beforeEdit = session.snapshot();
    const auto edit = session.submit(
        setParamCommand(session.document().rootNetworkId(), plate, "gain", ParameterValue{std::string{"2.0"}}),
        current(session));
    ASSERT_TRUE(edit.committed);
    EXPECT_EQ(rootGraph(session.document()).node(plate)->params.at("gain"), ParameterValue{std::string{"2.0"}});
    EXPECT_EQ(rootGraph(beforeEdit).node(plate)->params.count("gain"), 0u);
    EXPECT_TRUE(session.undo(current(session)).committed);
    EXPECT_EQ(rootGraph(session.document()).node(plate)->params.count("gain"), 0u);
    EXPECT_TRUE(session.redo(current(session)).committed);
    EXPECT_EQ(rootGraph(session.snapshot()).node(plate)->params.at("gain"), ParameterValue{std::string{"2.0"}});
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

    const auto rejected = session.submit(
        setParamCommand(session.document().rootNetworkId(), 999, "gain", ParameterValue{std::string{"1.0"}}),
        current(session));
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

TEST(CommandStackTest, TypedParameterBatchResetAndUndoRedoAreAtomic) {
    Document doc = emptyDocument();
    const auto node = rootGraph(doc).addNode("testpattern", "node");
    CommandStack stack(doc);
    const auto network = doc.rootNetworkId();

    stack.push(setParametersCommand({{ParameterAddress{network, node, "first"}, ParameterValue{std::string{"one"}}},
                                     {ParameterAddress{network, node, "second"}, ParameterValue{std::int64_t{2}}}}));
    ASSERT_EQ(stack.depth(), 1u);
    EXPECT_EQ(rootGraph(doc).node(node)->params.at("first"), ParameterValue{std::string{"one"}});
    EXPECT_EQ(rootGraph(doc).node(node)->params.at("second"), ParameterValue{std::int64_t{2}});

    stack.push(resetParamCommand(network, node, "first"));
    ASSERT_EQ(stack.depth(), 2u);
    EXPECT_FALSE(rootGraph(doc).node(node)->params.contains("first"));
    ASSERT_TRUE(stack.undo());
    EXPECT_EQ(rootGraph(doc).node(node)->params.at("first"), ParameterValue{std::string{"one"}});
    ASSERT_TRUE(stack.redo());
    EXPECT_FALSE(rootGraph(doc).node(node)->params.contains("first"));
    ASSERT_TRUE(stack.undo());
    ASSERT_TRUE(stack.undo());
    EXPECT_TRUE(rootGraph(doc).node(node)->params.empty());
    ASSERT_TRUE(stack.redo());
    ASSERT_TRUE(stack.redo());
    EXPECT_EQ(rootGraph(doc).node(node)->params.at("second"), ParameterValue{std::int64_t{2}});
}

TEST(CommandStackTest, InvalidTypedBatchLeavesDocumentAndHistoryUntouched) {
    Document doc = emptyDocument();
    const auto node = rootGraph(doc).addNode("testpattern", "node");
    const auto color = rootGraph(doc).addNode("constcolor", "color");
    CommandStack stack(doc);
    const auto revision = doc.stateRevision();
    const auto batch = setParametersCommand(
        {{ParameterAddress{doc.rootNetworkId(), node, "valid"}, ParameterValue{std::string{"value"}}},
         {ParameterAddress{doc.rootNetworkId(), color, "color"}, ParameterValue{std::int64_t{1}}}});

    EXPECT_THROW(stack.push(batch), GraphException);
    EXPECT_EQ(doc.stateRevision(), revision);
    EXPECT_EQ(stack.depth(), 0u);
    EXPECT_TRUE(rootGraph(doc).node(node)->params.empty());
    EXPECT_TRUE(rootGraph(doc).node(color)->params.empty());
}

TEST(CommandStackTest, InstanceParameterBatchRequiresDefinitionNetworkScope) {
    Document doc = emptyDocument();
    const auto definition = doc.addNetwork("Definition");
    const auto target = doc.network(definition).graph().addNode("testpattern", "target");
    const auto instance = doc.addInstance(doc.rootNetworkId(), definition, "instance");
    CommandStack stack(doc);
    const auto wrongScope = setParametersCommand(
        {{ParameterAddress{doc.rootNetworkId(), target, "override", instance}, ParameterValue{std::string{"wrong"}}}});
    EXPECT_THROW(stack.push(wrongScope), GraphException);
    EXPECT_TRUE(doc.instance(instance)->params.empty());

    stack.push(setParametersCommand(
        {{ParameterAddress{definition, target, "override", instance}, ParameterValue{std::string{"right"}}}}));
    EXPECT_EQ(doc.instance(instance)->params.at(target).at("override"), ParameterValue{std::string{"right"}});
    ASSERT_TRUE(stack.undo());
    EXPECT_TRUE(doc.instance(instance)->params.empty());
}

TEST(ProjectSessionTest, BatchFailurePreservesHistoryAndValidBatchUndoesEveryEdit) {
    Document doc = emptyDocument();
    const auto source = rootGraph(doc).addNode("testpattern", "source");
    const auto output = rootGraph(doc).addNode("output", "out");
    ProjectSession session(std::move(doc));
    auto commands = std::vector<Command>{
        setParamCommand(session.document().rootNetworkId(), source, "note", ParameterValue{std::string{"batch"}}),
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
    EXPECT_EQ(rootGraph(session.document()).node(source)->params.at("note"), ParameterValue{std::string{"batch"}});
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
    rootGraph(doc).addNodeWithId(
        7, "testpattern", "plate-first",
        ParameterValues{{"a", ParameterValue{std::string{"one"}}}, {"b", ParameterValue{std::string{"two"}}}});

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
    EXPECT_EQ(values.front().value, ParameterValue{std::string{"two"}});

    EXPECT_TRUE(session.queryValues(network, 7, {}, 0).empty());
}
TEST(ProjectSessionTest, ValueQueriesIncludeTypedCatalogDefaults) {
    Document doc = emptyDocument();
    const auto node = rootGraph(doc).addNode("constcolor", "color");
    ProjectSession session(std::move(doc));
    const auto values = session.queryValues(session.document().rootNetworkId(), node);
    ASSERT_EQ(values.size(), 1u);
    EXPECT_EQ(values.front().key, "color");
    EXPECT_EQ(values.front().value, (ParameterValue{ColorValue{{1.0F, 1.0F, 1.0F, 1.0F}}}));

    ASSERT_TRUE(session
                    .submit(setParamCommand(session.document().rootNetworkId(), node, "color",
                                            ParameterValue{ColorValue{{0.25F, 0.5F, 0.75F, 1.0F}}}),
                            current(session))
                    .committed);
    const auto authored = session.queryValues(session.document().rootNetworkId(), node);
    ASSERT_EQ(authored.size(), 1u);
    EXPECT_EQ(authored.front().value, (ParameterValue{ColorValue{{0.25F, 0.5F, 0.75F, 1.0F}}}));
}
TEST(ProjectSessionTest, ParameterGesturePreviewsAreTransientAndCommitOnce) {
    Document doc = emptyDocument();
    const auto node = rootGraph(doc).addNode("testpattern", "node");
    ProjectSession session(std::move(doc));
    const auto network = session.document().rootNetworkId();
    const auto before = session.snapshot();

    const auto begin = session.beginParameterGesture(
        {{ParameterAddress{network, node, "gain"}, ParameterValue{std::string{"one"}}}}, current(session));
    ASSERT_EQ(begin.result.error, std::nullopt);
    ASSERT_NE(begin.token, 0u);
    ASSERT_NE(begin.snapshot, nullptr);
    EXPECT_FALSE(session.document().network(network).graph().node(node)->params.contains("gain"));
    EXPECT_FALSE(session.canUndo());
    EXPECT_EQ(session.revision(), 1u);

    Document isolated = *begin.snapshot;
    isolated.network(network).graph().setParam(node, "gain", ParameterValue{std::string{"isolated"}});
    EXPECT_EQ(begin.snapshot->network(network).graph().node(node)->params.at("gain"),
              ParameterValue{std::string{"one"}});

    const auto updated = session.updateParameterGesture(
        begin.token, {{ParameterAddress{network, node, "gain"}, ParameterValue{std::string{"two"}}}});
    ASSERT_EQ(updated.result.error, std::nullopt);
    ASSERT_NE(updated.snapshot, nullptr);
    EXPECT_EQ(updated.snapshot->network(network).graph().node(node)->params.at("gain"),
              ParameterValue{std::string{"two"}});
    EXPECT_FALSE(session.document().network(network).graph().node(node)->params.contains("gain"));
    EXPECT_EQ(session.snapshot().stateRevision(), before.stateRevision());

    const auto committed = session.commitParameterGesture(begin.token, current(session));
    ASSERT_TRUE(committed.committed);
    EXPECT_TRUE(session.canUndo());
    EXPECT_EQ(session.document().network(network).graph().node(node)->params.at("gain"),
              ParameterValue{std::string{"two"}});
    ASSERT_TRUE(session.undo(current(session)).committed);
    EXPECT_FALSE(session.document().network(network).graph().node(node)->params.contains("gain"));
    ASSERT_TRUE(session.redo(current(session)).committed);
    EXPECT_EQ(session.document().network(network).graph().node(node)->params.at("gain"),
              ParameterValue{std::string{"two"}});
}

TEST(ProjectSessionTest, ParameterGestureCancelAndConflictDoNotPublish) {
    Document doc = emptyDocument();
    const auto node = rootGraph(doc).addNode("testpattern", "node");
    ProjectSession session(std::move(doc));
    const auto network = session.document().rootNetworkId();
    const auto before = session.snapshot();

    const auto begin = session.beginParameterGesture(
        {{ParameterAddress{network, node, "gain"}, ParameterValue{std::string{"preview"}}}}, current(session));
    ASSERT_NE(begin.snapshot, nullptr);
    const auto cancelled = session.cancelParameterGesture(begin.token);
    EXPECT_FALSE(cancelled.committed);
    EXPECT_FALSE(cancelled.error.has_value());
    EXPECT_FALSE(session.canUndo());
    EXPECT_EQ(session.snapshot().stateRevision(), before.stateRevision());
    EXPECT_TRUE(session.changesSince(session.revision()).events.empty());
    EXPECT_TRUE(session.commitParameterGesture(begin.token, current(session)).error.has_value());

    const auto second = session.beginParameterGesture(
        {{ParameterAddress{network, node, "gain"}, ParameterValue{std::string{"preview"}}}}, current(session));
    ASSERT_NE(second.snapshot, nullptr);
    ASSERT_TRUE(
        session
            .submit(setParamCommand(network, node, "other", ParameterValue{std::string{"committed"}}), current(session))
            .committed);
    const auto conflict = session.commitParameterGesture(second.token, current(session));
    ASSERT_FALSE(conflict.committed);
    ASSERT_TRUE(conflict.error.has_value());
    EXPECT_EQ(conflict.error->code, EditErrorCode::RevisionConflict);
    EXPECT_EQ(session.document().network(network).graph().node(node)->params.at("other"),
              ParameterValue{std::string{"committed"}});
    EXPECT_FALSE(session.document().network(network).graph().node(node)->params.contains("gain"));
    EXPECT_TRUE(session.cancelParameterGesture(second.token).error == std::nullopt);
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

TEST(CommandStackTest, GraphNodeDeletionRemovesIncidentAnimationAndUndoRestoresIt) {
    Document document = emptyDocument();
    const NetworkId network = document.rootNetworkId();
    auto& graph = rootGraph(document);
    const NodeId animated = graph.addNode("constcolor", "animated");
    const NodeId merge = graph.addNode("merge", "merge");
    const NodeId output = graph.addNode("output", "delivery");
    const EdgeId first = graph.connect({animated, 0}, {merge, 0});
    const EdgeId second = graph.connect({merge, 0}, {output, 0});
    graph.setRoute(first, {{3.0, 4.0}});
    document.restoreAnimationChannels(
        {{7, ParameterAddress{network, animated, "color"}, {{9, 0.0, ColorValue{{1.0F, 0.0F, 0.0F, 1.0F}}}}}}, 8, 10);
    const Document before = document;

    CommandStack history(document);
    history.push(removeNodeCommand(network, animated));
    EXPECT_EQ(rootGraph(document).node(animated), nullptr);
    ASSERT_EQ(rootGraph(document).edges().size(), 1u);
    EXPECT_EQ(rootGraph(document).edges().front().id, second);
    EXPECT_TRUE(document.animationChannels().empty());
    ASSERT_TRUE(history.undo());
    EXPECT_NE(rootGraph(document).node(animated), nullptr);
    ASSERT_EQ(rootGraph(document).edges().size(), before.network(network).graph().edges().size());
    EXPECT_EQ(rootGraph(document).edges()[0].id, first);
    EXPECT_EQ(rootGraph(document).edges()[0].route, (std::vector<LayoutPosition>{{3.0, 4.0}}));
    EXPECT_EQ(rootGraph(document).edges()[1].id, second);
    ASSERT_EQ(document.animationChannels().size(), 1u);
    EXPECT_EQ(document.animationChannels().front().id, 7u);
}

TEST(CommandStackTest, OccupiedInputReplacementPreservesMergeFanOutAndUndo) {
    Document document = emptyDocument();
    const NetworkId network = document.rootNetworkId();
    auto& initial = rootGraph(document);
    const NodeId first = initial.addNode("testpattern", "first");
    const NodeId second = initial.addNode("testpattern", "second");
    const NodeId merge = initial.addNode("merge", "merge");
    const EdgeId old = initial.connect({first, 0}, {merge, 0});
    const EdgeId fanout = initial.connect({second, 0}, {merge, 1});
    initial.setRoute(old, {{1.0, 2.0}});
    CommandStack history(document);

    history.push(replaceInputCommand(network, {second, 0}, {merge, 0}));
    ASSERT_EQ(rootGraph(document).edges().size(), 2u);
    const auto replacement = std::find_if(
        rootGraph(document).edges().begin(), rootGraph(document).edges().end(),
        [second, merge](const Edge& edge) { return edge.from == PortRef(second, 0) && edge.to == PortRef(merge, 0); });
    ASSERT_NE(replacement, rootGraph(document).edges().end());
    EXPECT_TRUE(std::find_if(rootGraph(document).edges().begin(), rootGraph(document).edges().end(),
                             [fanout](const Edge& edge) { return edge.id == fanout; }) !=
                rootGraph(document).edges().end());
    EXPECT_TRUE(replacement->route.empty());
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(rootGraph(document).edges().size(), 2u);
    EXPECT_TRUE(std::find_if(rootGraph(document).edges().begin(), rootGraph(document).edges().end(),
                             [old](const Edge& edge) { return edge.id == old && edge.route.size() == 1; }) !=
                rootGraph(document).edges().end());
}

TEST(CommandStackTest, InsertNodeOnEdgePreservesDestinationAndOutputFanOut) {
    Document document = emptyDocument();
    const NetworkId network = document.rootNetworkId();
    auto& initial = rootGraph(document);
    const NodeId source = initial.addNode("testpattern", "source");
    const NodeId merge = initial.addNode("merge", "merge");
    const NodeId output = initial.addNode("output", "delivery");
    const EdgeId original = initial.connect({source, 0}, {merge, 0});
    const EdgeId fanout = initial.connect({source, 0}, {output, 0});
    initial.setRoute(original, {{4.0, 5.0}});
    auto inserted = std::make_shared<NodeId>();
    CommandStack history(document);

    history.push(insertNodeOnEdgeCommand(network, original, "merge", "inserted", {20.0, 30.0}, inserted));
    ASSERT_NE(rootGraph(document).node(*inserted), nullptr);
    EXPECT_EQ(rootGraph(document).node(*inserted)->layout, (LayoutPosition{20.0, 30.0}));
    ASSERT_EQ(rootGraph(document).edges().size(), 3u);
    EXPECT_TRUE(std::find_if(rootGraph(document).edges().begin(), rootGraph(document).edges().end(),
                             [fanout, source, output](const Edge& edge) {
                                 return edge.id == fanout && edge.from == PortRef(source, 0) &&
                                        edge.to == PortRef(output, 0);
                             }) != rootGraph(document).edges().end());
    EXPECT_TRUE(std::find_if(rootGraph(document).edges().begin(), rootGraph(document).edges().end(),
                             [inserted, merge](const Edge& edge) {
                                 return edge.from == PortRef(*inserted, 0) && edge.to == PortRef(merge, 0);
                             }) != rootGraph(document).edges().end());
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(rootGraph(document).node(*inserted), nullptr);
    ASSERT_EQ(rootGraph(document).edges().size(), 2u);
    EXPECT_TRUE(std::find_if(rootGraph(document).edges().begin(), rootGraph(document).edges().end(),
                             [original](const Edge& edge) {
                                 return edge.id == original && edge.route == std::vector<LayoutPosition>{{4.0, 5.0}};
                             }) != rootGraph(document).edges().end());
}

TEST(CommandStackTest, RoutePointCommandsAndLayoutBatchAreAtomic) {
    Document document = emptyDocument();
    const NetworkId network = document.rootNetworkId();
    auto& initial = rootGraph(document);
    const NodeId source = initial.addNode("testpattern", "source");
    const NodeId output = initial.addNode("output", "delivery");
    const EdgeId edge = initial.connect({source, 0}, {output, 0});
    initial.setRoute(edge, {{1.0, 1.0}, {2.0, 2.0}});
    CommandStack history(document);

    history.push(insertRoutePointCommand(network, edge, 1, {1.5, 1.5}));
    history.push(moveRoutePointCommand(network, edge, 0, {0.0, 0.0}));
    history.push(removeRoutePointCommand(network, edge, 1));
    EXPECT_EQ(rootGraph(document).edges().front().route, (std::vector<LayoutPosition>{{0.0, 0.0}, {2.0, 2.0}}));
    EXPECT_THROW(history.push(moveRoutePointCommand(network, edge, 9, {0.0, 0.0})), GraphException);

    const auto revision = document.stateRevision();
    EXPECT_THROW(history.push(setLayoutsCommand(network, {{source, {10.0, 20.0}}, {999, {0.0, 0.0}}})), GraphException);
    EXPECT_EQ(document.stateRevision(), revision);
    EXPECT_EQ(rootGraph(document).node(source)->layout, (LayoutPosition{}));
    history.push(setLayoutsCommand(network, {{source, {10.0, 20.0}}, {output, {30.0, 40.0}}}));
    EXPECT_EQ(rootGraph(document).node(source)->layout, (LayoutPosition{10.0, 20.0}));
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(rootGraph(document).node(source)->layout, (LayoutPosition{}));
}

TEST(CommandStackTest, GraphDeletionProtectsFormalOutputTerminal) {
    Document document;
    const NetworkId network = document.rootNetworkId();
    const NodeId output = rootGraph(document).nodeByName("Output")->id;
    const auto before = document.stateRevision();
    CommandStack history(document);
    try {
        history.push(removeNodeCommand(network, output));
        FAIL() << "expected formal output terminal rejection";
    } catch (const GraphException& error) {
        EXPECT_EQ(error.errorCode(), GraphError::InvalidNetwork);
        EXPECT_NE(std::string(error.what()).find(std::to_string(output)), std::string::npos);
    }
    EXPECT_EQ(document.stateRevision(), before);
    EXPECT_NE(rootGraph(document).node(output), nullptr);
    EXPECT_EQ(history.depth(), 0u);
}

TEST(CommandStackTest, ExistingDisconnectedNodeInsertionPreservesEdgeAndRejectsConnectedNode) {
    Document document = emptyDocument();
    const NetworkId network = document.rootNetworkId();
    auto& initial = rootGraph(document);
    const NodeId source = initial.addNode("testpattern", "source");
    const NodeId processing = initial.addNode("merge", "processing");
    const NodeId output = initial.addNode("output", "delivery");
    const EdgeId original = initial.connect({source, 0}, {output, 0});
    initial.setRoute(original, {{7.0, 8.0}});
    CommandStack history(document);

    history.push(insertExistingNodeOnEdgeCommand(network, original, processing, {50.0, 60.0}));
    ASSERT_NE(rootGraph(document).node(processing), nullptr);
    EXPECT_EQ(rootGraph(document).node(processing)->layout, (LayoutPosition{50.0, 60.0}));
    ASSERT_EQ(rootGraph(document).edges().size(), 2u);
    EXPECT_TRUE(std::find_if(rootGraph(document).edges().begin(), rootGraph(document).edges().end(),
                             [processing, output](const Edge& edge) {
                                 return edge.from == PortRef(processing, 0) && edge.to == PortRef(output, 0);
                             }) != rootGraph(document).edges().end());
    ASSERT_TRUE(history.undo());
    ASSERT_EQ(rootGraph(document).edges().size(), 1u);
    EXPECT_EQ(rootGraph(document).edges().front().id, original);
    EXPECT_EQ(rootGraph(document).edges().front().route, (std::vector<LayoutPosition>{{7.0, 8.0}}));

    const NodeId busy = rootGraph(document).addNode("merge", "busy");
    const NodeId secondOutput = rootGraph(document).addNode("output", "second-delivery");
    rootGraph(document).connect({busy, 0}, {secondOutput, 0});
    const auto before = document.stateRevision();
    EXPECT_THROW(history.push(insertExistingNodeOnEdgeCommand(network, original, busy, {1.0, 2.0})), GraphException);
    EXPECT_EQ(document.stateRevision(), before);
    EXPECT_EQ(rootGraph(document).edges().front().id, original);
    EXPECT_EQ(rootGraph(document).node(busy)->layout, (LayoutPosition{}));
}
TEST(CommandStackTest, CreateAfterAnchorPreservesFanoutAndCommitsShiftsAtomically) {
    Document document = emptyDocument();
    const NetworkId network = document.rootNetworkId();
    auto& graph = rootGraph(document);
    const NodeId source = graph.addNode("testpattern", "source");
    const NodeId branch = graph.addNode("merge", "branch");
    const NodeId output = graph.addNode("output", "output");
    graph.connect({source, 0}, {branch, 0});
    graph.connect({source, 0}, {output, 0});

    auto created = std::make_shared<NodeId>();
    CommandStack history(document);
    history.push(
        addNodeCommand(network, "merge", "inserted", created, {25.0, 35.0}, source, {{output, {100.0, 200.0}}}));
    const auto& insertedGraph = rootGraph(document);
    ASSERT_NE(insertedGraph.node(*created), nullptr);
    EXPECT_EQ(insertedGraph.node(*created)->layout, (LayoutPosition{25.0, 35.0}));
    EXPECT_EQ(insertedGraph.node(output)->layout, (LayoutPosition{100.0, 200.0}));
    ASSERT_EQ(insertedGraph.edges().size(), 3u);
    EXPECT_NE(std::find_if(insertedGraph.edges().begin(), insertedGraph.edges().end(),
                           [source, created](const Edge& edge) {
                               return edge.from == PortRef{source, 0} && edge.to == PortRef{*created, 0};
                           }),
              insertedGraph.edges().end());
    EXPECT_EQ(std::count_if(insertedGraph.edges().begin(), insertedGraph.edges().end(),
                            [created](const Edge& edge) { return edge.from.node == *created; }),
              2);
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(rootGraph(document).nodes().size(), 3u);
    EXPECT_EQ(rootGraph(document).edges().size(), 2u);
    EXPECT_EQ(rootGraph(document).node(output)->layout, LayoutPosition{});
}

TEST(CommandStackTest, CreateAfterAnchorMakesSinkBranchAndRejectsInvalidShiftBatch) {
    Document document = emptyDocument();
    const NetworkId network = document.rootNetworkId();
    auto& graph = rootGraph(document);
    const NodeId source = graph.addNode("testpattern", "source");
    const NodeId output = graph.addNode("output", "output");
    graph.connect({source, 0}, {output, 0});
    const auto beforeNodes = graph.nodes().size();
    const auto beforeEdges = graph.edges().size();
    CommandStack history(document);
    EXPECT_THROW(history.push(addNodeCommand(network, "output", "bad", {}, {}, source, {{999, {1.0, 2.0}}})),
                 GraphException);
    EXPECT_EQ(graph.nodes().size(), beforeNodes);
    EXPECT_EQ(graph.edges().size(), beforeEdges);

    auto sink = std::make_shared<NodeId>();
    history.push(addNodeCommand(network, "output", "sink", sink, {10.0, 20.0}, source));
    ASSERT_EQ(rootGraph(document).edges().size(), 2u);
    EXPECT_EQ(std::count_if(rootGraph(document).edges().begin(), rootGraph(document).edges().end(),
                            [source](const Edge& edge) { return edge.from.node == source; }),
              2);
}

TEST(CommandStackTest, RewireNoOpPreservesRouteAndOccupiedTargetReplacementIsAtomic) {
    Document document = emptyDocument();
    const NetworkId network = document.rootNetworkId();
    auto& graph = rootGraph(document);
    const NodeId first = graph.addNode("testpattern", "first");
    const NodeId second = graph.addNode("testpattern", "second");
    const NodeId output = graph.addNode("output", "output");
    const EdgeId edge = graph.connect({first, 0}, {output, 0});
    graph.setRoute(edge, {{4.0, 5.0}});
    CommandStack history(document);
    history.push(rewireGraphEdgeCommand(network, edge, {first, 0}, {output, 0}));
    ASSERT_EQ(rootGraph(document).edges().size(), 1u);
    EXPECT_EQ(rootGraph(document).edges().front().id, edge);
    EXPECT_EQ(rootGraph(document).edges().front().route, (std::vector<LayoutPosition>{{4.0, 5.0}}));

    history.push(rewireGraphEdgeCommand(network, edge, {second, 0}, {output, 0}));
    ASSERT_EQ(rootGraph(document).edges().size(), 1u);
    EXPECT_EQ(rootGraph(document).edges().front().from, (PortRef{second, 0}));
    EXPECT_TRUE(rootGraph(document).edges().front().route.empty());
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(rootGraph(document).edges().front().id, edge);
    EXPECT_EQ(rootGraph(document).edges().front().route, (std::vector<LayoutPosition>{{4.0, 5.0}}));
}
