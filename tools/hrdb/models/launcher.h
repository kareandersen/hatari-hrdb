#ifndef LAUNCHER_H
#define LAUNCHER_H

#include <QString>
#include <QStringList>
#include "exceptionmask.h"

class QSettings;
class Session;

class LaunchSettings
{
public:
    void loadSettings(QSettings& settings);
    void saveSettings(QSettings& settings) const;

    // What sort of automatic breakpoint to use
    enum BreakMode
    {
        kNone,
        kBoot,
        kProgStart,
        kProgramBreakpoint,
        kBootsector,
    };

    int m_breakMode;                // one of BreakMode above
    QString m_hatariFilename;
    QString m_hatariConfigFilename;
    QString m_prgFilename;          // .prg or TOS file to launch
    QString m_workingDirectory;
    QString m_watcherFiles;
    QString m_argsTxt;
    QString m_breakPointTxt;
    bool m_watcherActive;
    bool m_fastLaunch;              // If true, start with --fast-forward and reset at program start
    ExceptionMask m_exceptionMask;
};

// Debugger commands that set up a launched program: fast-forward during boot,
// plus the breakpoints that stop at boot/program start and run the program
// start script. Also re-applied on reset, so that a reboot sets the program
// up in the same way as the original launch.
QStringList GenerateStartupCommands(const LaunchSettings& settings, const QString& progStartScriptFilename);

// Returns true on success (Qt doesn't offer more options?)
bool LaunchHatari(const LaunchSettings& settings, Session* pSession);

#endif // LAUNCHER_H
