#pragma once

#include <functional>

#include <QtCore/QPair>
#include <QtCore/QPointer>
#include <QtCore/QTemporaryDir>
#include <QtCore/QVariant>

#include "LinkConfiguration.h"
#include "LinkInterface.h"
#include "QmlUITestBase.h"
#ifndef QGC_NO_SERIAL_LINK
#include "SerialPortManager.h"
#endif

class COPController;
class Fact;
class SerialConnectionNotice;

class COPSerialNoticesUITest : public QmlUITestBase
{
    Q_OBJECT

private slots:
    void init() override;
    void cleanup() override;
    void _noticeRows_data();
    void _noticeRows();
    void _navigation_data();
    void _navigation();
    void _attentionAndHistory();
    void _discoveryLifecycle();
    void _automaticOpen_data();
    void _automaticOpen();
    void _serialVehicleLoss();
    void _stockFooterHook();
    void _dismissOneOfTwo();

private:
    COPController* _controller() const;
    QQuickItem* _footer() const;
    void _startFooter();
    bool _clickStable(const QString& name);
#ifndef QGC_NO_SERIAL_LINK
    QQuickItem* _liveRow(const QString& id);
    void _narrowFooter(const QString& id);
    SharedLinkConfigurationPtr _configuration(const QString& path = {});
    SerialConnectionNotice* _raise(const QString& trigger);
    QList<SerialPortManager::Port> _ports;
    QList<SerialPortManager::ReservationPtr> _claims;
    QList<SharedLinkInterfacePtr> _links;
    std::function<quint64()> _previousClock;
#endif
    QList<SharedLinkConfigurationPtr> _configs;
    QList<QPair<Fact*, QVariant>> _facts;
    QVariantMap _settings;
    QTemporaryDir _directory;
    quint64 _nowUs = 1;
};
