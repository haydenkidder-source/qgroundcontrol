#pragma once

#include <QtCore/QObject>
#include <QtCore/QStringList>
#include <QtQmlIntegration/QtQmlIntegration>

/// A transport incident. Actions and reasons are stable, untranslated tokens.
class SerialConnectionNotice : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Managed by LinkManager")
    Q_PROPERTY(QString id READ id CONSTANT)
    Q_PROPERTY(QString source READ source CONSTANT)
    Q_PROPERTY(QString reason READ reason NOTIFY changed)
    Q_PROPERTY(QString text READ text NOTIFY changed)
    Q_PROPERTY(QStringList actions READ actions NOTIFY changed)
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool acknowledged READ acknowledged NOTIFY changed)
    Q_PROPERTY(quint64 generation READ generation CONSTANT)

    friend class LinkManager;

public:
    SerialConnectionNotice(const QString& id, const QString& source, quint64 generation, QObject* parent);

    QString id() const { return _noticeId; }

    QString source() const { return _source; }

    QString reason() const { return _reason; }

    QString text() const { return _text; }

    QStringList actions() const { return _actions; }

    bool active() const { return _active; }

    bool acknowledged() const { return _acknowledged; }

    quint64 generation() const { return _generation; }

signals:
    void changed();

private:
    const QString _noticeId;
    const QString _source;
    const quint64 _generation;
    QString _reason;
    QString _text;
    QStringList _actions;
    QString _transition;
    bool _active = false;
    bool _acknowledged = false;
};
