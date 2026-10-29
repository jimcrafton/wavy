#include <gtest/gtest.h>

#include <cstdio>

#include "wavy/wavy.h"

TEST(Version, IsFourPartDottedNumber) {
    int parts[4] = {};
    ASSERT_EQ(std::sscanf(wavy::version(), "%d.%d.%d.%d", &parts[0], &parts[1], &parts[2], &parts[3]), 4);
}
