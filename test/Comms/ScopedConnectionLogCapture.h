#pragma once

#include <optional>

#include <QtCore/QByteArray>
#include <QtCore/QLoggingCategory>
#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QSettings>
#include <QtCore/QStringList>
#include <QtCore/QVariant>

#include "RAIIFixtures.h"

/// Consumes the enabled category's debug output; all other messages still reach strict-log checking.
class ScopedConnectionLogCapture
{
public:
    ScopedConnectionLogCapture(const char* category, const char* prefix)
        : _category(category)
        , _prefix(QString::fromLatin1(prefix))
    {
        const QString categoryName = QString::fromLatin1(category);
        _settings.beginGroup(QStringLiteral("LoggingFilters"));
        _hadSetting = _settings.contains(categoryName);
        _previousSetting = _settings.value(categoryName);
        {
            const QMutexLocker locker(&_mutex);
            _active = this;
            _previous = qInstallMessageHandler(&_handle);
        }
        _loggingCategory.emplace(categoryName);
    }

    ~ScopedConnectionLogCapture()
    {
        _loggingCategory.reset();
        // The manager persists toggles; also preserve an absent key or an explicit false value.
        const QString categoryName = QString::fromLatin1(_category);
        if (_hadSetting) {
            _settings.setValue(categoryName, _previousSetting);
        } else {
            _settings.remove(categoryName);
        }
        _settings.sync();
        const QMutexLocker locker(&_mutex);
        (void) qInstallMessageHandler(_previous);
        _active = nullptr;
    }

    ScopedConnectionLogCapture(const ScopedConnectionLogCapture&) = delete;
    ScopedConnectionLogCapture& operator=(const ScopedConnectionLogCapture&) = delete;

    QStringList messages() const
    {
        const QMutexLocker locker(&_mutex);
        return _messages;
    }

private:
    static void _handle(QtMsgType type, const QMessageLogContext& context, const QString& message)
    {
        QtMessageHandler previous = nullptr;
        {
            const QMutexLocker locker(&_mutex);
            if (_active) {
                previous = _active->_previous;
                if (type == QtDebugMsg && QByteArray(context.category) == _active->_category) {
                    if (message.startsWith(_active->_prefix)) {
                        _active->_messages.append(message);
                    }
                    return;
                }
            }
        }
        if (previous) {
            previous(type, context, message);
        }
    }

    QByteArray _category;
    QString _prefix;
    QSettings _settings;
    QVariant _previousSetting;
    bool _hadSetting = false;
    std::optional<TestFixtures::LoggingCategoryFixture> _loggingCategory;
    QtMessageHandler _previous = nullptr;
    QStringList _messages;
    static inline QMutex _mutex;
    static inline ScopedConnectionLogCapture* _active = nullptr;
};
