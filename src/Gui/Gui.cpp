#include "Gui.hpp"

#include <unistd.h>

#include <iostream>
#include <random>
#include <sstream>

#include "ElfReader.hpp"
#include "ImguiPlugins.hpp"
#include "VarReader.hpp"
#include "glfw3.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include "nfd.h"

Gui::Gui(PlotHandler* plotHandler, ConfigHandler* configHandler, bool& done, std::mutex* mtx) : plotHandler(plotHandler), configHandler(configHandler), done(done), mtx(mtx)
{
	elfReader = std::make_unique<ElfReader>(projectElfFile);
	threadHandle = std::thread(&Gui::mainThread, this);
}

Gui::~Gui()
{
	if (threadHandle.joinable())
		threadHandle.join();
}

static void glfw_error_callback(int error, const char* description)
{
	fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

void Gui::mainThread()
{
	glfwSetErrorCallback(glfw_error_callback);
	if (!glfwInit())
		return;

	GLFWwindow* window = glfwCreateWindow(1500, 1000, "STMViewer", NULL, NULL);
	if (window == NULL)
		return;
	glfwMakeContextCurrent(window);
	glfwSwapInterval(1);  // Enable vsync

	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImPlot::CreateContext();

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

	// Setup Dear ImGui style
	ImGui::StyleColorsDark();

	ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
	const char* glsl_version = "#version 130";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	// Setup Platform/Renderer backends
	ImGui_ImplGlfw_InitForOpenGL(window, true);
	ImGui_ImplOpenGL3_Init(glsl_version);

	NFD_Init();

	bool show_demo_window = true;
	bool p_open = true;

	while (!done)
	{
		done = glfwWindowShouldClose(window);
		glfwPollEvents();

		// Start the Dear ImGui frame
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();

		ImGui::NewFrame();
		ImGui::DockSpaceOverViewport(ImGui::GetMainViewport());

		if (show_demo_window)
			ImPlot::ShowDemoWindow();

		ImGui::Begin("Plots", &p_open, 0);
		if (showAcqusitionSettingsWindow)
			drawAcqusitionSettingsWindow();

		uint32_t tablePlots = 0;

		for (std::shared_ptr<Plot> plt : *plotHandler)
		{
			if (plt->getType() == Plot::type_E::TABLE)
			{
				drawPlotTable(plt.get(), plt->getTimeSeries(), plt->getSeriesMap());
				if (plt->getVisibility())
					tablePlots++;
			}
		}

		uint32_t curveBarPlotsCnt = plotHandler->getVisiblePlotsCount() - tablePlots;
		uint32_t row = curveBarPlotsCnt > 0 ? curveBarPlotsCnt : 1;

		if (ImPlot::BeginSubplots("##subplos", row, 1, ImVec2(-1, -1)))
		{
			for (std::shared_ptr<Plot> plt : *plotHandler)
				if (plt->getType() == Plot::type_E::CURVE || plt->getType() == Plot::type_E::BAR)
					drawPlotCurveBar(plt.get(), plt->getTimeSeries(), plt->getSeriesMap(), tablePlots);
			ImPlot::EndSubplots();
		}

		drawMenu();
		ImGui::End();

		ImGui::Begin("VarViewer", &p_open, 0);
		drawStartButton();
		drawAddVariableButton();
		drawUpdateAddressesFromElf();
		drawVarTable();
		drawPlotsTree();
		ImGui::End();

		// Rendering
		ImGui::Render();
		int display_w, display_h;
		glfwGetFramebufferSize(window, &display_w, &display_h);
		glViewport(0, 0, display_w, display_h);
		glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

		if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			GLFWwindow* backup_current_context = glfwGetCurrentContext();
			ImGui::UpdatePlatformWindows();
			ImGui::RenderPlatformWindowsDefault();
			glfwMakeContextCurrent(backup_current_context);
		}

		glfwSwapBuffers(window);
	}

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	glfwDestroyWindow(window);
	glfwTerminate();
	NFD_Quit();
}

void Gui::drawMenu()
{
	ImGui::BeginMainMenuBar();

	if (ImGui::BeginMenu("File"))
	{
		if (ImGui::MenuItem("New"))
		{
		}
		if (ImGui::MenuItem("Open", "Ctrl+O"))
		{
			nfdchar_t* outPath;
			nfdfilteritem_t filterItem[1] = {{"Project files", "cfg"}};
			nfdresult_t result = NFD_OpenDialog(&outPath, filterItem, 1, NULL);
			if (result == NFD_OKAY)
			{
				configHandler->changeConfigFile(std::string(outPath));
				vars.clear();
				plotHandler->removeAllPlots();
				projectElfFile = configHandler->getElfFilePath();
				configHandler->readConfigFile(vars, projectElfFile);
				std::replace(projectElfFile.begin(), projectElfFile.end(), '\\', '/');
				std::cout << outPath << std::endl;
				NFD_FreePath(outPath);
			}
			else if (result == NFD_ERROR)
			{
				std::cout << "Error: %s\n"
						  << NFD_GetError() << std::endl;
			}
		}
		if (ImGui::MenuItem("Save", "Ctrl+S"))
		{
			configHandler->saveConfigFile(vars, projectElfFile, projectConfigFile);
		}
		if (ImGui::MenuItem("Save As.."))
		{
			nfdchar_t* outPath;
			nfdfilteritem_t filterItem[1] = {{"Project files", "cfg"}};
			nfdresult_t result = NFD_SaveDialog(&outPath, filterItem, 1, NULL, NULL);
			if (result == NFD_OKAY)
			{
				configHandler->saveConfigFile(vars, projectElfFile, std::string(outPath));
				NFD_FreePath(outPath);
			}
			else if (result == NFD_ERROR)
			{
				std::cout << "Error: %s\n"
						  << NFD_GetError() << std::endl;
			}
		}
		if (ImGui::MenuItem("Quit"))
		{
			done = true;
		}
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Options"))
	{
		ImGui::MenuItem("Acqusition settings...", NULL, &showAcqusitionSettingsWindow);
		ImGui::EndMenu();
	}
	ImGui::EndMainMenuBar();
}

void Gui::drawStartButton()
{
	if (viewerState == state::RUN)
	{
		ImVec4 color = (ImVec4)ImColor::HSV(0.365f, 0.94f, 0.37f);
		ImGui::PushStyleColor(ImGuiCol_Button, color);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
	}
	else if (viewerState == state::STOP)
	{
		ImVec4 color = ImColor::HSV(0.116f, 0.97f, 0.72f);
		ImGui::PushStyleColor(ImGuiCol_Button, color);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
	}

	if (ImGui::Button(viewerStateMap.at(viewerState).c_str(), ImVec2(-1, 50)))
	{
		if (viewerState == state::STOP)
		{
			viewerState = state::RUN;
			plotHandler->eraseAllPlotData();
			plotHandler->setViewerState((PlotHandler::state)state::RUN);
		}
		else
		{
			plotHandler->setViewerState((PlotHandler::state)state::STOP);
			viewerState = state::STOP;
		}
	}

	ImGui::PopStyleColor(3);
}
void Gui::drawAddVariableButton()
{
	if (ImGui::Button("Add variable", ImVec2(-1, 30)))
	{
		uint32_t num = 0;
		while (vars.find(std::string("new") + std::to_string(num)) != vars.end())
		{
			num++;
		}
		std::string newName = std::string("new") + std::to_string(num);

		std::shared_ptr<Variable> newVar = std::make_shared<Variable>(newName);
		newVar->setAddress(0x20000000);
		newVar->setType(Variable::type::U8);
		std::random_device rd;
		std::mt19937 gen(rd());
		std::uniform_int_distribution<> dist(0, UINT32_MAX);
		newVar->setColor(static_cast<uint32_t>(dist(gen)));
		vars.emplace(newName, newVar);
	}
}
void Gui::drawUpdateAddressesFromElf()
{
	if (ImGui::Button("Update Variable addresses", ImVec2(-1, 30)))
		elfReader->updateVariableMap(vars);
}

void Gui::drawVarTable()
{
	static ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable;

	if (ImGui::BeginTable("table_scrolly", 3, flags, ImVec2(0.0f, 300)))
	{
		ImGui::TableSetupScrollFreeze(0, 1);  // Make top row always visible
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_None);
		ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_None);
		ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_None);
		ImGui::TableHeadersRow();

		std::string varNameToDelete = {};

		for (auto& [keyName, var] : vars)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::PushID(keyName.c_str());
			ImGui::ColorEdit4("##", &var->getColor().r, ImGuiColorEditFlags_NoInputs);
			ImGui::SameLine();
			ImGui::PopID();
			char variable[maxVariableNameLength] = {0};
			memcpy(variable, var->getName().data(), (var->getName().length()));
			ImGui::SelectableInput(var->getName().c_str(), false, ImGuiSelectableFlags_None, variable, maxVariableNameLength);
			if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
			{
				auto varr = vars.extract(var->getName());
				varr.key() = std::string(variable);
				var->setName(variable);
				vars.insert(std::move(varr));
			}
			/* TODO std::optional ?*/
			if (showDeletePopup("Delete", keyName) == keyName)
				varNameToDelete = keyName;

			if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
			{
				ImGui::SetDragDropPayload("MY_DND", &var->getName(), sizeof(var->getName()));
				ImPlot::ItemIcon(var->getColorU32());
				ImGui::SameLine();
				ImGui::TextUnformatted(var->getName().c_str());
				ImGui::EndDragDropSource();
			}
			ImGui::TableSetColumnIndex(1);
			ImGui::Text(("0x" + std::string(intToHexString(var->getAddress()))).c_str());
			ImGui::TableSetColumnIndex(2);
			ImGui::Text(var->getTypeStr().c_str());
		}
		if (varNameToDelete != "")
		{
			for (std::shared_ptr<Plot> plt : *plotHandler)
				plt->removeSeries(varNameToDelete);
			vars.erase(varNameToDelete);
		}
		ImGui::EndTable();
	}
}

void Gui::drawPlotsTree()
{
	ImGui::SetNextItemOpen(true, ImGuiCond_Once);
	std::string newName = "";

	if (ImGui::BeginTabBar("MyTabBar", ImGuiTabBarFlags_Reorderable))
	{
		if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip))
			plotHandler->addPlot("new plot");

		if (ImGui::BeginPopupContextWindow())
		{
			if (ImGui::Button("Add plot"))
			{
				ImGui::CloseCurrentPopup();
				plotHandler->addPlot("new plot");
			}
			ImGui::EndPopup();
		}

		std::string plotNameToDelete = {};

		for (std::shared_ptr<Plot> plt : *plotHandler)
		{
			const char* plotTypes[3] = {"curve", "bar", "table"};
			int32_t typeCombo = (int32_t)plt->getType();

			newName = plt->getName();

			if (ImGui::BeginTabItem(plt->getName().c_str()))
			{
				/* TODO std::optional ?*/
				if (showDeletePopup("Delete plot", plt->getName()) == plt->getName())
					plotNameToDelete = plt->getName();

				ImGui::Text("name    ");
				ImGui::SameLine();
				ImGui::PushID("input");
				ImGui::InputText("##", &newName, 0, NULL, NULL);
				ImGui::PopID();
				ImGui::Text("type    ");
				ImGui::SameLine();
				ImGui::PushID("combo");
				ImGui::Combo("##", &typeCombo, plotTypes, IM_ARRAYSIZE(plotTypes));
				ImGui::PopID();
				ImGui::Text("visible ");
				ImGui::SameLine();
				ImGui::Checkbox("##", &plt->getVisibilityVar());
				ImGui::PushID("list");
				if (ImGui::BeginListBox("##", ImVec2(-1, 80)))
				{
					std::string seriesNameToDelete = {};
					for (auto& [name, ser] : plt->getSeriesMap())
					{
						ImGui::Selectable(name.c_str());
						/* TODO std::optional ?*/
						if (showDeletePopup("Delete var", name) == name)
							seriesNameToDelete = name;
					}
					plt->removeSeries(seriesNameToDelete);
					ImGui::EndListBox();
				}
				ImGui::PopID();
				ImGui::TreePop();
			}

			if (typeCombo != (int32_t)plt->getType())
				plt->setType(static_cast<Plot::type_E>(typeCombo));

			if ((ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) && newName != plt->getName())
				plotHandler->renamePlot(plt->getName(), newName);
		}
		ImGui::EndTabBar();

		plotHandler->removePlot(plotNameToDelete);
	}
}

void Gui::drawAcqusitionSettingsWindow()
{
	ImGui::Begin("Acqusition Settings", &showAcqusitionSettingsWindow, 0);
	ImGui::Text("Please pick *.elf file");
	ImGui::InputText("##", &projectElfFile, 0, NULL, NULL);
	ImGui::SameLine();
	if (ImGui::SmallButton("..."))
	{
		nfdchar_t* outPath;
		nfdfilteritem_t filterItem[1] = {{"Executable files", "elf"}};
		nfdresult_t result = NFD_OpenDialog(&outPath, filterItem, 1, NULL);
		if (result == NFD_OKAY)
		{
			std::cout << outPath << std::endl;
			projectElfFile = std::string(outPath);
			std::replace(projectElfFile.begin(), projectElfFile.end(), '\\', '/');
			NFD_FreePath(outPath);
		}
		else if (result == NFD_ERROR)
		{
			std::cout << "Error: %s\n"
					  << NFD_GetError() << std::endl;
		}
	}

	if (ImGui::Button("Done"))
	{
		showAcqusitionSettingsWindow = false;
	}

	ImGui::End();
}

void Gui::drawPlotCurveBar(Plot* plot, ScrollingBuffer<float>& time, std::map<std::string, std::shared_ptr<Plot::Series>>& seriesMap, uint32_t curveBarPlots)
{
	if (!plot->getVisibility())
		return;

	ImVec2 plotSize = ImVec2(-1, -1);

	if (plot->getType() == Plot::type_E::CURVE)
	{
		if (ImPlot::BeginPlot(plot->getName().c_str(), plotSize, ImPlotFlags_NoChild))
		{
			if (plotHandler->getViewerState())
				ImPlot::SetupAxes("time[s]", NULL, ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
			else
			{
				ImPlot::SetupAxes("time[s]", NULL, 0, 0);
				ImPlot::SetupAxisLimits(ImAxis_X1, -1, 10, ImPlotCond_Once);
				ImPlot::SetupAxisLimits(ImAxis_Y1, -0.1, 0.1, ImPlotCond_Once);
			}

			if (ImPlot::BeginDragDropTargetPlot())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MY_DND"))
					plot->addSeries(*vars[*(std::string*)payload->Data]);

				ImPlot::EndDragDropTarget();
			}

			/* make thread safe copies of buffers - probably can be made better but it works */
			mtx->lock();
			time.copyData();
			for (auto& [key, serPtr] : seriesMap)
				serPtr->buffer->copyData();
			uint32_t offset = time.getOffset();
			uint32_t size = time.getSize();
			mtx->unlock();

			for (auto& [key, serPtr] : seriesMap)
			{
				ImPlot::SetNextLineStyle(ImVec4(serPtr->var->getColor().r, serPtr->var->getColor().g, serPtr->var->getColor().b, 1.0f));
				ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle);
				ImPlot::PlotLine(serPtr->var->getName().c_str(), time.getFirstElementCopy(), serPtr->buffer->getFirstElementCopy(), size, 0, offset, sizeof(float));
			}

			ImPlot::EndPlot();
		}
	}
	else if (plot->getType() == Plot::type_E::BAR)
	{
		if (ImPlot::BeginPlot(plot->getName().c_str(), plotSize, ImPlotFlags_NoChild))
		{
			std::vector<const char*> glabels;
			std::vector<double> positions;

			float pos = 0.0f;
			for (const auto& [key, series] : seriesMap)
			{
				glabels.push_back(series->var->getName().c_str());
				positions.push_back(pos);
				pos += 1.0f;
			}
			glabels.push_back(nullptr);

			ImPlot::SetupAxes(NULL, "Value", 0, 0);
			ImPlot::SetupAxisLimits(ImAxis_X1, -1, seriesMap.size(), ImPlotCond_Always);
			ImPlot::SetupAxisTicks(ImAxis_X1, positions.data(), seriesMap.size(), glabels.data());

			if (ImPlot::BeginDragDropTargetPlot())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MY_DND"))
					plot->addSeries(*vars[*(std::string*)payload->Data]);
				ImPlot::EndDragDropTarget();
			}

			float xs = 0.0f;
			float barSize = 0.5f;

			for (auto& [key, serPtr] : seriesMap)
			{
				float value = *serPtr->buffer->getLastElement();

				ImPlot::SetNextLineStyle(ImVec4(serPtr->var->getColor().r, serPtr->var->getColor().g, serPtr->var->getColor().b, 1.0f));
				ImPlot::PlotBars(serPtr->var->getName().c_str(), &xs, &value, 1, barSize);
				float textX = xs - barSize / 4.0f;
				float textY = value / 2.0f;
				ImPlot::Annotation(textX, textY, ImPlot::GetLastItemColor(), ImVec2(0.5f, 0.5f), false, "%.5f", value);
				xs += 1.0f;
			}
			ImPlot::EndPlot();
		}
	}
}

void Gui::drawPlotTable(Plot* plot, ScrollingBuffer<float>& time, std::map<std::string, std::shared_ptr<Plot::Series>>& seriesMap)
{
	if (!plot->getVisibility())
		return;

	static ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV;

	if (ImGui::BeginTable(plot->getName().c_str(), 4, flags))
	{
		ImGui::TableSetupScrollFreeze(0, 1);  // Make top row always visible
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_None);
		ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_None);
		ImGui::TableSetupColumn("Read value", ImGuiTableColumnFlags_None);
		ImGui::TableSetupColumn("Write value", ImGuiTableColumnFlags_None);
		ImGui::TableHeadersRow();

		for (auto& [key, serPtr] : seriesMap)
		{
			float value = *serPtr->buffer->getLastElement();
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Text(key.c_str());
			ImGui::TableSetColumnIndex(1);
			ImGui::Text(("0x" + std::string(intToHexString(serPtr->var->getAddress()))).c_str());
			ImGui::TableSetColumnIndex(2);
			ImGui::Text(std::to_string(value).c_str());
			ImGui::TableSetColumnIndex(3);
			ImGui::PushID("input");
			char newValue[maxVariableNameLength] = {0};
			if (ImGui::SelectableInput(key.c_str(), false, ImGuiSelectableFlags_None, newValue, maxVariableNameLength))
			{
				if (!plotHandler->getViewerState())
				{
					ImGui::PopID();
					continue;
				}
				if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
				{
					std::cout << "VALUE:" << atof(newValue) << std::endl;
					if (!plotHandler->writeSeriesValue(*serPtr->var, static_cast<float>(atof(newValue))))
						std::cout << "ERROR while writing new value!" << std::endl;
				}
			}
			ImGui::PopID();
		}
		ImGui::EndTable();

		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MY_DND"))
				plot->addSeries(*vars[*(std::string*)payload->Data]);
			ImGui::EndDragDropTarget();
		}
	}
}

std::string Gui::showDeletePopup(const char* text, const std::string name)
{
	std::string nameToDelete = "";
	if (ImGui::BeginPopupContextItem())
	{
		if (ImGui::Button(text))
		{
			ImGui::CloseCurrentPopup();
			nameToDelete = name;
		}
		ImGui::EndPopup();
	}

	return nameToDelete;
}

std::string Gui::intToHexString(uint32_t var)
{
	std::stringstream ss;
	ss << std::hex << var;
	return ss.str();
}