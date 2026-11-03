#include <gtest/gtest.h>

#include "pluginBrowser.h"

namespace {

using namespace wavy::editor;

BrowserItem item(const char* name, const char* vendor, bool instrument) {
    BrowserItem i;
    i.name = name;
    i.vendor = vendor;
    i.instrument = instrument;
    i.subCategories = instrument ? "Instrument|Synth" : "Fx|Reverb";
    return i;
}

PluginBrowser sample() {
    PluginBrowser b;
    b.setItems({item("Surge XT Effects", "Surge Synth Team", false), item("Surge XT", "Surge Synth Team", true),
                item("Valhalla Plate", "valhalla dsp", false), item("Valhalla Room", "Valhalla DSP", false), item("Mystery", "", true)});
    return b;
}

std::vector<std::string> names(const PluginBrowser& b) {
    std::vector<std::string> out;
    for (const BrowserItem& i : b.visible()) {
        out.push_back(i.name);
    }
    return out;
}

TEST(PluginBrowser, ListsTheTypesFirstThenTheManufacturers) {
    const PluginBrowser b = sample();
    const auto& c = b.categories();
    ASSERT_EQ(c.size(), 5u);
    EXPECT_EQ(c[0].label, "Instruments");
    EXPECT_EQ(c[0].count, 2u);
    EXPECT_EQ(c[1].label, "Audio Effects");
    EXPECT_EQ(c[1].count, 3u);
    EXPECT_EQ(c[2].label, "Surge Synth Team");
    EXPECT_EQ(c[2].count, 2u);
    EXPECT_EQ(c[3].count, 2u);  // "valhalla dsp" and "Valhalla DSP" are one maker
    EXPECT_EQ(c[4].label, "Unknown");
}

TEST(PluginBrowser, ShowsTheSelectedCategorySortedByName) {
    PluginBrowser b = sample();
    EXPECT_EQ(names(b), (std::vector<std::string>{"Mystery", "Surge XT"}));
    b.selectCategory(1);
    EXPECT_EQ(names(b), (std::vector<std::string>{"Surge XT Effects", "Valhalla Plate", "Valhalla Room"}));
    b.selectCategory(3);
    EXPECT_EQ(names(b), (std::vector<std::string>{"Valhalla Plate", "Valhalla Room"}));
    b.selectCategory(4);
    EXPECT_EQ(names(b), (std::vector<std::string>{"Mystery"}));
}

TEST(PluginBrowser, ASearchLooksThroughEveryCategory) {
    PluginBrowser b = sample();
    b.setSearch("surge");
    EXPECT_EQ(names(b), (std::vector<std::string>{"Surge XT", "Surge XT Effects"}));
    b.setSearch("REVERB");  // the plugin kind counts too
    EXPECT_EQ(names(b), (std::vector<std::string>{"Surge XT Effects", "Valhalla Plate", "Valhalla Room"}));
    b.setSearch("zzz");
    EXPECT_TRUE(b.visible().empty());
    b.setSearch("");
    EXPECT_EQ(names(b), (std::vector<std::string>{"Mystery", "Surge XT"}));
}

TEST(PluginBrowser, KeepsTheSelectedCategoryWhenTheListIsRefreshed) {
    PluginBrowser b = sample();
    b.selectCategory(3);
    b.setItems({item("Valhalla Room", "Valhalla DSP", false), item("Other", "Maker", true)});
    ASSERT_LT(b.selectedCategory(), b.categories().size());
    EXPECT_EQ(b.categories()[b.selectedCategory()].label, "Valhalla DSP");  // the spelling of the plugin now first in name order
    EXPECT_EQ(names(b), (std::vector<std::string>{"Valhalla Room"}));
}

TEST(PluginBrowser, AnEmptyCatalogShowsNothing) {
    PluginBrowser b;
    b.setItems({});
    EXPECT_EQ(b.itemCount(), 0u);
    EXPECT_TRUE(b.visible().empty());
    b.selectCategory(7);  // out of range is ignored
}

}  // namespace
