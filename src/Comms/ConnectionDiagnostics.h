#pragma once

#include <QtCore/QString>

namespace ConnectionDiagnostics {
inline QString quoted(QString value)
{
    if (value.isEmpty()) {
        value = QStringLiteral("unknown");
    }
    value.replace(u'\\', QStringLiteral("\\\\"));
    value.replace(u'"', QStringLiteral("\\\""));
    value.replace(u'\n', QStringLiteral("\\n"));
    value.replace(u'\r', QStringLiteral("\\r"));
    value.replace(u'\t', QStringLiteral("\\t"));
    return u'"' + value + u'"';
}
}  // namespace ConnectionDiagnostics
