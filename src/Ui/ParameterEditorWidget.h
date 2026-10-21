#pragma once

// The generic editor's widget (docs/generic-editor.md): one row per parameter
// of a plugin with no editor of its own, drawn from a ParameterEditor.

#include <cstddef>
#include <string>

#include "App/ParameterEditor.h"
#include "Ui/UiWidget.h"

namespace stagehand {

class ParameterEditorWidget final : public UiWidget {
public:
    ParameterEditorWidget(DGL_NAMESPACE::Window& window, ParameterEditor& editor, std::string plugin);

    // The height (logical) that shows every row, for sizing a new window.
    float wantedHeight() const;

protected:
    void draw() override;
    void wheelUnclaimed(float x, float y, float notches, std::uint32_t mod) override;

private:
    void drawRow(std::size_t index, float y);
    void openChoices(std::size_t index, float x, float y);
    void clampScroll();

    ParameterEditor& editor;
    std::string      pluginName;   // the header
    float            scrollY = 0;
    std::size_t      lastClicked   = SIZE_MAX;   // a double-click resets to the default
    std::uint32_t    lastClickTime = 0;
};

} // namespace stagehand
