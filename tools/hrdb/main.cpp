#include "ui/mainwindow.h"

#include <QApplication>
#include <QDateTime>
#include <QSettings>
#include "hrdbapplication.h"
#include <QCommandLineParser>

void logFormatter(QtMsgType type, const QMessageLogContext &context, const QString &msg);
QtMessageHandler originalMessageHandler = nullptr;
QString lastLogMessage;
QDateTime firstSequentialLogTs;
long messageRepeatCounter = 0;
const char* tsFormat = "hhmmsszzz";

int main(int argc, char *argv[])
{
    // These are used in settings
    QCoreApplication::setOrganizationName("hrdb");
    QCoreApplication::setApplicationName("hrdb");
    QCoreApplication::setApplicationVersion(VERSION_STRING);
    QSettings::setDefaultFormat(QSettings::Format::IniFormat);

    originalMessageHandler = qInstallMessageHandler(logFormatter);

    // This creates the app and the session (including loading session settings)
    HrdbApplication app(argc, argv);

    QCommandLineParser parser;
    parser.setApplicationDescription("hrdb -- a Hatari Remote DeBugger UI");
    parser.addHelpOption();
    parser.addVersionOption();

    // A boolean option (-q, --quicklaunch)
    QCommandLineOption quickLaunchOption(QStringList() << "q" << "quicklaunch",
                                         "Launch Hatari with previously-saved UI settings.");
    parser.addOption(quickLaunchOption);
    parser.process(app);


    // Build the UI
    MainWindow w(app.m_session);
    w.show();

    // Kick off hatari if requested
    if (parser.isSet(quickLaunchOption))
    {
       if (!LaunchHatari(app.m_session.GetLaunchSettings(), &app.m_session))
       {
            QTextStream(stderr) << QString("ERROR: quicklaunch: Unable to run hatari\n");
            return 1;
       }
    }

    return app.exec();
}

void logFormatter(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    // Check for repeated messages, if so bail
    if (!lastLogMessage.isNull() && msg == lastLogMessage)
    {
        if (messageRepeatCounter == 0) {
            firstSequentialLogTs = QDateTime::currentDateTime();
        }

        ++messageRepeatCounter;
        return;
    }

    const QDateTime nowTs = QDateTime::currentDateTime();

    // If we have repeats pending, flush a summary line *before* logging the new message.
    if (!lastLogMessage.isNull() && messageRepeatCounter > 0)
    {
        // Duration of the repeat burst in ms
        const qint64 durationMs = firstSequentialLogTs.msecsTo(nowTs);

        double callsPerSecond = 0.0;
        if (durationMs > 0) {
            // calls/sec based on repeat count only (does not include the first original log)
            callsPerSecond = (messageRepeatCounter * 1000.0) /
                             static_cast<double>(durationMs);
        }

        QString repeatSummary = QStringLiteral(
            "Previous message repeated %1 times between %2 and %3 (%4 calls/sec)")
            .arg(messageRepeatCounter)
            .arg(firstSequentialLogTs.toString(tsFormat))
            .arg(nowTs.toString(tsFormat))
            .arg(QString::number(callsPerSecond, 'f', 2));

        QString formattedSummary = qFormatLogMessage(QtInfoMsg, context, repeatSummary);

        if (originalMessageHandler) {
            originalMessageHandler(QtInfoMsg, context, formattedSummary);
        }

        messageRepeatCounter = 0;
        firstSequentialLogTs = QDateTime();
    }

    // Timestamp the message
    QString timeStampedMessage = QStringLiteral("%1: %2")
        .arg(nowTs.toString(tsFormat))
        .arg(msg);

    QString formatted = qFormatLogMessage(type, context, timeStampedMessage);

    // Spit it back out (we could chain these for file logging etc.)
    if (originalMessageHandler) {
        originalMessageHandler(type, context, formatted);
    }

    lastLogMessage = msg;
}

