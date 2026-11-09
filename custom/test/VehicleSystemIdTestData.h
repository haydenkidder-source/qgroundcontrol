#pragma once

#include <QtTest/QTest>

namespace VehicleSystemIdTestData {
inline void addValidRows()
{
    QTest::addColumn<quint32>("sysid");
    QTest::newRow("one") << quint32(1);
    QTest::newRow("legacy-max") << quint32(255);
    QTest::newRow("above-byte") << quint32(256);
    QTest::newRow("alias-257") << quint32(257);
    QTest::newRow("signed-max") << quint32(2147483647U);
    QTest::newRow("high-bit") << quint32(2147483648U);
    QTest::newRow("uint32-max") << quint32(4294967295U);
}
}  // namespace VehicleSystemIdTestData
