#pragma once

#include "nemo/core/document/Roto.hpp"
#include "nemo/core/session/ProjectSession.hpp"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace nemo::ui {

// GUI-thread authoring adapter for ONE Roto node (issue #93).
//
// The adapter is the explicit owner of the transient authoring state of one
// roto node: the tool, the selected element, the selected points and the
// in-progress topology draft. Two presentations of the SAME node (the shape
// list in the inspector and the viewport overlay) share one adapter, so their
// selection is the same selection without any global state; a different node
// has a different adapter.
//
// Authored state never lives here. Reading goes through the published document
// (`node->roto` for the authored hierarchy, `evaluateRoto` for the values and
// geometry at the panel frame); writing goes through the existing command and
// parameter-gesture machinery of ProjectSession, so undo/cancel, keying and
// persistence stay exactly as they are for every other parameter. A topology
// edit (create/remove/reparent/reorder element, add/remove point) is one
// setRotoDataCommand; a numeric edit (position, tangents, tension, feather,
// transform, opacity) is one gesture over element- or point-scoped
// ParameterAddresses and is never committed from an interruption.
class RotoController final : public QObject {
    Q_OBJECT
    // Identity of the authored node this adapter addresses. It is fixed for the
    // adapter's lifetime: the per-node owner map hands out one adapter per node.
    Q_PROPERTY(QString networkId READ networkId CONSTANT)
    Q_PROPERTY(QString nodeId READ nodeId CONSTANT)
    // True when the addressed node exists and is a Roto node. A roto node
    // without authored data is available: authoring the first shape creates it.
    Q_PROPERTY(bool available READ available NOTIFY dataChanged)
    Q_PROPERTY(QString reason READ reason NOTIFY dataChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    // The panel frame values and keys are read and authored at.
    Q_PROPERTY(int frame READ frame WRITE setFrame NOTIFY frameChanged)
    Q_PROPERTY(bool viewerAttached READ viewerAttached NOTIFY viewerAttachedChanged)
    // Session revision the published snapshot was derived from. A presenter
    // binds to it to re-read `parameterState`.
    Q_PROPERTY(int revision READ revision NOTIFY dataChanged)
    // Flat, depth-first authored hierarchy: one record per element (groups
    // included) with its parent, kind, blend and flags. Lifetime-inactive
    // elements stay listed because this is the authored structure.
    Q_PROPERTY(QVariantList elements READ elements NOTIFY dataChanged)
    // The evaluated drawing geometry at `frame`: every contributing element with
    // its points in image coordinates, its tangents as image-space offsets, its
    // per-point feather as an outward offset and the element's local->image
    // affine. Groups carry no points.
    Q_PROPERTY(QVariantList geometry READ geometry NOTIFY dataChanged)
    // Points of the selected element in that element's OWN coordinates, with
    // their authored ids and frame-evaluated values. The refinement controls
    // (tension, per-point feather, smooth/cusp) address exactly these.
    Q_PROPERTY(QVariantList points READ points NOTIFY dataChanged)
    Q_PROPERTY(QString selectedElement READ selectedElement NOTIFY selectionChanged)
    Q_PROPERTY(QStringList selectedPoints READ selectedPoints NOTIFY selectionChanged)
    // Every selected shape, in selection order. `selectedElement` stays the
    // primary (last selected) one the parameter surface addresses; a point
    // selection carries its owners here, so a point of a second shape is never
    // an addressing contradiction.
    Q_PROPERTY(QStringList selectedElements READ selectedElements NOTIFY selectionChanged)
    // Sorted unique frames the current selection's geometry channels hold keys
    // at, so a compact spline surface can state the keys it acts on. Empty when
    // nothing is selected.
    Q_PROPERTY(QVariantList keyTimes READ keyTimes NOTIFY keyTimesChanged)
    // Active viewport tool: select, bezier, bspline, rectangle, ellipse.
    Q_PROPERTY(QString tool READ tool NOTIFY toolChanged)
    Q_PROPERTY(bool draftActive READ draftActive NOTIFY draftChanged)
    Q_PROPERTY(QString draftKind READ draftKind NOTIFY draftChanged)
    // Draft outline in image coordinates while a shape is being drawn.
    Q_PROPERTY(QVariantList draftPoints READ draftPoints NOTIFY draftChanged)
    // True while one numeric gesture owns the session preview.
    Q_PROPERTY(bool gestureActive READ gestureActive NOTIFY gestureChanged)
public:
    RotoController(ProjectSession& session, NetworkId network, NodeId node, QObject* parent = nullptr);

    [[nodiscard]] QString networkId() const;
    [[nodiscard]] QString nodeId() const;
    [[nodiscard]] bool available() const { return available_; }
    [[nodiscard]] QString reason() const { return reason_; }
    [[nodiscard]] QString error() const { return error_; }
    [[nodiscard]] int frame() const { return frame_; }
    [[nodiscard]] int revision() const { return static_cast<int>(revision_); }
    [[nodiscard]] QVariantList elements() const { return elements_; }
    [[nodiscard]] QVariantList geometry() const { return geometry_; }
    [[nodiscard]] QVariantList points() const { return points_; }
    [[nodiscard]] QString selectedElement() const;
    [[nodiscard]] QStringList selectedPoints() const;
    [[nodiscard]] QStringList selectedElements() const;
    [[nodiscard]] QVariantList keyTimes() const { return keyTimes_; }
    [[nodiscard]] QString tool() const { return tool_; }
    [[nodiscard]] bool draftActive() const { return draftActive_; }
    [[nodiscard]] QString draftKind() const { return draftKind_; }
    [[nodiscard]] QVariantList draftPoints() const;
    [[nodiscard]] bool gestureActive() const { return gestureToken_ != 0; }

    void setFrame(int frame);
    // View attachment: the explicit consumer list of this shared adapter. The
    // last detached consumer discards the transient selection and draft, so a
    // closed presenter never leaves a stale highlight for the next one.
    void attachView(QObject* owner);
    Q_INVOKABLE void detachView(QObject* owner);
    // An attached viewer owns the displayed authoring frame. Inspectors use
    // their group clock only when no viewer presents this adapter.
    [[nodiscard]] bool viewerAttached() const;
    Q_INVOKABLE void setViewerFrame(QObject* owner, int frame);

    // --- reading one element/point property --------------------------------
    // {available, kind, value, valueText, label, step, hasMinimum, minimum,
    //  hasMaximum, maximum, choices, keyStatus, animated, keyed}. `pointId`
    // empty addresses the element scope; a non-empty id addresses a point.
    Q_INVOKABLE QVariantMap parameterState(const QString& elementId, const QString& pointId, const QString& key) const;
    Q_INVOKABLE bool setTool(const QString& tool);

    // --- transient selection ----------------------------------------------
    // `selectElement` selects or, with `additive`, toggles ONE shape of the
    // current selection; `selectPoint` resolves the point's owner and toggles
    // the point itself, so a selection spans shapes. Both clear the point mode
    // when they address a shape, and `selectedElement` is always the primary
    // (last selected) element of `selectedElements`.
    Q_INVOKABLE bool selectElement(const QString& elementId, bool additive = false);
    Q_INVOKABLE bool selectPoint(const QString& pointId, bool additive = false);
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE bool pointSelected(const QString& pointId) const;
    Q_INVOKABLE bool elementSelected(const QString& elementId) const;
    // Replace (or, with `additive`, union) the point selection with `pointIds`,
    // across shapes, and state the points' owners as the selected elements.
    Q_INVOKABLE bool setPointSelection(const QStringList& pointIds, bool additive = false);
    // Every point of every unlocked shape, in authored order.
    Q_INVOKABLE bool selectAllPoints();

    // --- selection-wide geometry edits ------------------------------------
    // Delete: the selected points when there is a point selection (a closed
    // shape keeps at least three), else the selected elements with everything
    // they own. ONE topology command.
    Q_INVOKABLE bool deleteSelection();
    // Mirrored tangents (Bezier) or tension 0 (B-spline) for `smooth`; zeroed
    // tangents (Bezier) or tension 1 (B-spline) for a cusp. The selected points
    // when there is a point selection, else every point of the selected
    // contours. Locked shapes and shapes under a locked ancestor are skipped;
    // the whole set is ONE gesture, so ONE undo.
    Q_INVOKABLE bool smoothSelection(bool smooth);
    // Insert a point on the segment from `segment` to its successor at curve
    // parameter `t` (0..1), in the element's own coordinates. A Bezier is split
    // exactly (de Casteljau), so the contour keeps its shape; a B-spline takes
    // one control point sampled on the curve. ONE topology command.
    Q_INVOKABLE bool insertCurvePoint(const QString& elementId, int segment, double t);
    // Current-frame geometry keys (position, tangents or tension, feather) for
    // the selection: the selected points when there is a point selection, else
    // every point of the selected contours. `remove` takes those same keys at
    // the panel frame away. ONE command, whichever the direction.
    Q_INVOKABLE bool keySelection(bool remove = false);

    // --- selection transform (one existing value gesture) ------------------
    // Freezes the selection's geometry and every addressed placement, and opens
    // ONE session gesture over the point channels of the selection: the
    // selected points when there is a point selection, else every point of the
    // selected shapes/groups, recursively (locked shapes and shapes under a
    // locked ancestor excluded, a point selected twice addressed once). Returns
    // the gesture token, or an empty string with `error` set.
    Q_INVOKABLE QString beginSelectionTransform();
    // States the affine on ORIGINAL full-resolution image-pixel coordinates
    // (x' = a*x + c*y + e, y' = b*x + d*y + f) for the frozen sample: positions
    // and tangent vectors both move, each mapped back through the frozen
    // placement, and the live preview is published as `geometry`/`points`. The
    // gesture commits and cancels through the existing commitGesture/
    // cancelGesture. A non-invertible affine is refused so no authored
    // coordinate becomes unreachable.
    Q_INVOKABLE bool updateSelectionTransform(const QString& token, double a, double b, double c, double d, double e,
                                              double f);

    // --- numeric gestures (existing ProjectSession machinery) --------------
    // `targets` is one {element, point, key} map per edited address, in the
    // order `values` arrives; `point` empty addresses the element scope. A
    // drag is one target; a smooth/cusp or multi-point edit is several, and the
    // whole set is ONE preview, ONE history entry, ONE semantic validation.
    Q_INVOKABLE QString beginGesture(const QVariantList& targets);
    Q_INVOKABLE bool updateGesture(const QString& token, const QVariantList& values);
    Q_INVOKABLE bool commitGesture(const QString& token);
    Q_INVOKABLE bool cancelGesture(const QString& token);
    // The cancellation entry point of the shared history owner: Escape or a
    // preview-only Undo discards the live preview and the release that follows
    // publishes nothing. An interrupted gesture is never committed.
    Q_INVOKABLE void cancelHistoryGesture();
    Q_INVOKABLE bool keyAtFrame(const QString& elementId, const QString& pointId, const QString& key);
    Q_INVOKABLE bool removeKeyAtFrame(const QString& elementId, const QString& pointId, const QString& key);

    // --- committed topology (one setRotoDataCommand each) ------------------
    // Rectangle/ellipse from the two corners of a viewport drag.
    Q_INVOKABLE QString createShape(const QString& kind, double x0, double y0, double x1, double y1);
    Q_INVOKABLE QString addGroup(const QString& parentId);
    Q_INVOKABLE bool removeElements(const QStringList& elementIds);
    Q_INVOKABLE bool renameElement(const QString& elementId, const QString& name);
    // blend / firstFrame / lastFrame (null clears the bound).
    Q_INVOKABLE bool setElementProperty(const QString& elementId, const QString& key, const QVariant& value);
    Q_INVOKABLE bool clearLifetime(const QString& elementId);
    // Reorder among siblings by one position (-1 earlier, +1 later).
    Q_INVOKABLE bool moveElement(const QString& elementId, int delta);
    // Reparent (empty parentId = root) and insert at `index` (-1 appends).
    Q_INVOKABLE bool reparentElement(const QString& elementId, const QString& parentId, int index);
    // Insert a point of the element's own path at `index` (-1 appends) in the
    // element's own coordinates.
    Q_INVOKABLE bool addPoint(const QString& elementId, int index, double x, double y);
    Q_INVOKABLE bool removePoints(const QString& elementId, const QStringList& pointIds);
    // Mirrored tangents (Bezier) or tension 0 (B-spline) for `smooth`; zeroed
    // tangents (Bezier) or tension 1 (B-spline) for a cusp. One gesture, so one
    // undo, and a keyed tangent is keyed at the current frame like any other.
    Q_INVOKABLE bool setSmooth(const QString& elementId, const QStringList& pointIds, bool smooth);

    // --- transient topology draft (never document state) -------------------
    Q_INVOKABLE bool beginDraft(const QString& kind, double x, double y);
    Q_INVOKABLE bool updateDraft(double x, double y);
    Q_INVOKABLE bool addDraftPoint(double x, double y);
    // Enter, or a click on the draft's first point, closes it and publishes the
    // one command. A curve draft with fewer than three points is refused.
    Q_INVOKABLE bool commitDraft();
    // Escape (or focus/target/frame change): discards the draft, publishes
    // nothing.
    Q_INVOKABLE void cancelDraft();

    // --- viewing this Roto through a downstream node -----------------------
    // A viewer may draw this node's shapes while it inspects a different node
    // only when the pixel grid is the same one: this node itself, or a target
    // this node reaches through nodes that carry the grid through unchanged
    // (grade, blur, merge, shuffle, roto, viewer, output), with NO other route
    // that passes through a node whose mapping is not proven (transform,
    // reformat, crop, a nested occurrence, a type this build does not model).
    // An unrelated target reports no reason at all, so a presenter can keep
    // looking; an upstream target whose mapping is not proven reports why.
    Q_INVOKABLE bool canOverlayViewer(const QString& target) const;
    Q_INVOKABLE QString overlayReason(const QString& target) const;

signals:
    void dataChanged();
    void selectionChanged();
    void toolChanged();
    void draftChanged();
    void gestureChanged();
    void frameChanged();
    void errorChanged();
    // The frames (sorted, unique) the current selection's geometry channels
    // hold keys at changed.
    void keyTimesChanged();
    void viewerAttachedChanged();
    // Explicit inspector navigation, not a broadcast of every frame change:
    // each attached viewer seeks through its existing transport owner.
    void seekRequested(int frame);

private:
    // One address inside this node: the element scope when `point` is 0.
    struct Scope {
        RotoElementId element{};
        RotoPointId point{};
    };
    static void sessionChanged(void* context) noexcept;
    // Re-reads the published document: availability, the authored hierarchy,
    // the evaluated geometry at `frame`, the selected element's points and the
    // revision. Cancels a live gesture whose target vanished.
    void refresh();
    bool fail(const QString& message);
    void clearError();

    [[nodiscard]] const RotoData* authoredData() const { return hasAuthored_ ? &authored_ : nullptr; }
    [[nodiscard]] const RotoData* evaluatedData() const;
    [[nodiscard]] const RotoElement* findElement(const RotoData& data, RotoElementId id) const;
    [[nodiscard]] const RotoElement* findPointElement(const RotoData& data, RotoPointId id) const;
    [[nodiscard]] bool elementLocked(RotoElementId id) const;
    // The model's lock is per element; an authoring gesture that walks a
    // hierarchy refuses a shape whose ancestor is locked too, because the
    // ancestor's transform is what the shape's pixels are stated in.
    [[nodiscard]] bool elementEffectivelyLocked(const RotoData& data, RotoElementId id) const;
    // The element's authored value as the panel displays it: the frame-evaluated
    // record when the element contributes at `frame`, else its authored record.
    // Every frozen drag sample and every current value comes from here, so a
    // gesture and the records it previews always agree.
    [[nodiscard]] const RotoData& displayData(RotoElementId id) const;
    [[nodiscard]] std::vector<Scope> selectionScopes() const;
    void collectScopes(const RotoData& data, RotoElementId id, std::set<RotoPointId>& seen,
                       std::vector<Scope>& scopes) const;
    // True when the element is selected, or when it is inside a selected group.
    [[nodiscard]] bool shapeSelected(RotoElementId id) const;
    // Adds the element once and makes it the primary selection.
    void selectElementSilently(RotoElementId id);
    // The frames the addressed channels hold keys at, sorted and unique.
    [[nodiscard]] QVariantList keyTimesFor(const std::vector<Scope>& scopes) const;
    // One gesture over the tangents/tension of `scopes`, shared by the
    // element-scoped and the selection-wide smooth/cusp commands.
    bool smoothScopes(const std::vector<Scope>& scopes, bool smooth);
    // Re-states `geometry`/`points` from the live gesture preview snapshot, so a
    // dragged selection shows the values it is stating before the release
    // publishes them. The published document stays untouched.
    void refreshPreviewGeometry();
    [[nodiscard]] std::optional<ParameterAddress> addressFor(const Scope& scope, const QString& key) const;
    // The value an address holds right now: the frame-evaluated value when the
    // element contributes at `frame`, else its authored value. This is exactly
    // what the presenter displays and what a gesture starts from.
    [[nodiscard]] std::optional<ParameterValue> currentValue(const Scope& scope, const QString& key) const;
    [[nodiscard]] std::optional<ParameterValue> convert(const ParameterSpec& spec, const QVariant& value,
                                                        QString& error);
    // Commits one RotoData version through the single topology command.
    bool commitData(RotoData data);
    [[nodiscard]] std::vector<const RotoElement*> orderedElements(const RotoData& data) const;
    [[nodiscard]] QVariantMap elementRecord(const RotoElement& element, std::size_t depth) const;
    [[nodiscard]] QVariantMap geometryRecord(const RotoElement& element, const RotoData& data) const;
    [[nodiscard]] QVariantList pointRecords(const RotoElement& element) const;
    // local -> image affine of one element inside one data version, with every
    // ancestor's transform composed in (own transform first, root last).
    struct Placement {
        double a{1}, b{}, c{}, d{1}, e{}, f{};
    };
    [[nodiscard]] std::optional<Placement> placementOf(const RotoData& data, RotoElementId id) const;
    [[nodiscard]] Placement localPlacement(const RotoElement& element, double pixelAspect) const;
    [[nodiscard]] double networkPixelAspect() const;
    // The element a new shape is created in: the selected group, else root.
    [[nodiscard]] RotoElementId creationParent() const;
    [[nodiscard]] QString uniqueName(const RotoData& data, const QString& base) const;
    // Returns the viewport tool to Select after a finished draw, so the pen is a
    // mode and not a sticky state.
    void finishDraftTool();

    ProjectSession& session_;
    NetworkId network_{kInvalidNetwork};
    NodeId node_{kInvalidNode};
    ProjectSession::Subscription subscription_;
    QString error_;
    QString reason_;
    bool available_{false};
    int frame_{};
    std::uint64_t revision_{};
    RotoData authored_;
    bool hasAuthored_{false};
    std::optional<RotoData> evaluated_;
    QVariantList elements_;
    QVariantList geometry_;
    QVariantList points_;
    RotoElementId selectedElement_{};
    std::vector<RotoPointId> selectedPoints_;
    // Every selected shape in selection order; `selectedElement_` is always one
    // of them (or 0 when nothing is selected).
    std::vector<RotoElementId> selectedElements_;
    QVariantList keyTimes_;
    // One frozen sample per point of a live selection transform: the element and
    // point identities, the ORIGINAL local position and tangents, and the
    // ORIGINAL local -> image placement. Every update composes from this sample,
    // so a drag never accumulates and a release publishes what was last stated.
    struct TransformTarget {
        RotoElementId element{};
        RotoPointId point{};
        Vector2Value position{};
        Vector2Value inTangent{};
        Vector2Value outTangent{};
        Placement placement{};
    };
    std::vector<TransformTarget> transformTargets_;
    // The session's transient preview of the live gesture, held as a shared
    // handle (the session owns the value) so `geometry`/`points` can state it.
    std::shared_ptr<const Document> preview_;
    // Draft: authored in image coordinates and never document state. `draft_`
    // holds the points a click already committed; `draftLive_` is the rubber
    // band the pointer is currently stating.
    QString tool_{QStringLiteral("select")};
    bool draftActive_{false};
    QString draftKind_;
    std::vector<Vector2Value> draft_;
    std::optional<Vector2Value> draftLive_;
    // One live numeric gesture: the session's token plus every address it owns,
    // in the order the values arrive.
    ParameterGestureToken gestureToken_{0};
    std::vector<ParameterAddress> gestureAddresses_;
    std::vector<ParameterSpec> gestureSpecs_;
    bool gestureInvalid_{false};
    struct AttachedView {
        QPointer<QObject> owner;
        bool viewer{false};
    };
    std::vector<AttachedView> views_;
};

}  // namespace nemo::ui
