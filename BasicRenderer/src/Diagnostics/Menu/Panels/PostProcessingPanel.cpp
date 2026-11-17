#include <BasicRenderer/Scene/Scene.h>
#include "Diagnostics/Menu/Menu.h"
#include "PostProcessing/AmbientOcclusion/AmbientOcclusionSettings.h"

void Menu::DrawUpscalingCombo()
{
    int modeIdx = static_cast<int>(m_currentUpscalingMode);

    if (ImGui::Combo("Upscaling Mode", &modeIdx, UpscalingModeNames, UpscalingModeCount))
    {
        m_currentUpscalingMode = static_cast<UpscalingMode>(modeIdx);
		setUpscalingMode(m_currentUpscalingMode);
    }
}

void Menu::DrawUpscalingQualityCombo()
{
    int modeIdx = static_cast<int>(m_currentUpscalingQualityMode);

    if (ImGui::Combo("Upscaling Quality", &modeIdx, UpscaleQualityModeNames, UpscaleQualityModeCount))
    {
        m_currentUpscalingQualityMode = static_cast<UpscaleQualityMode>(modeIdx);
        setUpscalingQualityMode(m_currentUpscalingQualityMode);
    }
}

void Menu::DrawWindowResolutionCombo()
{
    int modeIdx = static_cast<int>(m_currentWindowResolutionPreset);

    if (ImGui::Combo("Window Resolution", &modeIdx, WindowResolutionPresetNames, WindowResolutionPresetCount))
    {
        m_currentWindowResolutionPreset = static_cast<WindowResolutionPreset>(modeIdx);
        setWindowResolutionPreset(m_currentWindowResolutionPreset);
    }
}

void Menu::DrawTonemapTypeDropdown() {
    static unsigned int selectedItemIndex = 0;
	selectedItemIndex = getTonemapType();
    if (ImGui::BeginCombo("Tonemap Type", TonemapTypeNames[selectedItemIndex].c_str())) {
        for (unsigned int i = 0; i < TonemapTypeNames.size(); ++i) {
            bool isSelected = (selectedItemIndex == i);
            if (ImGui::Selectable(TonemapTypeNames[i].c_str(), isSelected)) {
                selectedItemIndex = i;
                setTonemapType(i);
            }
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();

    }
}

// The screen-space AO technique and CORTAO's settings (docs/lighting.md "Ambient occlusion"). They are read straight from the
// settings each frame: the technique and the sample count rebuild the graph, the rest apply on the next frame.
void Menu::DrawAmbientOcclusionSettings()
{
    namespace ao = br::ambient_occlusion;
    auto& settings = SettingsManager::GetInstance();
    ImGui::Indent();
    int technique = ao::ReadAmbientOcclusionTechnique() == ao::AmbientOcclusionTechnique::XeGTAO ? 0 : 1;
    if (ImGui::Combo("AO Technique", &technique, "XeGTAO\0CORTAO\0")) {
        settings.getSettingSetter<uint32_t>(ao::TechniqueSettingName)(static_cast<uint32_t>(technique));
    }
    if (technique == 1) {
        const uint32_t sampleCount = ao::ClampCortaoSampleCount(settings.getSettingGetter<uint32_t>(ao::CortaoSampleCountSettingName)());
        int preset = 0;
        for (size_t i = 0; i < ao::CortaoSampleCounts.size(); ++i) {
            if (ao::CortaoSampleCounts[i] == sampleCount) preset = static_cast<int>(i);
        }
        if (ImGui::Combo("CORTAO Samples", &preset, "4 (minimal)\0" "6\0" "8\0" "12\0" "16\0" "24\0" "32 (ultra)\0")) {
            settings.getSettingSetter<uint32_t>(ao::CortaoSampleCountSettingName)(ao::CortaoSampleCounts[static_cast<size_t>(preset)]);
        }
        const auto slider = [&](const char* label, const char* name, float minValue, float maxValue, const char* format, const char* tooltip) {
            float value = settings.getSettingGetter<float>(name)();
            if (ImGui::SliderFloat(label, &value, minValue, maxValue, format)) {
                settings.getSettingSetter<float>(name)(value);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", tooltip);
            }
        };
        slider("CORTAO Radius", ao::CortaoRadiusSettingName, 0.05f, 10.0f, "%.2f", "The world-space radius occluders are found within.");
        slider("CORTAO Screen Radius", ao::CortaoScreenRadiusRatioSettingName, 0.05f, 0.5f, "%.2f",
            "The largest radius on screen, as a fraction of its height (0.1 to 0.25 recommended).");
        slider("CORTAO Mip Bias", ao::CortaoMipBiasSettingName, -8.0f, 0.0f, "%.1f",
            "Lower reads finer depth mips for distant samples: more accurate, slower.");
        slider("CORTAO Slope Compensation", ao::CortaoSlopeCompensationSettingName, 1.0f, 8.0f, "%.1f",
            "How far occluders seen at a grazing angle are enlarged to make up for their foreshortening.");
        slider("CORTAO Backface Threshold", ao::CortaoBackfaceThresholdSettingName, -1.0f, 1.0f, "%.2f",
            "Occluders facing away from the pixel past this are treated as back faces (no self-occlusion offset).");
        slider("CORTAO Backface Boost", ao::CortaoBackfaceBoostSettingName, 0.0f, 1.0f, "%.2f",
            "Enlarges back-face occluders by their slope compensation, against light leaking.");
        slider("CORTAO Filter Depth", ao::CortaoFilterDepthSensitivitySettingName, 0.0f, 2048.0f, "%.0f",
            "How strongly the spatial filter rejects neighbours at other depths.");
        slider("CORTAO Filter Normal", ao::CortaoFilterNormalSensitivitySettingName, 0.0f, 1024.0f, "%.0f",
            "How strongly the spatial filter rejects neighbours with other normals.");
    }
    ImGui::Unindent();
}
