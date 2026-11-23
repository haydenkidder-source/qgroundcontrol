#pragma once

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtQmlIntegration/QtQmlIntegration>

#include "LinkConfiguration.h"
#include "LinkInterface.h"

class VehicleLinkState : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(LinkConfiguration* configuration READ configuration WRITE setConfiguration NOTIFY configurationChanged)
    Q_PROPERTY(State state READ state NOTIFY stateChanged)

public:
    enum class State
    {
        Disconnected,
        Retrying,
        Connecting,
        Connected
    };
    Q_ENUM(State)

    explicit VehicleLinkState(QObject* parent = nullptr);

    LinkConfiguration* configuration() const { return _configuration; }

    void setConfiguration(LinkConfiguration* configuration);
    State state() const;

signals:
    void configurationChanged();
    void stateChanged();

private:
    void _updateLink();
    QPointer<LinkConfiguration> _configuration;
    QPointer<LinkInterface> _link;
};
