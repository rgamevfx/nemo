#include "PanelContextRouter.hpp"
#include "WorkspaceController.hpp"

#include "nemo/core/commands/MediaCatalogCommands.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/session/ProjectSession.hpp"

#include <QSignalSpy>
#include <QVariantList>
#include <QVariantMap>
#include <gtest/gtest.h>

#include <memory>

namespace {

QVariantMap changes(const char* timeline, const char* source, double clock) {
    return {{QStringLiteral("timelineTarget"), timeline},
            {QStringLiteral("sourceTarget"), source},
            {QStringLiteral("timelineClock"), clock},
            {QStringLiteral("sourceClock"), clock + 1.0}};
}

}  // namespace

TEST(PanelContextRouter, RejectsInvalidValuesWithoutChangingBinding) {
    nemo::ProjectSession session;
    nemo::ui::PanelContextRouter router(session);
    ASSERT_TRUE(router.registerPanel(QStringLiteral("one"), QStringLiteral("A")));
    EXPECT_FALSE(router.registerPanel(QStringLiteral("one"), QStringLiteral("Z")));
    EXPECT_FALSE(router.registerPanel(QString(), QStringLiteral("A")));
    EXPECT_FALSE(router.setGroup(QStringLiteral("one"), QStringLiteral("Z")));
    EXPECT_FALSE(router.setViewerRole(QStringLiteral("one"), QStringLiteral("viewer")));
    EXPECT_EQ(router.viewerRole(QStringLiteral("one")), QStringLiteral("graph"));
    EXPECT_EQ(router.contextFor(QStringLiteral("one")).value(QStringLiteral("group")).toString(), QStringLiteral("A"));
}

TEST(PanelContextRouter, GroupClocksAreIndependentAndNotifyOnlyAffectedPanels) {
    nemo::ProjectSession session;
    nemo::ui::PanelContextRouter router(session);
    ASSERT_TRUE(router.registerPanel(QStringLiteral("a"), QStringLiteral("A")));
    ASSERT_TRUE(router.registerPanel(QStringLiteral("b"), QStringLiteral("B")));
    QSignalSpy changed(&router, &nemo::ui::PanelContextRouter::panelContextChanged);
    ASSERT_TRUE(router.setGroupContext(QStringLiteral("A"), changes("ta", "sa", 4.0)));
    EXPECT_EQ(changed.count(), 1);
    EXPECT_EQ(changed.at(0).at(0).toString(), QStringLiteral("a"));
    EXPECT_EQ(router.contextFor(QStringLiteral("b")).value(QStringLiteral("sourceClock")).toDouble(), 0.0);
    EXPECT_EQ(router.contextFor(QStringLiteral("a")).value(QStringLiteral("timelineClock")).toDouble(), 4.0);
    EXPECT_EQ(router.contextFor(QStringLiteral("a")).value(QStringLiteral("sourceClock")).toDouble(), 5.0);
}

TEST(PanelContextRouter, RetiredRoutingModesAreAbsentFromThePublicApi) {
    nemo::ProjectSession session;
    nemo::ui::PanelContextRouter router(session);
    const auto* meta = router.metaObject();
    EXPECT_EQ(meta->indexOfMethod("setLinkMode(QString,QString)"), -1);
    EXPECT_EQ(meta->indexOfMethod("setGraphTarget(QString,QString)"), -1);
    EXPECT_EQ(meta->indexOfMethod("availableTargets(QString)"), -1);
    EXPECT_EQ(meta->indexOfMethod("registerPanel(QString,QString,QString)"), -1);
    EXPECT_NE(meta->indexOfMethod("registerPanel(QString,QString)"), -1);
    EXPECT_EQ(meta->indexOfSignal("panelBindingChanged(QString)"), -1);
    EXPECT_EQ(meta->indexOfSignal("inspectorRequested(QString,QString)"), -1);
    EXPECT_NE(meta->indexOfSignal("inspectorRequested(QString,QString,QString)"), -1);

    ASSERT_TRUE(router.registerPanel(QStringLiteral("one"), QStringLiteral("A")));
    const auto context = router.contextFor(QStringLiteral("one"));
    EXPECT_FALSE(context.contains(QStringLiteral("mode")));
    EXPECT_FALSE(context.contains(QStringLiteral("resolvedGroup")));
    EXPECT_FALSE(context.contains(QStringLiteral("graphTarget")));
    EXPECT_EQ(context.value(QStringLiteral("group")).toString(), QStringLiteral("A"));
}

TEST(PanelContextRouter, ViewerRoleSelectsTargetWithoutRepurposingGroupContext) {
    nemo::ProjectSession session;
    auto source = session.submit(nemo::setSourceCommand("clip", {.path = "/tmp/clip"}),
                                 {.expectedRevision = session.revision(), .requestId = "role-source"});
    ASSERT_TRUE(source.committed);
    nemo::ui::PanelContextRouter router(session);
    ASSERT_TRUE(router.registerPanel(QStringLiteral("viewer"), QStringLiteral("C")));
    ASSERT_TRUE(router.setTimelineTarget(QStringLiteral("C"), QStringLiteral("timeline-id")));
    ASSERT_TRUE(router.openSource(QStringLiteral("C"), QStringLiteral("clip")));
    ASSERT_TRUE(router.setGroupContext(QStringLiteral("C"),
                                       {{QStringLiteral("timelineClock"), 6.0}, {QStringLiteral("sourceClock"), 7.0}}));

    auto context = router.contextFor(QStringLiteral("viewer"));
    EXPECT_EQ(context.value(QStringLiteral("viewerRole")).toString(), QStringLiteral("graph"));
    // The graph role owns no image gate; the viewer's attached node is resolved
    // by the viewer controller, not by this router.
    EXPECT_FALSE(context.value(QStringLiteral("available")).toBool());
    ASSERT_TRUE(router.setViewerRole(QStringLiteral("viewer"), QStringLiteral("timeline")));
    context = router.contextFor(QStringLiteral("viewer"));
    EXPECT_EQ(context.value(QStringLiteral("viewerRole")).toString(), QStringLiteral("timeline"));
    EXPECT_EQ(context.value(QStringLiteral("timelineTarget")).toString(), QStringLiteral("timeline-id"));
    EXPECT_TRUE(context.value(QStringLiteral("available")).toBool());
    ASSERT_TRUE(router.setViewerRole(QStringLiteral("viewer"), QStringLiteral("media")));
    context = router.contextFor(QStringLiteral("viewer"));
    EXPECT_EQ(context.value(QStringLiteral("sourceTarget")).toString(), QStringLiteral("clip"));
    EXPECT_EQ(context.value(QStringLiteral("group")).toString(), QStringLiteral("C"));
    EXPECT_EQ(session.document().sources.size(), 1);
}

TEST(PanelContextRouter, MediaAvailabilityAndMarksFollowTheDocumentCatalog) {
    nemo::ProjectSession session;
    auto source = session.submit(nemo::setSourceCommand("clip", {.path = "/tmp/clip"}),
                                 {.expectedRevision = session.revision(), .requestId = "source"});
    ASSERT_TRUE(source.committed);
    auto created = std::make_shared<nemo::MediaSourceId>();
    auto imported = session.submit(nemo::importMediaReferenceCommand("clip", nemo::kInvalidMediaBin, {}, created),
                                   {.expectedRevision = session.revision(), .requestId = "import"});
    ASSERT_TRUE(imported.committed);
    auto marked = session.submit(nemo::setMediaMarksCommand(*created, {{12, 34}}),
                                 {.expectedRevision = session.revision(), .requestId = "marks"});
    ASSERT_TRUE(marked.committed);

    nemo::ui::PanelContextRouter router(session);
    ASSERT_TRUE(router.registerPanel(QStringLiteral("viewer"), QStringLiteral("A")));
    ASSERT_TRUE(router.setViewerRole(QStringLiteral("viewer"), QStringLiteral("media")));
    ASSERT_TRUE(router.openSource(QStringLiteral("A"), QStringLiteral("clip")));
    const auto available = router.contextFor(QStringLiteral("viewer"));
    EXPECT_TRUE(available.value(QStringLiteral("available")).toBool());
    const auto marks = available.value(QStringLiteral("sourceMarks")).toList();
    ASSERT_EQ(marks.size(), 1);
    EXPECT_EQ(marks.front().toMap().value(QStringLiteral("inFrame")).toLongLong(), 12);
    EXPECT_EQ(marks.front().toMap().value(QStringLiteral("outFrame")).toLongLong(), 34);

    QSignalSpy changed(&router, &nemo::ui::PanelContextRouter::panelContextChanged);
    auto removed = session.submit(nemo::removeMediaEntryCommand(*created),
                                  {.expectedRevision = session.revision(), .requestId = "remove"});
    auto sourceRemoved = session.submit(nemo::removeSourceCommand("clip"),
                                        {.expectedRevision = session.revision(), .requestId = "remove-source"});
    ASSERT_TRUE(sourceRemoved.committed);
    ASSERT_TRUE(removed.committed);
    EXPECT_EQ(changed.count(), 2);
    const auto unavailable = router.contextFor(QStringLiteral("viewer"));
    EXPECT_FALSE(unavailable.value(QStringLiteral("available")).toBool());
    EXPECT_EQ(unavailable.value(QStringLiteral("unavailableReason")).toString(),
              QStringLiteral("source target is unavailable"));
}

TEST(PanelContextRouter, ExplicitSourceOpenIsGroupScopedAndDoesNotMutateTheDocument) {
    nemo::ProjectSession session;
    const auto source = session.submit(nemo::setSourceCommand("clip", {.path = "/tmp/clip"}),
                                       {.expectedRevision = session.revision(), .requestId = "source-open"});
    ASSERT_TRUE(source.committed);
    const auto revision = session.revision();
    nemo::ui::PanelContextRouter router(session);
    ASSERT_TRUE(router.registerPanel(QStringLiteral("a"), QStringLiteral("A")));
    ASSERT_TRUE(router.registerPanel(QStringLiteral("b"), QStringLiteral("B")));

    EXPECT_FALSE(router.openSource(QStringLiteral("A"), QStringLiteral("missing")));
    ASSERT_TRUE(router.openSource(QStringLiteral("B"), QStringLiteral("clip")));
    EXPECT_TRUE(router.contextFor(QStringLiteral("a")).value(QStringLiteral("sourceTarget")).toString().isEmpty());
    EXPECT_EQ(router.contextFor(QStringLiteral("b")).value(QStringLiteral("sourceTarget")).toString(),
              QStringLiteral("clip"));
    EXPECT_EQ(session.revision(), revision);
}

TEST(PanelContextRouter, InspectorRequestIsGroupScopedAndMediaFree) {
    nemo::ProjectSession session;
    nemo::ui::PanelContextRouter router(session);
    ASSERT_TRUE(router.registerPanel(QStringLiteral("a"), QStringLiteral("A")));
    ASSERT_TRUE(router.registerPanel(QStringLiteral("b"), QStringLiteral("B")));

    // Two receivers mirror the QML parameters panels: each consumes only
    // requests addressed to its own group.
    QVariantList receivedA;
    QVariantList receivedB;
    QObject::connect(&router, &nemo::ui::PanelContextRouter::inspectorRequested, &router,
                     [&](const QString& group, const QString& network, const QString& nodeId) {
                         const QVariantMap entry{{QStringLiteral("group"), group},
                                                 {QStringLiteral("network"), network},
                                                 {QStringLiteral("node"), nodeId}};
                         if (group == QStringLiteral("A"))
                             receivedA.push_back(entry);
                         if (group == QStringLiteral("B"))
                             receivedB.push_back(entry);
                     });

    EXPECT_FALSE(router.requestInspector(QString{}, QStringLiteral("7"), QStringLiteral("42")));
    EXPECT_FALSE(router.requestInspector(QStringLiteral("A"), QString(), QStringLiteral("42")));
    EXPECT_FALSE(router.requestInspector(QStringLiteral("A"), QStringLiteral("7"), QString{}));
    EXPECT_TRUE(receivedA.isEmpty());

    const auto revision = session.revision();
    ASSERT_TRUE(router.requestInspector(QStringLiteral("A"), QStringLiteral("7"), QStringLiteral("42")));
    ASSERT_EQ(receivedA.size(), 1);
    EXPECT_TRUE(receivedB.isEmpty()) << "a group-A request must not reach a group-B receiver";
    const auto entry = receivedA.front().toMap();
    EXPECT_EQ(entry.value(QStringLiteral("group")).toString(), QStringLiteral("A"));
    EXPECT_EQ(entry.value(QStringLiteral("network")).toString(), QStringLiteral("7"));
    EXPECT_EQ(entry.value(QStringLiteral("node")).toString(), QStringLiteral("42"));

    ASSERT_TRUE(router.requestInspector(QStringLiteral("B"), QStringLiteral("9"), QStringLiteral("11")));
    EXPECT_EQ(receivedA.size(), 1);
    ASSERT_EQ(receivedB.size(), 1);
    EXPECT_EQ(receivedB.front().toMap().value(QStringLiteral("node")).toString(), QStringLiteral("11"));

    // The relay never touches the document or the media catalog.
    EXPECT_EQ(session.revision(), revision);
    EXPECT_TRUE(session.document().mediaCatalog.entries().empty());
}
