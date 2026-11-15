#include "QGCPlanStyle.h"

#include <limits>

QGCPlanStyle::QGCPlanStyle(QObject* parent)
    : QObject(parent)
{}

QColor QGCPlanStyle::vehicleColor(quint32 sysid) const
{
    Q_UNUSED(sysid);
    return {};
}

void QGCPlanStyle::_advanceRevision()
{
    _revision = _revision == std::numeric_limits<int>::max() ? 0 : _revision + 1;
    emit revisionChanged();
}
