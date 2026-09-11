#include "PanelContextRouter.hpp"

#include "nemo/core/commands/MediaCatalogCommands.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/session/ProjectSession.hpp"

#include <QSignalSpy>
#include <QVariantMap>
#include <gtest/gtest.h>

#include <memory>

namespace {

QVariantMap changes(const char* graph, const char* timeline, const char* source, double clock) {
    return {{QStringLiteral("graphTarget"), graph},         {QStringLiteral("timelineTarget"), timeline},
            {QStringLiteral("sourceTarget"), source},       {QStringLiteral("graphClock"), clock},
            {QStringLiteral("timelineClock"), clock + 1.0}, {QStringLiteral("sourceClock"), clock + 2.0}};
}

}  // namespace

TEST(PanelContextRouter, RejectsInvalidValuesWithoutChangingBinding) {
    nemo::ProjectSession session;
    nemo::ui::PanelContextRouter router(session);
    ASSERT_TRUE(router.registerPanel(QStringLiteral("one"), QStringLiteral("A"), QStringLiteral("group")));
    EXPECT_FALSE(router.setLinkMode(QStringLiteral("one"), QStringLiteral("bad")));
    EXPECT_FALSE(router.setGroup(QStringLiteral("one"), QStringLiteral("Z")));
    EXPECT_FALSE(router.setViewerRole(QStringLiteral("one"), QStringLiteral("viewer")));
    EXPECT_EQ(router.viewerRole(QStringLiteral("one")), QStringLiteral("graph"));
    EXPECT_EQ(router.contextFor(QStringLiteral("one")).value(QStringLiteral("mode")).toString(),
              QStringLiteral("group"));
}

TEST(PanelContextRouter, GroupClocksAreIndependentAndNotifyOnlyAffectedPanels) {
    nemo::ProjectSession session;
    nemo::ui::PanelContextRouter router(session);
    ASSERT_TRUE(router.registerPanel(QStringLiteral("a"), QStringLiteral("A"), QStringLiteral("group")));
    ASSERT_TRUE(router.registerPanel(QStringLiteral("b"), QStringLiteral("B"), QStringLiteral("group")));
    QSignalSpy changed(&router, &nemo::ui::PanelContextRouter::panelContextChanged);
    ASSERT_TRUE(router.setGroupContext(QStringLiteral("A"), changes("ga", "ta", "sa", 4.0)));
    EXPECT_EQ(changed.count(), 1);
    EXPECT_EQ(changed.at(0).at(0).toString(), QStringLiteral("a"));
    EXPECT_EQ(router.contextFor(QStringLiteral("b")).value(QStringLiteral("graphClock")).toDouble(), 0.0);
    EXPECT_EQ(router.contextFor(QStringLiteral("a")).value(QStringLiteral("timelineClock")).toDouble(), 5.0);
}

TEST(PanelContextRouter, PinnedSnapshotDoesNotBorrowLaterGroupUpdates) {
    nemo::ProjectSession session;
    nemo::ui::PanelContextRouter router(session);
    ASSERT_TRUE(router.registerPanel(QStringLiteral("one"), QStringLiteral("A"), QStringLiteral("group")));
    ASSERT_TRUE(router.setGroupContext(QStringLiteral("A"), changes("old-g", "old-t", "old-s", 1.0)));
    ASSERT_TRUE(router.setActivePanel(QStringLiteral("one")));
    ASSERT_TRUE(router.setLinkMode(QStringLiteral("one"), QStringLiteral("pinned")));
    ASSERT_TRUE(router.setGroupContext(QStringLiteral("A"), changes("new-g", "new-t", "new-s", 9.0)));
    const auto context = router.contextFor(QStringLiteral("one"));
    EXPECT_EQ(context.value(QStringLiteral("mode")).toString(), QStringLiteral("pinned"));
    EXPECT_EQ(context.value(QStringLiteral("graphTarget")).toString(), QStringLiteral("old-g"));
    EXPECT_EQ(context.value(QStringLiteral("graphClock")).toDouble(), 1.0);
}

TEST(PanelContextRouter, FollowReportsUnavailableWithoutActivePanel) {
    nemo::ProjectSession session;
    nemo::ui::PanelContextRouter router(session);
    ASSERT_TRUE(router.registerPanel(QStringLiteral("one"), QStringLiteral("A"), QStringLiteral("follow")));
    const auto context = router.contextFor(QStringLiteral("one"));
    EXPECT_FALSE(context.value(QStringLiteral("available")).toBool());
    EXPECT_EQ(context.value(QStringLiteral("unavailableReason")).toString(), QStringLiteral("no active panel"));
}

TEST(PanelContextRouter, FollowActiveSwitchesResolvedGroupWithoutChangingItsOwnBinding) {
    nemo::ProjectSession session;
    nemo::ui::PanelContextRouter router(session);
    ASSERT_TRUE(router.registerPanel(QStringLiteral("a"), QStringLiteral("A"), QStringLiteral("group")));
    ASSERT_TRUE(router.registerPanel(QStringLiteral("b"), QStringLiteral("B"), QStringLiteral("group")));
    ASSERT_TRUE(router.registerPanel(QStringLiteral("follow"), QStringLiteral("A"), QStringLiteral("follow")));
    ASSERT_TRUE(router.setGroupContext(QStringLiteral("A"), {{QStringLiteral("graphClock"), 10}}));
    ASSERT_TRUE(router.setGroupContext(QStringLiteral("B"), {{QStringLiteral("graphClock"), 20}}));

    ASSERT_TRUE(router.setActivePanel(QStringLiteral("a")));
    EXPECT_EQ(router.contextFor(QStringLiteral("follow")).value(QStringLiteral("graphClock")).toInt(), 10);
    ASSERT_TRUE(router.setActivePanel(QStringLiteral("b")));
    const auto followed = router.contextFor(QStringLiteral("follow"));
    EXPECT_EQ(followed.value(QStringLiteral("graphClock")).toInt(), 20);
    EXPECT_EQ(followed.value(QStringLiteral("group")).toString(), QStringLiteral("A"));
    EXPECT_EQ(followed.value(QStringLiteral("resolvedGroup")).toString(), QStringLiteral("B"));
}

TEST(PanelContextRouter, ViewerRoleSelectsTargetWithoutRepurposingGroupContext) {
    nemo::ProjectSession session;
    auto source = session.submit(nemo::setSourceCommand("clip", {.path = "/tmp/clip"}),
                                 {.expectedRevision = session.revision(), .requestId = "role-source"});
    ASSERT_TRUE(source.committed);
    nemo::ui::PanelContextRouter router(session);
    ASSERT_TRUE(router.registerPanel(QStringLiteral("viewer"), QStringLiteral("C"), QStringLiteral("group")));
    ASSERT_TRUE(router.setGroupContext(QStringLiteral("C"), changes("graph-id", "timeline-id", "clip", 5.0)));

    auto context = router.contextFor(QStringLiteral("viewer"));
    EXPECT_EQ(context.value(QStringLiteral("viewerRole")).toString(), QStringLiteral("graph"));
    EXPECT_TRUE(context.value(QStringLiteral("available")).toBool());
    ASSERT_TRUE(router.setViewerRole(QStringLiteral("viewer"), QStringLiteral("timeline")));
    context = router.contextFor(QStringLiteral("viewer"));
    EXPECT_EQ(context.value(QStringLiteral("viewerRole")).toString(), QStringLiteral("timeline"));
    EXPECT_EQ(context.value(QStringLiteral("timelineTarget")).toString(), QStringLiteral("timeline-id"));
    ASSERT_TRUE(router.setViewerRole(QStringLiteral("viewer"), QStringLiteral("media")));
    context = router.contextFor(QStringLiteral("viewer"));
    EXPECT_EQ(context.value(QStringLiteral("sourceTarget")).toString(), QStringLiteral("clip"));
    EXPECT_EQ(context.value(QStringLiteral("resolvedGroup")).toString(), QStringLiteral("C"));
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
    ASSERT_TRUE(router.registerPanel(QStringLiteral("viewer"), QStringLiteral("A"), QStringLiteral("group")));
    ASSERT_TRUE(router.setViewerRole(QStringLiteral("viewer"), QStringLiteral("media")));
    ASSERT_TRUE(
        router.setGroupContext(QStringLiteral("A"), {{QStringLiteral("sourceTarget"), QStringLiteral("clip")}}));
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
