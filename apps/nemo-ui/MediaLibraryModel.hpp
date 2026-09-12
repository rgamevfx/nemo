#pragma once

// Media Bin adapter (issue #43): the Qt/QML presentation boundary over the
// persistent MediaCatalog and the asynchronous media import service.
//
// Ownership:
//   - Persistent identity, hierarchy, metadata, marks, probe commits and
//     history belong to Document / MediaCatalog / ProjectSession. This model
//     only submits validated commands; it never mutates catalog primitives.
//   - Decode/probe/thumbnail work belongs to media::MediaImportService. This
//     model polls it on the GUI thread, owns no decoder, and receives no
//     worker callbacks.
//   - The bounded thumbnail cache below is presentation-only. It is keyed by
//     runtime request identity and is never part of the persistent document.
//
// The catalog method shape mirrors the archived prototype MediaCatalog.qml so
// MediaBinPanel.qml ports without panel-local persistent state; identities are
// stable strings ("root", "bin-<id>", "media-<id>") and media records carry
// the prototype fields plus read-only probe/runtime additions.
//
// Public record shape:
//   item/children/descendants/path/query records:
//     { id, parentId, kind, name, sourceId, duration, color, tags[], description, offline }
//     kind is "bin" | "video" | "audio" | "still" | "unknown" | "other".
//     Unknown/other are reported honestly (never substituted with a media
//     kind); the panel's neutral glyph covers them and mediaOnly selects them.
//     Bins carry tags/description/color too (the prototype edits any item).
//     sourceId is the stable Document source key (a catalog entry's identity;
//     duplicate entries share it and are never collapsed).
//     `offline`/`kind`/`mediaKind`/`probe`/`probeStatus`/`duration`/`runtime`
//     are DISPLAYED values: for an entry whose live runtime result still
//     matches the source reference and viewing transforms they come from that
//     validated result (no explicit Apply needed), otherwise from authored
//     metadata. authoredKind, authoredOffline and committedProbe expose the
//     persisted values separately; the overlay never rewrites authored
//     metadata, and explicit applyProbe remains the only persistent mutation.
//     Media records add: frameOffset (authored sequence start frame, read-only
//     for relink), marks[] ({inFrame,outFrame}, null when unbounded; per
//     catalog entry, so repeated occurrences of one source key keep their
//     own range), mediaKind ("unknown"|"image"|"video"|"audio"|"sequence"|
//     "other"), probeStatus, probe (null or {width,height,
//     duration,codec,colorPrimaries,colorTransfer,colorMatrix,provenance,
//     status,runtime}), runtime ({frameRate,pixelAspect,pixelFormat,bitDepth,
//     colorRange,chromaLocation,hardware,fallbackReason,streamIndex,profile,
//     planeCount}), thumbnailUrl.
//   query(parentId, text, recursive, filter): filter keys are kind
//   (string or list), offline, unused, mediaOnly, scope, recursive. Media
//   membership is resolved by the core query owner from a typed
//   MediaQueryDescriptor (text/kind/includeImageSequences/unused/scope), so
//   the catalog's search semantics are not duplicated here; the adapter only
//   overlays the runtime availability fact for `offline` and appends the
//   presentation bin rows. A scope bin that no longer exists is unavailable
//   (empty), never a root fallback.
//   smartBins[] records:
//     { id, name, builtIn:false, parentId, query:{parentId,projectScope,
//       searchText,kindFilter,mediaOnly,unusedOnly,offlineOnly} }
//   apply(operation) accepts the prototype operations (createBin, rename,
//   move, duplicate, remove{keepContents}, metadata{changes:{tags,description,
//   color}} on media or bins, collect) plus the persisted smart-bin operations
//   createSmartBin, setSmartBinQuery, renameSmartBin, deleteSmartBin. A
//   saved search with no text and no filter is a valid smart-all query (only
//   an explicit nullopt query is a plain bin). remove{keepContents:true} is
//   the archived prototype's true flatten: every descendant of the removed bin
//   is promoted to its parent through existing move commands, then the empty
//   bin is removed. One call is one atomic history entry; errors surface
//   through `error`.
//   probeState(id) returns { id, sourceId, pending, hasResult, applied,
//   offline, error, kind, runtime, thumbnailUrl }.
//   requestTimelineInsert marks are a list parallel to sourceIds, each
//   { sourceId, inFrame, outFrame } with null for an unbounded range.

#include "PanelContextRouter.hpp"
#include "nemo/core/session/ProjectSession.hpp"
#include "nemo/media/MediaImportService.hpp"

#include <QHash>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QQuickImageProvider>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nemo::workspace {
class WorkspaceController;
}

namespace nemo::ui {

// Shared native chooser (apps/nemo-ui/NativeFileChooser.hpp). The dialog and
// every platform detail belong to that owner; this model only routes requests
// to it and maps its one outcome back to QML signals.
class NativeFileChooser;

// Bounded, thread-safe cache of display-referred thumbnails. The GUI thread
// inserts after a runtime result arrives; QQuickImageProvider may read from the
// render thread, so every access is guarded by the mutex.
class MediaThumbnailCache final {
public:
    explicit MediaThumbnailCache(int capacity = 128);

    void insert(const QString& key, QImage image);
    [[nodiscard]] QImage find(const QString& key) const;
    void clear();

private:
    mutable QMutex mutex_;
    QHash<QString, QImage> images_;
    QList<QString> order_;  // oldest first
    int capacity_;
};

// Image provider registered as `image://nemo-media/<requestId>`. The id is the
// media runtime request identity, so a fresh probe of the same source produces
// a new URL and QML reloads without ad-hoc cache busting. The QML engine takes
// ownership of the provider; the cache is shared with the model so neither
// outlives the other.
class MediaThumbnailProvider final : public QQuickImageProvider {
public:
    explicit MediaThumbnailProvider(std::shared_ptr<MediaThumbnailCache> cache);
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

private:
    std::shared_ptr<MediaThumbnailCache> cache_;
};

class MediaLibraryModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(QVariantList smartBins READ smartBins NOTIFY smartBinsChanged)
public:
    MediaLibraryModel(nemo::ProjectSession& session, nemo::media::MediaImportService& importer,
                      PanelContextRouter* contextRouter = nullptr,
                      nemo::workspace::WorkspaceController* workspace = nullptr, QObject* parent = nullptr);
    ~MediaLibraryModel() override;

    // Optional collaborators. The session and import service are required and
    // injected by the owner; the router owns group-scoped open routing and the
    // workspace owns panel placement/reveal.
    void setContextRouter(PanelContextRouter* contextRouter);
    void setWorkspaceController(nemo::workspace::WorkspaceController* workspace);
    // Shared native file chooser (owned by main.cpp). Optional: without it the
    // chooser invokables report mediaChooserFailed. This model is the requester
    // for the dialogs it starts, so it receives those outcomes and no others.
    void setNativeFileChooser(NativeFileChooser* chooser);
    // Creates the image provider for `image://nemo-media/...`. Call once before
    // the QML types load; the engine takes ownership.
    [[nodiscard]] QQuickImageProvider* createThumbnailProvider();

    [[nodiscard]] int revision() const { return revision_; }
    [[nodiscard]] QString error() const { return error_; }
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    // Persisted smart-query bins as prototype-shaped records
    // ({id,name,builtIn:false,parentId,query}); the panel keeps its own
    // built-in Offline/Unused entries, as the prototype did.
    [[nodiscard]] QVariantList smartBins() const;

    // --- Prototype catalog surface (archived MediaCatalog.qml) -------------
    Q_INVOKABLE QVariant item(const QString& id) const;
    Q_INVOKABLE QVariantList children(const QString& parentId) const;
    Q_INVOKABLE QVariantList descendants(const QString& id) const;
    Q_INVOKABLE QVariantList path(const QString& id) const;
    Q_INVOKABLE QVariantList query(const QString& parentId, const QString& text, bool recursive,
                                   const QVariantMap& filter) const;
    Q_INVOKABLE QVariant sourceItem(const QString& sourceId) const;
    Q_INVOKABLE QVariantList itemsForSource(const QString& sourceId) const;
    Q_INVOKABLE bool isUsed(const QString& sourceId) const;
    Q_INVOKABLE QVariantList normalizeSelection(const QVariantList& ids) const;
    // Validates an operation on a document copy without publishing it.
    Q_INVOKABLE QVariantMap preview(const QVariantMap& operation) const;
    // Applies one catalog gesture as a single atomic history entry.
    Q_INVOKABLE bool apply(const QVariantMap& operation);
    Q_INVOKABLE bool undo();
    Q_INVOKABLE bool redo();
    Q_INVOKABLE bool newBinFromSelection(const QVariantList& ids, const QString& name, const QString& parentId);

    // --- Runtime import / relink / probe -----------------------------------
    // Creates one Document source plus one catalog reference per path in a
    // single transaction (no graph node is created merely for import) and
    // queues a probe/thumbnail job per new source key. `frameOffset` is the
    // authored sequence start frame written into every new reference; it is
    // never guessed from the file.
    Q_INVOKABLE bool importPaths(const QStringList& paths, const QString& parentId, qint64 frameOffset = 0);
    Q_INVOKABLE bool reprobe(const QString& id);
    // The only path that publishes runtime probe data into authored metadata.
    Q_INVOKABLE bool applyProbe(const QString& id);
    Q_INVOKABLE bool relink(const QString& id, const QString& path);
    Q_INVOKABLE QString thumbnailUrl(const QString& id) const;
    // Raw pending/offline/error state for panel presentation; presentation
    // decisions belong to the media panel owner. This is also the lazy feed
    // point for a persisted catalog: called for a visible item it queues a
    // probe when no result/thumbnail exists for the current viewing
    // transforms, so reopening a project populates thumbnails on demand.
    Q_INVOKABLE QVariantMap probeState(const QString& id);

    // --- Routing / reveal / timeline insertion intent ----------------------
    // Explicit open only: this never changes selection.
    Q_INVOKABLE bool openMediaSource(const QString& group, const QString& sourceId);
    // Ordered insertion intent. Repeated catalog occurrences are preserved and
    // each entry carries its marked range; the playhead is never touched and
    // no Timeline type is created here.
    Q_INVOKABLE bool requestTimelineInsert(const QString& group, const QStringList& sourceIds, const QString& mode,
                                           const QVariantList& marks);
    // Activates an existing media panel of `group`, creating one when absent.
    Q_INVOKABLE bool revealMediaPanel(const QString& group);
    // Per-catalog-entry authored mark ranges. `id` is a media identity
    // ("media-N"); returns [] for bins/unknown ids. The panel uses this to
    // build the parallel marks list for requestTimelineInsert so repeated
    // occurrences of one source key keep their own range.
    Q_INVOKABLE QVariantList marks(const QString& id) const;
    // The authored sequence start frame of the entry's source reference.
    Q_INVOKABLE qint64 frameOffset(const QString& id) const;

    // Native chooser entry points. The dialog itself belongs to the shared
    // NativeFileChooser owner (no platform code lives here); each request emits
    // exactly one of importPathsChosen / relinkPathChosen / mediaChooserCancelled
    // / mediaChooserFailed, and the panel owns the follow-up importPaths/relink
    // call. A chooser can only select existing files, so an explicit sequence
    // pattern (e.g. /shots/plate.####.exr) is typed into the panel instead.
    Q_INVOKABLE void chooseImportPaths(const QString& parentId);
    Q_INVOKABLE void chooseRelinkPath(const QString& id);

signals:
    void revisionChanged();
    void errorChanged();
    void historyChanged();
    void catalogChanged();
    void smartBinsChanged();
    // A runtime probe/thumbnail result for `id` is stored; applyProbe(id) is
    // the only path that publishes it into authored metadata.
    void probeResultAvailable(const QString& id);
    // The runtime could not serve `id`, or a result became stale; `reason` is
    // diagnostic text, not a persistent state.
    void probeRejected(const QString& id, const QString& reason);
    void thumbnailChanged(const QString& sourceId);
    void timelineInsertRequested(const QString& group, const QStringList& sourceIds, const QString& mode,
                                 const QVariantList& marks);
    // Exactly one of these is emitted per chooser request. The panel owns the
    // follow-up call (importPaths / relink) and failure presentation.
    void importPathsChosen(const QStringList& paths, const QString& parentId);
    void relinkPathChosen(const QString& id, const QString& path);
    void mediaChooserCancelled();
    void mediaChooserFailed(const QString& message);

private:
    struct InFlightProbe {
        std::uint64_t requestId{0};
        MediaSourceId entry{kInvalidMediaSource};
        SourceReference expected;
        // The probe result identity is the request, and the request carries
        // the viewing transforms; a result whose transforms no longer match
        // the document is stale even when the source reference is unchanged.
        ColorPolicy colorPolicy;
        std::string colorConfig;
    };
    struct StoredProbe {
        nemo::media::MediaImportResult result;
        MediaSourceId entry{kInvalidMediaSource};
        SourceReference expected;
        ColorPolicy colorPolicy;
        std::string colorConfig;
    };
    struct ThumbnailIndexEntry {
        std::uint64_t requestId{0};
        ColorPolicy colorPolicy;
        std::string colorConfig;
    };

    static void sessionChanged(void* context) noexcept;
    void onSessionChanged();
    void setError(QString message);
    void clearError();
    bool finishEdit(const nemo::EditResult& result, const QString& fallback);
    bool submitCommands(const std::string& label, std::vector<nemo::Command> commands);
    static nemo::EditOptions editOptions(const nemo::ProjectSession& session);
    [[nodiscard]] std::uint64_t documentStamp() const;
    [[nodiscard]] std::uint64_t historyStamp() const;

    QVariantMap entryRecord(const MediaCatalogEntry& entry) const;
    // The live runtime result for a source key, only when it still matches the
    // document's source reference and viewing transforms.
    [[nodiscard]] const StoredProbe* validStoredProbe(const std::string& sourceKey) const;
    // Kind shown for an entry: the validated runtime kind when a live result
    // classifies it, otherwise the authored kind. Authored metadata is never
    // rewritten by this overlay.
    [[nodiscard]] MediaKind displayedMediaKind(const MediaCatalogEntry& entry) const;
    // Displayed availability: a live runtime probe's offline fact for the
    // unchanged source reference replaces the authored MediaMetadata.offline
    // (mirroring the core query overlay); with no live fact the authored flag
    // applies. The overlay never writes authored metadata.
    [[nodiscard]] bool effectiveOffline(const MediaCatalogEntry& entry) const;
    QVariantMap binRecord(const MediaBin& bin) const;
    QVariantList itemsSnapshot(const nemo::Document& document) const;
    QVariantMap defaultMarkForSource(const QString& sourceId) const;
    static QVariantMap rootRecord();
    bool enqueueRuntime(MediaSourceId entry);
    // Queues a probe only when the entry has no in-flight, queued, stored or
    // thumbnail result for the current source reference and viewing
    // transforms. Used by the visible-request hook and by color refresh.
    bool ensureRuntime(MediaSourceId entry);
    void refreshThumbnailsForColorChange();
    bool trySubmitRuntime(nemo::media::MediaImportRequest request);
    void flushQueuedRuntime();
    [[nodiscard]] bool runtimePending(const std::string& sourceKey) const;
    void pruneThumbnailIndex();
    void handleRuntimeResult(nemo::media::MediaImportResult result);
    void pollRuntime();
    void startPollingIfNeeded();
    void failMediaChooser(QString message);

    nemo::ProjectSession& session_;
    nemo::media::MediaImportService& importer_;
    PanelContextRouter* contextRouter_{nullptr};
    nemo::workspace::WorkspaceController* workspace_{nullptr};
    NativeFileChooser* chooser_{nullptr};
    nemo::ProjectSession::Subscription subscription_;
    std::shared_ptr<MediaThumbnailCache> thumbnails_;
    QTimer* pollTimer_{nullptr};
    std::map<std::string, InFlightProbe> inFlight_;
    std::map<std::string, StoredProbe> stored_;
    std::map<std::string, ThumbnailIndexEntry> latestRequest_;
    // Requests the service could not accept because its outstanding bound was
    // reached; retried from the poll timer as results are collected.
    std::vector<nemo::media::MediaImportRequest> queued_;
    std::uint64_t nextRequestId_{1};
    std::uint64_t projectGeneration_{0};
    std::uint64_t lastDocumentStamp_{0};
    std::uint64_t lastHistoryStamp_{0};
    // Viewing-transform state the queued/stored results were requested with.
    ColorPolicy lastColorPolicy_;
    std::string lastColorConfig_;
    int revision_{0};
    QString error_;
};

}  // namespace nemo::ui
