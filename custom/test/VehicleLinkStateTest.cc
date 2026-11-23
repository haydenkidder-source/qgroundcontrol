#include "VehicleLinkStateTest.h"

#include <memory>

#include <QtTest/QSignalSpy>

#include "UDPLink.h"
#include "VehicleLinkState.h"

class VehicleLinkStateTestTransport : public LinkInterface
{
public:
    explicit VehicleLinkStateTestTransport(SharedLinkConfigurationPtr& config)
        : LinkInterface(config)
    {}

    bool isConnected() const override { return _connected; }

    void setConnected(bool connected)
    {
        _connected = connected;
        if (connected) {
            emit this->connected();
        } else {
            emit disconnected();
        }
    }

    void disconnect() override { setConnected(false); }

private:
    bool _connect() override { return true; }

    void _writeBytes(const QByteArray&) override {}

    bool _connected = false;
};

UT_REGISTER_TEST(VehicleLinkStateTest, TestLabel::Unit, TestLabel::Comms)

void VehicleLinkStateTest::_signalsAndLifetime()
{
    SharedLinkConfigurationPtr config = std::make_shared<UDPConfiguration>(QStringLiteral("Observer"));
    VehicleLinkState observer;
    QSignalSpy changed(&observer, &VehicleLinkState::stateChanged);
    QSignalSpy configurationChanged(&observer, &VehicleLinkState::configurationChanged);
    observer.setConfiguration(config.get());
    QCOMPARE(observer.state(), VehicleLinkState::State::Disconnected);
    auto link = std::make_shared<VehicleLinkStateTestTransport>(config);
    config->setLink(link);
    QCOMPARE(observer.state(), VehicleLinkState::State::Connecting);
    changed.clear();
    link->setConnected(true);
    QVERIFY(!changed.isEmpty());
    QCOMPARE(observer.state(), VehicleLinkState::State::Connected);
    changed.clear();
    link->setConnected(false);
    QVERIFY(!changed.isEmpty());
    QCOMPARE(observer.state(), VehicleLinkState::State::Connecting);
    changed.clear();
    link.reset();
    QVERIFY(!changed.isEmpty());
    QCOMPARE(observer.state(), VehicleLinkState::State::Disconnected);
    config->setAutoConnect(true);
    changed.clear();
    config->setAutoConnectStarted(true);
    QVERIFY(!changed.isEmpty());
    QVERIFY(config->linkActive());
    QCOMPARE(observer.state(), VehicleLinkState::State::Retrying);
    changed.clear();
    config->setSuppressAutoReconnect(true);
    QVERIFY(!changed.isEmpty());
    QCOMPARE(observer.state(), VehicleLinkState::State::Disconnected);
    changed.clear();
    config->setSuppressAutoReconnect(false);
    QVERIFY(!changed.isEmpty());
    QCOMPARE(observer.state(), VehicleLinkState::State::Retrying);
    changed.clear();
    link = std::make_shared<VehicleLinkStateTestTransport>(config);
    config->setLink(link);
    QVERIFY(!changed.isEmpty());
    QCOMPARE(observer.state(), VehicleLinkState::State::Connecting);
    changed.clear();
    link.reset();
    QVERIFY(!changed.isEmpty());
    QCOMPARE(observer.state(), VehicleLinkState::State::Retrying);
    changed.clear();
    configurationChanged.clear();
    config.reset();
    QVERIFY(!configurationChanged.isEmpty());
    QVERIFY(!changed.isEmpty());
    QCOMPARE(observer.state(), VehicleLinkState::State::Disconnected);
    QVERIFY(!observer.configuration());
}
