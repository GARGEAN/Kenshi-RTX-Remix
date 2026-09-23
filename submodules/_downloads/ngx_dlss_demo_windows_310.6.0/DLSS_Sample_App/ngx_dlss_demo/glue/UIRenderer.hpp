#include <donut/app/imgui_renderer.h>

class UIRenderer : public ImGui_Renderer
{
private:
    std::shared_ptr<FeatureDemo> m_app;
    UIData& m_ui;
    nvrhi::CommandListHandle m_CommandList;
    std::deque<float> frameTimeList;

public:
    UIRenderer(DeviceManager* deviceManager, std::shared_ptr<FeatureDemo> app, UIData& ui)
        : ImGui_Renderer(deviceManager)
        , m_app(app)
        , m_ui(ui)
    {
        m_CommandList = GetDevice()->createCommandList();
    }

protected:
    virtual void buildUI(void) override
    {
        if (!m_ui.ShowUI)
            return;

        const auto& io = ImGui::GetIO();


        if (m_app->IsSceneLoading())
        {
            BeginFullScreenWindow();

            char messageBuffer[256];
            const auto& stats = Scene::GetLoadingStats();
            snprintf(messageBuffer, std::size(messageBuffer), "Loading scene %s, please wait...\nObjects: %d/%d, Textures: %d/%d",
                m_app->GetCurrentSceneName().c_str(), stats.ObjectsLoaded.load(), stats.ObjectsTotal.load(), m_app->GetTextureCache()->GetNumberOfLoadedTextures(), m_app->GetTextureCache()->GetNumberOfRequestedTextures());

            DrawScreenCenteredText(messageBuffer);

            EndFullScreenWindow();

            return;
        }


        ImGui::Begin("Settings", 0, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("Renderer: %s", GetDeviceManager()->GetRendererString());
        double avgFrameTime = GetDeviceManager()->GetAverageFrameTimeSeconds();
        if (avgFrameTime > 0.0)
        {
            if (frameTimeList.size() > 50)
            {
                frameTimeList.pop_front();
            }
            frameTimeList.push_back((float)avgFrameTime * 1e3f);
            std::vector<float> sortedList(frameTimeList.begin(), frameTimeList.end());
            std::sort(sortedList.begin(), sortedList.end());
            ImGui::Text("Mdn %.3f ms/frm Avg %.3f ms/frm (%.1f FPS)",
                        sortedList[sortedList.size() / 2],
                        avgFrameTime * 1e3,
                        1.0 / avgFrameTime );
        }

        ImGui::Separator();
        ImGui::Text("AA Mode");
        ImGui::Separator();
        ImGui::Combo("AA Mode", (int*)&m_ui.AntiAliasingMode, "None\0TemporalAA\0DLSS\0");
        if (m_ui.AntiAliasingMode == UIData::AntiAliasingMode::DLSS)
        {
            ImGui::Checkbox("Pre Tonemapping", &m_ui.PreTonemapping);

            if (ImGui::BeginCombo("PerfQuality", m_ui.PERF_QUALITY_LIST[m_ui.PerfModeListIdx].PerfQualityText)) // The second parameter is the label previewed before opening the combo.
            {
                for (int i = 0; i < m_ui.PERF_QUALITY_LIST.size(); ++i)
                {
                    if (m_ui.PERF_QUALITY_LIST[i].PerfQualityAllowed)
                    {
                        bool is_selected = m_ui.PerfModeListIdx == i;
                        if (ImGui::Selectable(m_ui.PERF_QUALITY_LIST[i].PerfQualityText, false))
                        {
                            m_ui.PerfModeListIdx = i;
                        }
                        if (is_selected)
                        {
                            ImGui::SetItemDefaultFocus();   // You may set the initial focus when opening the combo (scrolling + for keyboard navigation support)
                        }
                    }
                    else
                    {
                        ImGui::TextDisabled(m_ui.PERF_QUALITY_LIST[i].PerfQualityText);
                    }
                }
                ImGui::EndCombo();
            }

            // If the Option was changed to a mode that didn't have VARIABLE_RATIO support
            // fall back to OPTIMAL_RATIO instead
            if (!m_ui.PERF_QUALITY_LIST[m_ui.PerfModeListIdx].PerfQualityDynamicAllowed &&
                m_ui.ScaleRatio == VARIABLE_RATIO)
            {
                m_ui.ScaleRatio = OPTIMAL_RATIO;
            }

            std::stringstream ratioString;
            ratioString << std::fixed << std::setprecision(2) << m_ui.ScaleRatio;

            if (ImGui::BeginCombo("ScaleRatio", m_ui.ScaleRatio == OPTIMAL_RATIO ? "Optimal" : m_ui.ScaleRatio == VARIABLE_RATIO ? "Variable" : ratioString.str().c_str())) // The second parameter is the label previewed before opening the combo.
            {
                for (int n = 0; n < IM_ARRAYSIZE(UIData::ScaleRatios); n++)
                {
                    if (UIData::ScaleRatios[n] == VARIABLE_RATIO &&
                        !m_ui.PERF_QUALITY_LIST[m_ui.PerfModeListIdx].PerfQualityDynamicAllowed)
                    {
                        ImGui::TextDisabled("Variable");
                    }
                    else
                    {
                        bool is_selected = (m_ui.ScaleRatio == UIData::ScaleRatios[n]); // You can store your selection however you want, outside or inside your objects
                        ratioString.str(""); ratioString.clear();
                        ratioString << std::fixed << std::setprecision(2) << UIData::ScaleRatios[n];
                        if (ImGui::Selectable(UIData::ScaleRatios[n] == OPTIMAL_RATIO ? "Optimal" : UIData::ScaleRatios[n] == VARIABLE_RATIO ? "Variable" : ratioString.str().c_str(), is_selected))
                        {
                            m_ui.ScaleRatio = UIData::ScaleRatios[n];
                        }
                        if (is_selected)
                        {
                            ImGui::SetItemDefaultFocus();   // You may set the initial focus when opening the combo (scrolling + for keyboard navigation support)
                        }
                    }
                }
                ImGui::EndCombo();
            }

            const char* uiLablePreview = nullptr;
            if (m_ui.RENDER_PRESET_MAP.find(m_ui.RenderPresetSelected) != m_ui.RENDER_PRESET_MAP.end())
            {
                uiLablePreview = m_ui.RENDER_PRESET_MAP[m_ui.RenderPresetSelected];
            }
            else
            {
                uiLablePreview = m_ui.RENDER_PRESET_MAP.at(NVSDK_NGX_DLSS_Hint_Render_Preset_Default);
                donut::log::info("Warning: There is a mismatch in presets supported by the sample app and presets supported by the snippet");
            }

            if (ImGui::BeginCombo("DLSS Render Presets", uiLablePreview)) // The second parameter is the label previewed before opening the combo.
            {
                for (const auto& [PresetEnum, PresetName]: m_ui.RENDER_PRESET_MAP)
                {
                    bool is_selected = m_ui.RenderPresetSelected == PresetEnum;
                    if (ImGui::Selectable(m_ui.RENDER_PRESET_MAP[PresetEnum], false))
                    {
                        m_ui.RenderPresetSelected = PresetEnum;
                        m_ui.RenderPresetSelectionHasChanged = true;
                    }
                    if (is_selected)
                    {
                        ImGui::SetItemDefaultFocus();   // You may set the initial focus when opening the combo (scrolling + for keyboard navigation support)
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::Checkbox("Override LODBias", &m_ui.OverrideLodBias);

            if (m_ui.OverrideLodBias)
            {
                ImGui::DragFloat("LODBias (-4.0 ~ 0.0)", &m_ui.ForcedLodBias, 0.01f, -4.0f, 0.0f);
            }
            else
            {
                ImGui::Text("Default LODBias : %.2f", m_ui.DefaultLodBias);
            }         

            ImGui::Checkbox("Enable Auto Exposure", &m_ui.EnableAutoExposure);

            ImGui::Checkbox("Force Scene Reset", &m_ui.ForceReset);           
        }       

        ImGui::Text("----");
        ImGui::Checkbox("VSync", &m_ui.EnableVsync);
        ImGui::Separator();
        ImGui::Text("");

        if (ImGui::CollapsingHeader("Scene Tweaks"))
        {
            ImGui::Separator();
            ImGui::Text("Non-Specific");
            ImGui::Separator();

            const std::string currentScene = m_app->GetCurrentSceneName();
            if (ImGui::BeginCombo("Scene", currentScene.c_str()))
            {
                const std::vector<std::string>& scenes = m_app->GetMediaFolder().GetAvailableScenes();
                for (const std::string& scene : scenes)
                {
                    bool is_selected = scene == currentScene;
                    if (ImGui::Selectable(scene.c_str(), is_selected))
                        m_app->SetCurrentSceneName(scene);
                    if (is_selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::DragFloat("Ambient Intensity (0.0 - 2.0)", &m_ui.AmbientIntensity, 0.01f, 0.f, 2.f);

            ImGui::Checkbox("Enable Procedural Sky", &m_ui.EnableProceduralSky);
            ImGui::Checkbox("Enable SSAO", &m_ui.EnableSsao);
            ImGui::Checkbox("Enable Bloom", &m_ui.EnableBloom);
            ImGui::DragFloat("Bloom Sigma (0.1 - 100.0)", &m_ui.BloomSigma, 0.01f, 0.1f, 100.f);
            ImGui::Checkbox("Enable Shadows", &m_ui.EnableShadows);
            ImGui::Checkbox("Enable Translucency", &m_ui.EnableTranslucency);
            ImGui::Checkbox("Material Events", &m_ui.EnableMaterialEvents);
            ImGui::Text("");

            ImGui::Separator();
            ImGui::Text("Temporal AA ");
            ImGui::Separator();
            ImGui::Checkbox("Clamping", &m_ui.TemporalAntiAliasingParams.enableHistoryClamping);
            ImGui::Text("");
        }
        ImGui::End();
    }
};