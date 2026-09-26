#include <BasicRenderer/Scene/Scene.h>
#include "Diagnostics/Menu/Menu.h"

void Menu::DrawOutputTypeDropdown() {
	static unsigned int selectedItemIndex = 0;
    if (ImGui::BeginCombo("Output Type", OutputTypeNames[selectedItemIndex].c_str())) {
		for (unsigned int i = 0; i < OutputTypeNames.size(); ++i) {
			bool isSelected = (selectedItemIndex == i);
			if (ImGui::Selectable(OutputTypeNames[i].c_str(), isSelected)) {
				selectedItemIndex = i;
				setOutputType(i);
			}
			if (isSelected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();

    }
    if (selectedItemIndex == static_cast<unsigned int>(OutputType::VSM_PAGE_STATE)) {
        ImGui::TextDisabled(
            "VSM sample result: violet = finite depth/lit, green = finite "
            "depth/shadowed, yellow = preferred page dirty.");
        ImGui::TextDisabled(
            "This view does not indicate which static pages were rendered.");
    }
    else if (selectedItemIndex ==
        static_cast<unsigned int>(OutputType::VSM_RERENDERED_THIS_FRAME)) {
        ImGui::TextDisabled(
            "VSM static lifecycle: orange = static page rendered this frame, "
            "dark gray = cached, magenta = missing depth.");
    }
    else if (selectedItemIndex ==
        static_cast<unsigned int>(OutputType::VSM_TRACE_FOOTPRINT)) {
        ImGui::TextDisabled(
            "VSM trace footprint: red = continuous LOD phase, green = sampled "
            "texel / visual footprint excess, blue = camera / visual footprint.");
    }
}

