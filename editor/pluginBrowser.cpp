#include "pluginBrowser.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace wavy::editor {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string vendorOf(const BrowserItem& item) { return item.vendor.empty() ? "Unknown" : item.vendor; }

}  // namespace

void PluginBrowser::setItems(std::vector<BrowserItem> items) {
    std::sort(items.begin(), items.end(), [](const BrowserItem& a, const BrowserItem& b) {
        const std::string x = lower(a.name), y = lower(b.name);
        return x != y ? x < y : a.path < b.path;
    });
    items_ = std::move(items);

    const bool had = selected_ < categories_.size();
    const Group keepGroup = had ? categories_[selected_].group : Group::instruments;
    const std::string keepVendor = had ? lower(categories_[selected_].vendor) : std::string();
    size_t instruments = 0;
    std::map<std::string, std::pair<std::string, size_t>> vendors;  // lower-cased name -> (display name, count)
    for (const BrowserItem& item : items_) {
        if (item.instrument) {
            ++instruments;
        }
        auto& v = vendors[lower(vendorOf(item))];
        v.first = vendorOf(item);
        ++v.second;
    }
    categories_.clear();
    categories_.push_back({"Instruments", Group::instruments, {}, instruments});
    categories_.push_back({"Audio Effects", Group::effects, {}, items_.size() - instruments});
    std::vector<Category> byVendor;
    for (const auto& [key, v] : vendors) {
        byVendor.push_back({v.first, Group::vendor, v.first, v.second});
    }
    std::stable_partition(byVendor.begin(), byVendor.end(), [](const Category& c) { return c.vendor != "Unknown"; });
    categories_.insert(categories_.end(), byVendor.begin(), byVendor.end());

    selected_ = 0;
    for (size_t i = 0; i < categories_.size(); ++i) {
        if (categories_[i].group == keepGroup && lower(categories_[i].vendor) == keepVendor) {
            selected_ = i;
        }
    }
    refresh();
}

void PluginBrowser::selectCategory(size_t index) {
    if (index < categories_.size()) {
        selected_ = index;
        refresh();
    }
}

void PluginBrowser::setSearch(const std::string& text) {
    search_ = text;
    refresh();
}

void PluginBrowser::refresh() {
    visible_.clear();
    const std::string needle = lower(search_);
    for (const BrowserItem& item : items_) {
        bool show;
        if (!needle.empty()) {
            show = lower(item.name).find(needle) != std::string::npos || lower(item.vendor).find(needle) != std::string::npos ||
                   lower(item.subCategories).find(needle) != std::string::npos;
        } else if (selected_ >= categories_.size()) {
            show = false;
        } else {
            const Category& c = categories_[selected_];
            show = c.group == Group::instruments ? item.instrument : c.group == Group::effects ? !item.instrument : lower(vendorOf(item)) == lower(c.vendor);
        }
        if (show) {
            visible_.push_back(item);
        }
    }
}

}  // namespace wavy::editor
