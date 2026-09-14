#pragma once

// Read node media control (issue #61): the presentation adapter behind the
// node-local file control the inspector hosts through ParameterEditorRegistry
// id "nemo.read.source".
//
// Ownership:
//   - Authored state stays in Document (SourceReference + Media Bin entry) and
//     is mutated only through the validated ReadSource commands. This adapter
//     owns no media model, decoder, or second catalog.
//   - Probing/validation stays with the Media Bin adapter's single import
//     worker; this adapter is one requester of it (requestReferenceProbe) and
//     commits a probe only after the result validates.
//   - The native dialog belongs to NativeFileChooser; this adapter is the
//     requester, so it receives exactly its own outcomes.

#include "NativeFileChooser.hpp"
#include "nemo/core/session/ProjectSession.hpp"
#include "nemo/media/MediaImportService.hpp"

#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantMap>

#include <cstdint>
#include <string>

namespace nemo::ui {

class MediaLibraryModel;

class ReadSourceController final : public QObject {
    Q_OBJECT
public:
    ReadSourceController(nemo::ProjectSession& session, MediaLibraryModel& media, NativeFileChooser& chooser,
                         QObject* parent = nullptr);
    ~ReadSourceController() override;

    // Presentation state of the node's media reference:
    //   { state, sourceKey, path, resolvedPath, frameOffset, frameStep,
    //     firstFrame, lastFrame, pending, error }
    // state is "empty" (no reference authored), "unresolved" (the node names a
    // key the document no longer has), "ready", or "offline" (the resolved
    // frame path is missing). Numeric fields cross as decimal strings so QML
    // never rounds an authored frame.
    Q_INVOKABLE QVariantMap info(const QString& networkId, const QVariant& nodeId) const;

    // Browse for a file (or type a sequence pattern) and point the node at the
    // reference for that path, reusing the document's reference and Media Bin
    // entry for the same normalized path + interpretation.
    Q_INVOKABLE void chooseSource(const QString& networkId, const QVariant& nodeId);
    Q_INVOKABLE bool setSourcePath(const QString& networkId, const QVariant& nodeId, const QString& path);

    // Relink the reference the node already names, in place: identity, timing
    // and interpretation are preserved, so every node sharing it recovers.
    Q_INVOKABLE void chooseRelinkSource(const QString& networkId, const QVariant& nodeId);
    Q_INVOKABLE bool relinkSource(const QString& networkId, const QVariant& nodeId, const QString& path);

    // Authored timing/range of the node's current reference. An empty string
    // leaves the corresponding field unauthored (offset 0, step 1, no bound).
    Q_INVOKABLE bool setSourceTiming(const QString& networkId, const QVariant& nodeId, const QString& frameOffset,
                                     const QString& frameStep, const QString& firstFrame, const QString& lastFrame);

    // Clears the node's reference binding to the explicit empty state. The
    // shared reference and its Media Bin entry are preserved.
    Q_INVOKABLE bool clearSource(const QString& networkId, const QVariant& nodeId);

    [[nodiscard]] QString error() const { return error_; }

signals:
    // The node's media state changed (probe started/finished, command
    // committed, clearing). The hosted control re-reads info().
    void changed();

private:
    enum class ProbeAction { Register, Relink };

    [[nodiscard]] bool resolveNode(const QString& networkId, const QVariant& nodeId, nemo::NetworkId& network,
                                   nemo::NodeId& node) const;
    // The node's current (key, reference) pair; false when it names nothing the
    // document resolves.
    [[nodiscard]] bool currentBinding(nemo::NetworkId network, nemo::NodeId node, std::string& key,
                                      nemo::SourceReference& reference) const;
    void beginProbe(ProbeAction action, nemo::NetworkId network, nemo::NodeId node, QString path);
    void handleProbe(std::uint64_t generation, ProbeAction action, nemo::NetworkId network, nemo::NodeId node,
                     QString path, const nemo::media::MediaImportResult& result);
    void setError(nemo::NetworkId network, nemo::NodeId node, QString message);

    nemo::ProjectSession& session_;
    MediaLibraryModel& media_;
    NativeFileChooser& chooser_;
    QString error_;
    // The node the current error belongs to, so one Read node's rejection is
    // never shown on another.
    nemo::NetworkId errorNetwork_{};
    nemo::NodeId errorNode_{};
    // One outstanding probe: user actions supersede it by generation, so a
    // late result can never edit a node the user has since changed.
    std::uint64_t pendingToken_{0};
    std::uint64_t generation_{0};
    nemo::NetworkId pendingNetwork_{};
    nemo::NodeId pendingNode_{};
};

}  // namespace nemo::ui
