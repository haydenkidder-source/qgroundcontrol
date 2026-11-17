#pragma once

#include <QtCore/QString>

namespace GstTest {

/// Failure-only diagnostics: Windows native stacks, Linux task states/wait channels, unavailable elsewhere.
/// Does not log. Windows suspension/DbgHelp can deadlock; a 90-second watchdog terminates the process.
QString dumpAllThreadStacks();

}  // namespace GstTest
