#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace wavy::editor {

struct BrowserItem {
    std::string name;
    std::string vendor;
    std::string subCategories;
    std::string path;
    size_t classIndex = 0;
    bool instrument = false;
};

// What the plugin browser shows: the scanned plugins grouped by type (Instruments, Audio Effects) and by manufacturer, narrowed by a
// search box. Pure data, no UI.
class PluginBrowser {
public:
    enum class Group { instruments, effects, vendor };
    struct Category {
        std::string label;
        Group group = Group::instruments;
        std::string vendor;  // for Group::vendor
        size_t count = 0;
    };

    void setItems(std::vector<BrowserItem> items);
    size_t itemCount() const { return items_.size(); }

    // Instruments, Audio Effects, then one entry per manufacturer (case-insensitive order, "Unknown" last).
    const std::vector<Category>& categories() const { return categories_; }
    size_t selectedCategory() const { return selected_; }
    void selectCategory(size_t index);

    // A non-empty search looks through every plugin by name, manufacturer or kind, whatever the category; empty shows the category.
    void setSearch(const std::string& text);
    const std::string& search() const { return search_; }

    // The plugins to list, sorted by name.
    const std::vector<BrowserItem>& visible() const { return visible_; }

private:
    void refresh();

    std::vector<BrowserItem> items_;
    std::vector<Category> categories_;
    std::vector<BrowserItem> visible_;
    size_t selected_ = 0;
    std::string search_;
};

}  // namespace wavy::editor
