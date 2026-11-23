#include "VehicleLinkState.h"

VehicleLinkState::VehicleLinkState(QObject* parent)
    : QObject(parent)
{}

void VehicleLinkState::setConfiguration(LinkConfiguration* configuration)
{
    if (_configuration == configuration) {
        return;
    }
    if (_configuration) {
        disconnect(_configuration, nullptr, this, nullptr);
    }
    _configuration = configuration;
    if (_configuration) {
        connect(_configuration, &LinkConfiguration::linkActiveChanged, this, &VehicleLinkState::_updateLink);
        connect(_configuration, &QObject::destroyed, this, [this] {
            _updateLink();
            emit configurationChanged();
        });
    }
    _updateLink();
    emit configurationChanged();
}

VehicleLinkState::State VehicleLinkState::state() const
{
    if (!_link) {
        return _configuration && _configuration->linkActive() ? State::Retrying : State::Disconnected;
    }
    return _link->isConnected() ? State::Connected : State::Connecting;
}

void VehicleLinkState::_updateLink()
{
    LinkInterface* const link = _configuration ? _configuration->link() : nullptr;
    if (_link != link) {
        if (_link) {
            disconnect(_link, nullptr, this, nullptr);
        }
        _link = link;
        if (_link) {
            connect(_link, &LinkInterface::connected, this, &VehicleLinkState::stateChanged);
            connect(_link, &LinkInterface::disconnected, this, &VehicleLinkState::stateChanged);
            connect(_link, &QObject::destroyed, this, &VehicleLinkState::_updateLink);
        }
    }
    emit stateChanged();
}
