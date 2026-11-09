#pragma once

#include "UnitTest.h"

class CustomPaletteTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _paletteContract_data();
    void _paletteContract();
};
