#pragma once

#include "nemo/core/session/ProjectSession.hpp"

#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <map>
#include <optional>

namespace nemo::ui {

// GUI-thread presentation adapter. ProjectSession outlives this object and owns
// all authored values/history. Component records are immutable projections, not
// a second animation model. Selection and pointer previews belong to the panel.
class AnimationViewModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList targets READ targets WRITE setTargets NOTIFY targetsChanged)
    Q_PROPERTY(QVariantList channels READ channels NOTIFY channelsChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(bool available READ available NOTIFY channelsChanged)
public:
    explicit AnimationViewModel(ProjectSession& session, QObject* parent = nullptr);
    // Selectors are {network, node}, optionally narrowed by parameter/component.
    // They describe presentation membership, never authored animation state.
    [[nodiscard]] QVariantList targets() const { return targets_; }
    void setTargets(const QVariantList& targets);
    [[nodiscard]] QVariantList channels() const { return channels_; }
    [[nodiscard]] QString error() const { return error_; }
    [[nodiscard]] bool available() const { return available_; }
    Q_INVOKABLE bool beginGesture();
    Q_INVOKABLE void cancelGesture();
    Q_INVOKABLE bool moveKeys(const QStringList& ids, double deltaTime, double deltaValue);
    Q_INVOKABLE QString insertKey(const QString& channel, double time);
    Q_INVOKABLE bool editKey(const QString& id, double time, double value, double inSlope, double outSlope);
    Q_INVOKABLE bool setInterpolation(const QStringList& ids, const QString& mode);
    Q_INVOKABLE bool setTangentMode(const QStringList& ids, const QString& mode);
    Q_INVOKABLE bool setTangent(const QString& id, const QString& side, double slope);
    Q_INVOKABLE bool removeKeys(const QStringList& ids);
signals:
    void targetsChanged();
    void channelsChanged();
    void errorChanged();
    void projectChanged();

private:
    struct Component {
        AnimationChannelId channel{};
        std::size_t index{};
        ParameterType type{};
    };
    struct Target {
        Component component;
        KeyframeId key{};
    };
    enum class Operation { Move, Exact, Interpolation, TangentMode, InSlope, OutSlope };
    bool edit(const QStringList& ids, Operation operation, double time, double value, double inSlope = 0,
              double outSlope = 0, const QString& mode = {});
    [[nodiscard]] Target resolve(const QString& id) const;
    bool submit(Command command);
    bool checkRevision();
    bool fail(const QString& message);
    void refresh();
    static void changed(void* context) noexcept;
    ProjectSession& session_;
    QVariantList targets_;
    QString error_;
    QVariantList channels_;
    std::map<QString, Component> components_;
    bool available_{};
    std::optional<std::uint64_t> gestureRevision_;
    std::uint64_t generation_{};
    std::uint64_t observedGeneration_{};
    ProjectSession::Subscription subscription_;
};

}  // namespace nemo::ui
