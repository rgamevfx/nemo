#include "NativeFileChooser.hpp"

#include <QMetaObject>
#include <QThread>
#include <QVariant>

#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <objbase.h>
#include <shobjidl.h>
#elif defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#endif

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
// org.freedesktop.portal.FileChooser filters: a(sa(us)), each entry a label
// plus (type, pattern) pairs where type 0 is a glob. A default-constructed
// QDBusArgument is a demarshaller and cannot be marshalled into, so the shapes
// are declared metatypes and registered with QtDBus instead. Q_DECLARE_METATYPE
// specializes a global template, so these live at file scope.
struct PortalFilterPattern {
    uint type{0};
    QString pattern;
};

struct PortalFilter {
    QString name;
    QList<PortalFilterPattern> patterns;
};

Q_DECLARE_METATYPE(PortalFilterPattern)
Q_DECLARE_METATYPE(PortalFilter)

QDBusArgument& operator<<(QDBusArgument& argument, const PortalFilterPattern& pattern) {
    argument.beginStructure();
    argument << pattern.type << pattern.pattern;
    argument.endStructure();
    return argument;
}

const QDBusArgument& operator>>(const QDBusArgument& argument, PortalFilterPattern& pattern) {
    argument.beginStructure();
    argument >> pattern.type >> pattern.pattern;
    argument.endStructure();
    return argument;
}

QDBusArgument& operator<<(QDBusArgument& argument, const PortalFilter& filter) {
    argument.beginStructure();
    argument << filter.name << filter.patterns;
    argument.endStructure();
    return argument;
}

const QDBusArgument& operator>>(const QDBusArgument& argument, PortalFilter& filter) {
    argument.beginStructure();
    argument >> filter.name >> filter.patterns;
    argument.endStructure();
    return argument;
}

static QVariant portalFilters(const std::vector<std::pair<QString, QStringList>>& entries) {
    static const bool registered = [] {
        qDBusRegisterMetaType<PortalFilterPattern>();
        qDBusRegisterMetaType<PortalFilter>();
        qDBusRegisterMetaType<QList<PortalFilter>>();
        return true;
    }();
    (void)registered;
    QList<PortalFilter> filters;
    for (const auto& entry : entries) {
        PortalFilter filter;
        filter.name = entry.first;
        for (const QString& pattern : entry.second) {
            filter.patterns.append(PortalFilterPattern{0U, pattern});
        }
        filters.append(std::move(filter));
    }
    return QVariant::fromValue(filters);
}
#endif

namespace nemo::ui {
namespace {

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
constexpr auto kPortalService = "org.freedesktop.portal.Desktop";
constexpr auto kPortalPath = "/org/freedesktop/portal/desktop";
constexpr auto kPortalInterface = "org.freedesktop.portal.FileChooser";
constexpr auto kPortalRequestInterface = "org.freedesktop.portal.Request";
#endif

#if defined(_WIN32)
struct NativeDialogResult {
    bool ok{false};
    bool cancelled{false};
    QStringList urls;
    QString error;
};

QString hresultText(HRESULT result) {
    return QStringLiteral("0x%1").arg(static_cast<qulonglong>(static_cast<unsigned long>(result)), 8, 16,
                                      QLatin1Char('0'));
}

// Worker-thread Win32 common item dialog (IFileOpenDialog/IFileSaveDialog).
// The thread that calls Show() owns the dialog's modal loop and message pump;
// the owner HWND comes from the app window on the GUI thread.
NativeDialogResult runWindowsDialog(bool save, bool multiple, const QString& title,
                                    const std::vector<NativeFileChooser::Filter>& filters,
                                    const std::filesystem::path& folder, const QString& suggestedName,
                                    const QString& defaultSuffix, HWND owner) {
    NativeDialogResult result;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninitialize = SUCCEEDED(initialized);

    IFileDialog* dialog = nullptr;
    HRESULT status = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&dialog));
    if (FAILED(status) || dialog == nullptr) {
        result.error = QStringLiteral("The Windows file dialog is unavailable (%1).").arg(hresultText(status));
        if (uninitialize) {
            CoUninitialize();
        }
        return result;
    }

    const bool multiSelect = multiple && !save;
    DWORD options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    options |= save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST;
    if (multiSelect) {
        options |= FOS_ALLOWMULTISELECT;
    }
    dialog->SetOptions(options);
    if (!title.isEmpty()) {
        dialog->SetTitle(reinterpret_cast<LPCWSTR>(title.utf16()));
    }

    // The suffix is a chooser convenience and never a format check.
    std::vector<COMDLG_FILTERSPEC> types;
    std::vector<std::wstring> typeNames;
    std::vector<std::wstring> typeSpecs;
    typeNames.reserve(filters.size());
    typeSpecs.reserve(filters.size());
    for (const NativeFileChooser::Filter& filter : filters) {
        typeNames.push_back(filter.label.toStdWString());
        typeSpecs.push_back(filter.patterns.join(QLatin1Char(';')).toStdWString());
    }
    types.reserve(typeNames.size());
    for (std::size_t index = 0; index < typeNames.size(); ++index) {
        types.push_back({typeNames[index].c_str(), typeSpecs[index].c_str()});
    }
    if (!types.empty()) {
        dialog->SetFileTypes(static_cast<UINT>(types.size()), types.data());
        dialog->SetFileTypeIndex(1);
    }

    if (!folder.empty()) {
        const std::wstring wide = folder.wstring();
        IShellItem* folderItem = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(wide.c_str(), nullptr, IID_PPV_ARGS(&folderItem))) &&
            folderItem != nullptr) {
            dialog->SetFolder(folderItem);
            folderItem->Release();
        }
    }
    if (save) {
        if (!defaultSuffix.isEmpty()) {
            dialog->SetDefaultExtension(reinterpret_cast<LPCWSTR>(defaultSuffix.utf16()));
        }
        if (!suggestedName.isEmpty()) {
            dialog->SetFileName(reinterpret_cast<LPCWSTR>(suggestedName.utf16()));
        }
    }

    status = dialog->Show(owner);
    if (status == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        result.cancelled = true;
    } else if (FAILED(status)) {
        result.error = QStringLiteral("The Windows file dialog failed (%1).").arg(hresultText(status));
    } else if (multiSelect) {
        IFileOpenDialog* open = nullptr;
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(&open))) && open != nullptr &&
            SUCCEEDED(open->GetResults(&items)) && items != nullptr) {
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD index = 0; index < count; ++index) {
                IShellItem* item = nullptr;
                if (FAILED(items->GetItemAt(index, &item)) || item == nullptr) {
                    continue;
                }
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
                    const QString text = QString::fromWCharArray(path);
                    if (!text.isEmpty()) {
                        result.urls.push_back(text);
                    }
                    CoTaskMemFree(path);
                }
                item->Release();
            }
            items->Release();
        }
        if (open != nullptr) {
            open->Release();
        }
        result.ok = !result.urls.isEmpty();
        if (!result.ok) {
            result.error = QStringLiteral("The Windows file dialog returned no file path.");
        }
    } else {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
                const QString text = QString::fromWCharArray(path);
                if (!text.isEmpty()) {
                    result.urls.push_back(text);
                    result.ok = true;
                }
                CoTaskMemFree(path);
            }
            item->Release();
        }
        if (!result.ok) {
            result.error = QStringLiteral("The Windows file dialog returned no file path.");
        }
    }
    dialog->Release();
    if (uninitialize) {
        CoUninitialize();
    }
    return result;
}
#endif

}  // namespace

NativeFileChooser::NativeFileChooser(QObject* parent) : QObject(parent) {
#if defined(_WIN32)
    // The Win32 common dialog blocks the calling thread inside Show(), so it
    // runs on a private thread whose message pump is the dialog's modal loop.
    blockingThread_ = new QThread(this);
    blockingWorker_ = new QObject();
    blockingWorker_->moveToThread(blockingThread_);
    blockingThread_->start();
#endif
}

NativeFileChooser::~NativeFileChooser() {
#if defined(_WIN32)
    if (blockingThread_ != nullptr) {
        blockingThread_->quit();
        blockingThread_->wait();
    }
    // Now idle and thread-less: safe to destroy from the owning thread.
    delete blockingWorker_;
#endif
}

bool NativeFileChooser::openFiles(QObject* requester, OutcomeHandler onOutcome, const QString& title, bool multiple,
                                  const std::vector<Filter>& filters, const std::filesystem::path& startFolder,
                                  const QString& suggestedName) {
    Request request;
    request.title = title;
    request.acceptLabel = QStringLiteral("Open");
    request.suggestedName = suggestedName;
    request.startFolder = startFolder;
    request.filters = filters;
    request.multiple = multiple;
    return begin(requester, std::move(onOutcome), std::move(request));
}

bool NativeFileChooser::saveFile(QObject* requester, OutcomeHandler onOutcome, const QString& title,
                                 const std::vector<Filter>& filters, const std::filesystem::path& startFolder,
                                 const QString& suggestedName, const QString& defaultSuffix) {
    Request request;
    request.title = title;
    request.acceptLabel = QStringLiteral("Save");
    request.suggestedName = suggestedName;
    request.defaultSuffix = defaultSuffix;
    request.startFolder = startFolder;
    request.filters = filters;
    request.save = true;
    return begin(requester, std::move(onOutcome), std::move(request));
}

bool NativeFileChooser::begin(QObject* requester, OutcomeHandler onOutcome, Request request) {
    // A request can only belong to a client that exists to receive it: without
    // a requester the outcome would have no owner, so nothing is started.
    if (inFlight_ || requester == nullptr || !onOutcome) {
        return false;
    }
    inFlight_ = true;
    const std::uint64_t serial = ++requestSerial_;
    request_ = std::move(request);
    requester_ = requester;
    onOutcome_ = std::move(onOutcome);
    requesterDestroyed_ = connect(requester, &QObject::destroyed, this, [this] {
        // The initiator is gone: release its handler so no callback outlives
        // its client. The platform request itself keeps running; its outcome
        // arrives for a requester that is no longer there and is dropped.
        requester_ = nullptr;
        onOutcome_ = nullptr;
    });
#if defined(_WIN32)
    startWindowsDialog(request_, serial);
#elif defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    startPortalDialog(request_, serial);
#else
    finish(serial, Outcome{Outcome::Status::Failed,
                           {},
                           QStringLiteral("No native file chooser is implemented for this platform.")});
#endif
    return true;
}

void NativeFileChooser::finish(std::uint64_t serial, Outcome outcome) {
    // A reply from a request that has already ended belongs to no client and
    // must not end the request that is outstanding now.
    if (!inFlight_ || serial != requestSerial_) {
        return;
    }
    // Release this request's state before handing the outcome over: the handler
    // is free to start the next request, which must not see anything of this
    // one, and nothing here may touch the chooser afterwards.
    inFlight_ = false;
    request_ = Request{};
    const QPointer<QObject> requester = std::exchange(requester_, nullptr);
    OutcomeHandler handler = std::exchange(onOutcome_, nullptr);
    if (requesterDestroyed_) {
        QObject::disconnect(requesterDestroyed_);
        requesterDestroyed_ = QMetaObject::Connection{};
    }
    // Every accepted request ends in exactly one outcome, and Chosen always
    // carries at least one file.
    if (outcome.status == Outcome::Status::Chosen && outcome.urls.isEmpty()) {
        outcome.status = Outcome::Status::Cancelled;
    }
    if (requester.isNull() || !handler) {
        return;  // The initiator is gone: nothing to act on.
    }
    handler(std::move(outcome));
}

#if defined(_WIN32)
void NativeFileChooser::startWindowsDialog(const Request& request, std::uint64_t serial) {
    // The owner HWND belongs to the GUI thread; the dialog itself runs on the
    // private worker thread so the Qt event loop stays free.
    const HWND owner = GetActiveWindow();
    const bool save = request.save;
    const bool multiple = request.multiple;
    const QString title = request.title;
    const std::vector<Filter> filters = request.filters;
    const std::filesystem::path folder = request.startFolder;
    const QString suggestedName = request.suggestedName;
    const QString defaultSuffix = request.defaultSuffix;
    QMetaObject::invokeMethod(
        blockingWorker_,
        [this, owner, save, multiple, title, filters, folder, suggestedName, defaultSuffix, serial] {
            const NativeDialogResult result =
                runWindowsDialog(save, multiple, title, filters, folder, suggestedName, defaultSuffix, owner);
            QMetaObject::invokeMethod(
                this,
                [this, result, serial] {
                    if (result.ok) {
                        QList<QUrl> urls;
                        urls.reserve(result.urls.size());
                        for (const QString& url : result.urls) {
                            urls.push_back(QUrl::fromLocalFile(url));
                        }
                        finish(serial, Outcome{Outcome::Status::Chosen, std::move(urls), {}});
                    } else if (result.cancelled) {
                        finish(serial, Outcome{Outcome::Status::Cancelled, {}, {}});
                    } else {
                        finish(serial, Outcome{Outcome::Status::Failed, {}, result.error});
                    }
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
#elif defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
void NativeFileChooser::startPortalDialog(const Request& request, std::uint64_t serial) {
    portalSerial_ = serial;
    const QString token = QStringLiteral("nemo_chooser_%1").arg(++portalTokenCounter_);
    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), token);
    options.insert(QStringLiteral("multiple"), request.multiple);
    options.insert(QStringLiteral("accept_label"), request.acceptLabel);
    if (!request.startFolder.empty() && request.startFolder != std::filesystem::path(".")) {
        QByteArray bytes = QByteArray::fromStdString(request.startFolder.string());
        bytes.append('\0');
        options.insert(QStringLiteral("current_folder"), bytes);
    }
    if (!request.suggestedName.isEmpty()) {
        options.insert(QStringLiteral("current_name"), request.suggestedName);
    }
    if (!request.filters.empty()) {
        std::vector<std::pair<QString, QStringList>> entries;
        entries.reserve(request.filters.size());
        for (const Filter& filter : request.filters) {
            entries.emplace_back(filter.label, filter.patterns);
        }
        options.insert(QStringLiteral("filters"), ::portalFilters(entries));
    }

    const QString method = request.save ? QStringLiteral("SaveFile") : QStringLiteral("OpenFile");
    QDBusMessage message =
        QDBusMessage::createMethodCall(QString::fromLatin1(kPortalService), QString::fromLatin1(kPortalPath),
                                       QString::fromLatin1(kPortalInterface), method);
    message << QString() << request.title << options;

    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    // `save` and the request identity are captured by value: the reply may not
    // arrive until after this request has ended, and the diagnostic names the
    // dialog it was for.
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, save = request.save, serial](QDBusPendingCallWatcher* call) {
                const QDBusMessage reply = call->reply();
                call->deleteLater();
                if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
                    const QString reason = save ? QStringLiteral("The system save dialog is unavailable: %1")
                                                : QStringLiteral("The system file dialog is unavailable: %1");
                    portalSerial_ = 0;
                    finish(serial, Outcome{Outcome::Status::Failed, {}, reason.arg(reply.errorMessage())});
                    return;
                }
                const QString handle = reply.arguments().constFirst().value<QDBusObjectPath>().path();
                if (handle.isEmpty()) {
                    portalSerial_ = 0;
                    finish(serial, Outcome{Outcome::Status::Failed,
                                           {},
                                           QStringLiteral("The system file dialog returned no request handle.")});
                    return;
                }
                portalHandle_ = handle;
                const bool connected = QDBusConnection::sessionBus().connect(
                    QString(), handle, QString::fromLatin1(kPortalRequestInterface), QStringLiteral("Response"), this,
                    SLOT(onPortalResponse(uint, QVariantMap)));
                if (!connected) {
                    portalHandle_.clear();
                    portalSerial_ = 0;
                    finish(serial, Outcome{Outcome::Status::Failed,
                                           {},
                                           QStringLiteral("Cannot observe the system file dialog response.")});
                }
            });
}
#endif

// Defined for every platform because MOC dispatches the slot unconditionally;
// only the portal path ever invokes it.
void NativeFileChooser::onPortalResponse(uint response, const QVariantMap& results) {
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    const QString handle = portalHandle_;
    portalHandle_.clear();
    if (!handle.isEmpty()) {
        QDBusConnection::sessionBus().disconnect(QString(), handle, QString::fromLatin1(kPortalRequestInterface),
                                                 QStringLiteral("Response"), this,
                                                 SLOT(onPortalResponse(uint, QVariantMap)));
    }
    // This response belongs to the request that opened the portal session; the
    // identity is consumed with it so a late response cannot reach a later
    // request.
    const std::uint64_t serial = std::exchange(portalSerial_, std::uint64_t{0});
    // org.freedesktop.portal.Request.Response: 0 success, 1 cancelled by the
    // user, 2+ the portal or the application side failed.
    if (response == 1) {
        finish(serial, Outcome{Outcome::Status::Cancelled, {}, {}});
        return;
    }
    if (response != 0) {
        finish(serial, Outcome{Outcome::Status::Failed,
                               {},
                               QStringLiteral("The system file dialog failed (portal response %1).").arg(response)});
        return;
    }
    QList<QUrl> urls;
    const QStringList uris = results.value(QStringLiteral("uris")).toStringList();
    urls.reserve(uris.size());
    for (const QString& uri : uris) {
        urls.push_back(QUrl(uri));
    }
    finish(serial, Outcome{Outcome::Status::Chosen, std::move(urls), {}});
#else
    Q_UNUSED(response);
    Q_UNUSED(results);
#endif
}

}  // namespace nemo::ui
