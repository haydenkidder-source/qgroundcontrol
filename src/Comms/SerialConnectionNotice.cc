#include "SerialConnectionNotice.h"

SerialConnectionNotice::SerialConnectionNotice(const QString& id, const QString& source, quint64 generation,
                                               QObject* parent)
    : QObject(parent)
    , _noticeId(id)
    , _source(source)
    , _generation(generation)
{}
