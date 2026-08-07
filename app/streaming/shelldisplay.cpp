#include "shelldisplay.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QVariant>

#define SHELL_DISPLAY_SERVICE "org.gnome.Shell"
#define SHELL_DISPLAY_PATH "/org/gnome/Shell/Extensions/MoonlightDisplay"
#define SHELL_DISPLAY_INTERFACE "org.gnome.Shell.Extensions.MoonlightDisplay"

// This runs on the SDL event loop thread, so a shell that stops answering must
// cost one hitch and not a stalled stream. Two calls at most per lookup.
#define SHELL_DISPLAY_TIMEOUT_MS 200

namespace ShellDisplay
{

// Returns false when the call itself failed, which means the extension is not
// there or the shell is not answering. A successful call reports what the shell
// knows in 'result'.
static bool callShell(const char* method, const QString& argument, WindowMonitor& result)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        result.unavailableReason = QStringLiteral("no session bus");
        return false;
    }

    // A plain method call, not QDBusInterface: introspecting the object first
    // would double the calls and the time budget.
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral(SHELL_DISPLAY_SERVICE),
                                                      QStringLiteral(SHELL_DISPLAY_PATH),
                                                      QStringLiteral(SHELL_DISPLAY_INTERFACE),
                                                      QString::fromUtf8(method));
    call << argument;

    QDBusMessage reply = bus.call(call, QDBus::Block, SHELL_DISPLAY_TIMEOUT_MS);
    if (reply.type() != QDBusMessage::ReplyMessage) {
        result.unavailableReason = QStringLiteral("%1 failed: %2").arg(QString::fromUtf8(method),
                                                                       reply.errorMessage());
        return false;
    }

    const QList<QVariant> args = reply.arguments();
    if (args.count() < 6) {
        result.unavailableReason = QStringLiteral("%1 returned %2 values")
                .arg(QString::fromUtf8(method)).arg(args.count());
        return false;
    }

    result.found = args.at(0).toBool();
    result.monitorIndex = args.at(1).toInt();
    result.rect.x = args.at(2).toInt();
    result.rect.y = args.at(3).toInt();
    result.rect.w = args.at(4).toInt();
    result.rect.h = args.at(5).toInt();

    // The workspace was added after the first release of the extension, so an
    // extension that stops at 6 values is answered with "no workspace" rather
    // than with an error. The caller already handles an unknown workspace,
    // because the shell also reports one for a window on every workspace.
    result.workspaceIndex = args.count() >= 7 ? args.at(6).toInt() : -1;

    if (result.found && (result.rect.w <= 0 || result.rect.h <= 0)) {
        result.found = false;
        result.unavailableReason = QStringLiteral("%1 returned an empty monitor rect")
                .arg(QString::fromUtf8(method));
    }

    return true;
}

WindowMonitor getWindowMonitor(const QString& windowTitle, const QString& appId)
{
    WindowMonitor result;

    // Ask by application id first. It identifies our windows and nothing else,
    // while a title is matched as a substring, so any window that happens to
    // mention ours answers instead: a terminal running a command that names the
    // stream window was observed doing exactly that. Both lookups prefer the
    // focused window, which during a stream is the stream window.
    if (!appId.isEmpty()) {
        if (!callShell("GetWindowMonitorByAppId", appId, result)) {
            return result;
        }
    }

    if (!result.found && !windowTitle.isEmpty()) {
        if (!callShell("GetWindowMonitor", windowTitle, result)) {
            return result;
        }
    }

    if (!result.found && result.unavailableReason.isEmpty()) {
        result.unavailableReason = QStringLiteral("the shell does not know the window");
    }

    return result;
}

}
