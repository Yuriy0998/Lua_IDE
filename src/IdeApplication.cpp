#include "IdeApplication.hpp"

#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>

#include <imgui.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

namespace
{
    std::string WideToUtf8(const std::wstring& text)
    {
        if (text.empty())
            return {};

        const int length = WideCharToMultiByte(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            nullptr,
            0,
            nullptr,
            nullptr
        );

        if (length <= 0)
            throw std::runtime_error("UTF-8 conversion failed.");

        std::string result(
            static_cast<std::size_t>(length),
            '\0'
        );

        WideCharToMultiByte(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            result.data(),
            length,
            nullptr,
            nullptr
        );

        return result;
    }

    std::string PathNameUtf8(
        const std::filesystem::path& path)
    {
        return WideToUtf8(path.filename().wstring());
    }

    bool IsLuaFile(const std::filesystem::path& path)
    {
        std::wstring extension = path.extension().wstring();

        std::transform(
            extension.begin(),
            extension.end(),
            extension.begin(),
            towlower
        );

        return extension == L".lua" ||
            extension == L".txt";
    }

    bool ChooseFile(
        bool save,
        std::filesystem::path& result,
        const std::filesystem::path& current = {})
    {
        wchar_t buffer[32768]{};

        if (!current.empty())
        {
            const std::wstring value = current.wstring();

            if (value.size() < std::size(buffer))
                wcscpy_s(buffer, value.c_str());
        }

        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = nullptr;
        dialog.lpstrFilter =
            L"Lua files (*.lua)\0*.lua\0"
            L"Text files (*.txt)\0*.txt\0"
            L"All files (*.*)\0*.*\0";
        dialog.lpstrFile = buffer;
        dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
        dialog.lpstrDefExt = L"lua";
        dialog.Flags =
            OFN_EXPLORER |
            OFN_NOCHANGEDIR |
            OFN_PATHMUSTEXIST |
            (save
                ? OFN_OVERWRITEPROMPT
                : OFN_FILEMUSTEXIST);

        const BOOL accepted = save
            ? GetSaveFileNameW(&dialog)
            : GetOpenFileNameW(&dialog);

        if (!accepted)
            return false;

        result = buffer;
        return true;
    }

    bool ChooseDirectory(std::filesystem::path& result)
    {
        IFileDialog* dialog = nullptr;

        const HRESULT createResult = CoCreateInstance(
            CLSID_FileOpenDialog,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)
        );

        if (FAILED(createResult))
            return false;

        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(
            options |
            FOS_PICKFOLDERS |
            FOS_FORCEFILESYSTEM
        );

        const HRESULT showResult = dialog->Show(nullptr);

        if (FAILED(showResult))
        {
            dialog->Release();
            return false;
        }

        IShellItem* item = nullptr;
        bool success = false;

        if (SUCCEEDED(dialog->GetResult(&item)))
        {
            PWSTR path = nullptr;

            if (SUCCEEDED(item->GetDisplayName(
                SIGDN_FILESYSPATH,
                &path)))
            {
                result = path;
                CoTaskMemFree(path);
                success = true;
            }

            item->Release();
        }

        dialog->Release();
        return success;
    }

    TextEditor::Coordinates OffsetToCoordinates(
        const std::string& text,
        std::size_t offset)
    {
        int line = 0;
        int column = 0;

        offset = std::min(offset, text.size());

        for (std::size_t i = 0; i < offset; ++i)
        {
            const unsigned char character =
                static_cast<unsigned char>(text[i]);

            if (character == '\n')
            {
                ++line;
                column = 0;
            }
            else if ((character & 0xC0) != 0x80)
            {
                ++column;
            }
        }

        return { line, column };
    }
}

IdeApplication::IdeApplication()
{
    NewDocument();

    activeDocument_->editor.SetText(
        "-- LuaIDE на Dear ImGui\n"
        "-- F5: запуск, Shift+F5: остановка\n\n"
        "local function factorial(n)\n"
        "    if n <= 1 then\n"
        "        return 1\n"
        "    end\n\n"
        "    return n * factorial(n - 1)\n"
        "end\n\n"
        "for i = 1, 10 do\n"
        "    print(i, factorial(i))\n"
        "end\n"
    );

    activeDocument_->dirty = false;

    consoleText_ =
        "LuaIDE console\n"
        "Нажмите F5 для запуска программы.\n";
}

IdeApplication::~IdeApplication()
{
    runner_.Stop();
    runner_.Join();
}

void IdeApplication::NewDocument()
{
    auto document = std::make_unique<Document>();

    document->name =
        "Без имени " +
        std::to_string(documents_.size() + 1) +
        ".lua";

    document->editor.SetLanguageDefinition(
        TextEditor::LanguageDefinition::Lua()
    );

    document->editor.SetPalette(
        TextEditor::GetDarkPalette()
    );

    document->editor.SetTabSize(4);
    document->editor.SetShowWhitespaces(false);
    document->requestSelect = true;

    activeDocument_ = document.get();
    documents_.push_back(std::move(document));
}

std::string IdeApplication::ReadFile(
    const std::filesystem::path& path)
{
    if (std::filesystem::file_size(path) >
        32ull * 1024ull * 1024ull)
    {
        throw std::runtime_error(
            "The file exceeds the 32 MB limit."
        );
    }

    std::ifstream stream(path, std::ios::binary);

    if (!stream)
        throw std::runtime_error("Cannot open file.");

    std::string data{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()
    };

    if (stream.bad())
        throw std::runtime_error("Cannot read file.");

    if (data.compare(0, 3, "\xEF\xBB\xBF") == 0)
        data.erase(0, 3);

    if (data.find('\0') != std::string::npos)
        throw std::runtime_error("Binary file is not supported.");

    return data;
}

void IdeApplication::WriteFile(
    const std::filesystem::path& path,
    const std::string& text)
{
    std::ofstream stream(
        path,
        std::ios::binary | std::ios::trunc
    );

    if (!stream)
        throw std::runtime_error(
            "Cannot open file for writing."
        );

    stream.write(
        text.data(),
        static_cast<std::streamsize>(text.size())
    );

    if (!stream)
        throw std::runtime_error("Cannot write file.");
}

void IdeApplication::OpenDocumentDialog()
{
    std::filesystem::path path;

    if (ChooseFile(false, path))
        OpenDocument(path);
}

void IdeApplication::OpenDocument(
    const std::filesystem::path& path)
{
    for (auto& document : documents_)
    {
        std::error_code error;

        if (!document->path.empty() &&
            std::filesystem::equivalent(
                document->path,
                path,
                error))
        {
            activeDocument_ = document.get();
            document->requestSelect = true;
            return;
        }
    }

    auto document = std::make_unique<Document>();

    document->path = path;
    document->name = PathNameUtf8(path);

    document->editor.SetLanguageDefinition(
        TextEditor::LanguageDefinition::Lua()
    );

    document->editor.SetPalette(
        TextEditor::GetDarkPalette()
    );

    document->editor.SetTabSize(4);
    document->editor.SetShowWhitespaces(false);
    document->editor.SetText(ReadFile(path));

    document->dirty = false;
    document->requestSelect = true;

    activeDocument_ = document.get();
    documents_.push_back(std::move(document));
}

void IdeApplication::OpenProjectDialog()
{
    std::filesystem::path path;

    if (ChooseDirectory(path))
        projectDirectory_ = path;
}

bool IdeApplication::SaveDocument(
    Document& document,
    bool saveAs)
{
    std::filesystem::path destination = document.path;

    if (saveAs || destination.empty())
    {
        if (!ChooseFile(true, destination, destination))
            return false;
    }

    WriteFile(destination, document.editor.GetText());

    document.path = destination;
    document.name = PathNameUtf8(destination);
    document.dirty = false;

    return true;
}

bool IdeApplication::ConfirmCloseDocument(Document& document)
{
    if (!document.dirty)
        return true;

    const int answer = MessageBoxW(
        nullptr,
        L"Сохранить изменения перед закрытием?",
        L"LuaIDE",
        MB_YESNOCANCEL | MB_ICONQUESTION
    );

    if (answer == IDCANCEL)
        return false;

    if (answer == IDYES)
        return SaveDocument(document, false);

    return true;
}

void IdeApplication::CloseDocument(Document& document)
{
    if (!ConfirmCloseDocument(document))
    {
        document.requestSelect = true;
        return;
    }

    const auto iterator = std::find_if(
        documents_.begin(),
        documents_.end(),
        [&document](const auto& item)
        {
            return item.get() == &document;
        }
    );

    if (iterator == documents_.end())
        return;

    const bool wasActive = activeDocument_ == &document;
    documents_.erase(iterator);

    if (documents_.empty())
    {
        activeDocument_ = nullptr;
        NewDocument();
    }
    else if (wasActive)
    {
        activeDocument_ = documents_.back().get();
        activeDocument_->requestSelect = true;
    }
}

void IdeApplication::QueueConsole(
    const char* text,
    std::size_t size)
{
    std::lock_guard<std::mutex> lock(consoleMutex_);

    constexpr std::size_t limit = 4 * 1024 * 1024;

    if (pendingConsole_.size() < limit)
    {
        const std::size_t remaining =
            limit - pendingConsole_.size();

        pendingConsole_.append(
            text,
            std::min(size, remaining)
        );
    }
}

void IdeApplication::FlushConsole()
{
    std::string output;

    {
        std::lock_guard<std::mutex> lock(consoleMutex_);
        output.swap(pendingConsole_);
    }

    if (output.empty())
        return;

    consoleText_ += output;

    constexpr std::size_t consoleLimit =
        4 * 1024 * 1024;

    if (consoleText_.size() > consoleLimit)
    {
        consoleText_.erase(
            0,
            consoleText_.size() - consoleLimit
        );
    }

    scrollConsoleToBottom_ = true;
}

void IdeApplication::RunDocument()
{
    if (!activeDocument_ || runner_.IsRunning())
        return;

    runner_.Join();

    const std::string source =
        activeDocument_->editor.GetText();

    const std::string chunkName =
        activeDocument_->path.empty()
        ? "=untitled.lua"
        : "@" + WideToUtf8(
            activeDocument_->path.wstring()
        );

    const std::string moduleDirectory =
        activeDocument_->path.empty()
        ? std::string{}
        : WideToUtf8(
            activeDocument_->path
            .parent_path()
            .wstring()
        );

    consoleText_.clear();
    consoleText_ = "--- Running ---\n";
    scrollConsoleToBottom_ = true;

    runner_.Run(
        source,
        chunkName,
        moduleDirectory,

        [this](const char* text, std::size_t size)
        {
            QueueConsole(text, size);
        },

        [this](bool)
        {
            QueueConsole("", 0);
        }
    );
}

void IdeApplication::StopDocument()
{
    runner_.Stop();
}

void IdeApplication::ProcessShortcuts()
{
    ImGuiIO& io = ImGui::GetIO();

    if (io.KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_N, false))
    {
        NewDocument();
    }

    if (io.KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_O, false))
    {
        OpenDocumentDialog();
    }

    if (io.KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_S, false) &&
        activeDocument_)
    {
        SaveDocument(*activeDocument_, io.KeyShift);
    }

    if (io.KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_F, false))
    {
        showSearch_ = true;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F5, false))
    {
        if (io.KeyShift)
            StopDocument();
        else
            RunDocument();
    }
}

void IdeApplication::RenderMainMenu()
{
    if (!ImGui::BeginMenuBar())
        return;

    if (ImGui::BeginMenu("Файл"))
    {
        if (ImGui::MenuItem("Новый", "Ctrl+N"))
            NewDocument();

        if (ImGui::MenuItem("Открыть...", "Ctrl+O"))
            OpenDocumentDialog();

        if (ImGui::MenuItem("Открыть папку..."))
            OpenProjectDialog();

        ImGui::Separator();

        if (ImGui::MenuItem(
            "Сохранить",
            "Ctrl+S",
            false,
            activeDocument_ != nullptr))
        {
            SaveDocument(*activeDocument_, false);
        }

        if (ImGui::MenuItem(
            "Сохранить как...",
            "Ctrl+Shift+S",
            false,
            activeDocument_ != nullptr))
        {
            SaveDocument(*activeDocument_, true);
        }

        ImGui::Separator();

        if (ImGui::MenuItem(
            "Закрыть вкладку",
            "Ctrl+W",
            false,
            activeDocument_ != nullptr))
        {
            CloseDocument(*activeDocument_);
        }

        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Правка"))
    {
        const bool enabled = activeDocument_ != nullptr;

        if (ImGui::MenuItem("Отменить", "Ctrl+Z", false, enabled))
            activeDocument_->editor.Undo();

        if (ImGui::MenuItem("Повторить", "Ctrl+Y", false, enabled))
            activeDocument_->editor.Redo();

        ImGui::Separator();

        if (ImGui::MenuItem("Вырезать", "Ctrl+X", false, enabled))
            activeDocument_->editor.Cut();

        if (ImGui::MenuItem("Копировать", "Ctrl+C", false, enabled))
            activeDocument_->editor.Copy();

        if (ImGui::MenuItem("Вставить", "Ctrl+V", false, enabled))
            activeDocument_->editor.Paste();

        if (ImGui::MenuItem("Выделить всё", "Ctrl+A", false, enabled))
            activeDocument_->editor.SelectAll();

        ImGui::Separator();

        if (ImGui::MenuItem("Поиск и замена", "Ctrl+F"))
            showSearch_ = true;

        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Выполнение"))
    {
        if (ImGui::MenuItem(
            "Запустить",
            "F5",
            false,
            activeDocument_ && !runner_.IsRunning()))
        {
            RunDocument();
        }

        if (ImGui::MenuItem(
            "Остановить",
            "Shift+F5",
            false,
            runner_.IsRunning()))
        {
            StopDocument();
        }

        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Вид"))
    {
        ImGui::MenuItem(
            "Проводник проекта",
            nullptr,
            &showProject_
        );

        ImGui::MenuItem(
            "Консоль",
            nullptr,
            &showConsole_
        );

        ImGui::MenuItem(
            "Поиск",
            nullptr,
            &showSearch_
        );

        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Справка"))
    {
        if (ImGui::MenuItem("О программе"))
            showAbout_ = true;

        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

void IdeApplication::RenderDockSpace()
{
    const ImGuiViewport* viewport =
        ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_MenuBar |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowRounding,
        0.0f
    );

    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowBorderSize,
        0.0f
    );

    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(0.0f, 0.0f)
    );

    ImGui::Begin("LuaIDE.DockSpace", nullptr, flags);
    ImGui::PopStyleVar(3);

    RenderMainMenu();

    const ImGuiID dockspaceId =
        ImGui::GetID("LuaIDE.MainDockSpace");

    ImGui::DockSpace(
        dockspaceId,
        ImVec2(0.0f, 0.0f)
    );

    ImGui::End();
}

void IdeApplication::RenderToolbar()
{
    ImGui::Begin("Панель инструментов");

    if (ImGui::Button("Новый"))
        NewDocument();

    ImGui::SameLine();

    if (ImGui::Button("Открыть"))
        OpenDocumentDialog();

    ImGui::SameLine();

    if (ImGui::Button("Сохранить") && activeDocument_)
        SaveDocument(*activeDocument_, false);

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    ImGui::BeginDisabled(
        !activeDocument_ || runner_.IsRunning()
    );

    if (ImGui::Button("Запустить F5"))
        RunDocument();

    ImGui::EndDisabled();

    ImGui::SameLine();

    ImGui::BeginDisabled(!runner_.IsRunning());

    if (ImGui::Button("Остановить"))
        StopDocument();

    ImGui::EndDisabled();

    ImGui::SameLine();

    if (runner_.IsRunning())
    {
        ImGui::TextColored(
            ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
            "Выполняется..."
        );
    }
    else
    {
        ImGui::TextDisabled("Готово");
    }

    ImGui::End();
}

void IdeApplication::RenderDocuments()
{
    ImGui::Begin("Редактор");

    if (ImGui::BeginTabBar(
        "Documents",
        ImGuiTabBarFlags_Reorderable |
        ImGuiTabBarFlags_AutoSelectNewTabs |
        ImGuiTabBarFlags_FittingPolicyScroll))
    {
        Document* closeRequest = nullptr;

        for (auto& pointer : documents_)
        {
            Document& document = *pointer;

            std::string title = document.name;

            if (document.dirty)
                title += " *";

            title += "###Document";
            title += std::to_string(
                reinterpret_cast<std::uintptr_t>(&document)
            );

            bool opened = true;

            ImGuiTabItemFlags flags =
                ImGuiTabItemFlags_None;

            if (document.requestSelect)
            {
                flags |= ImGuiTabItemFlags_SetSelected;
                document.requestSelect = false;
            }

            if (ImGui::BeginTabItem(
                title.c_str(),
                &opened,
                flags))
            {
                activeDocument_ = &document;

                document.editor.Render(
                    "##TextEditor",
                    ImVec2(-1.0f, -1.0f),
                    false
                );

                if (document.editor.IsTextChanged())
                    document.dirty = true;

                ImGui::EndTabItem();
            }

            if (!opened)
                closeRequest = &document;
        }

        ImGui::EndTabBar();

        if (closeRequest)
            CloseDocument(*closeRequest);
    }

    ImGui::End();
}

void IdeApplication::RenderProjectDirectory(
    const std::filesystem::path& directory)
{
    std::error_code error;

    std::vector<std::filesystem::directory_entry> entries;

    for (std::filesystem::directory_iterator iterator(
        directory,
        std::filesystem::directory_options::
        skip_permission_denied,
        error);
        !error &&
        iterator != std::filesystem::directory_iterator();
        iterator.increment(error))
    {
        entries.push_back(*iterator);
    }

    std::sort(
        entries.begin(),
        entries.end(),
        [](const auto& left, const auto& right)
        {
            if (left.is_directory() != right.is_directory())
                return left.is_directory();

            return left.path().filename() <
                right.path().filename();
        }
    );

    for (const auto& entry : entries)
    {
        const std::string name =
            PathNameUtf8(entry.path());

        if (entry.is_directory(error))
        {
            const bool opened = ImGui::TreeNodeEx(
                entry.path().string().c_str(),
                ImGuiTreeNodeFlags_SpanAvailWidth,
                "%s",
                name.c_str()
            );

            if (opened)
            {
                RenderProjectDirectory(entry.path());
                ImGui::TreePop();
            }
        }
        else if (entry.is_regular_file(error))
        {
            const bool supported = IsLuaFile(entry.path());

            if (!supported)
                ImGui::BeginDisabled();

            if (ImGui::Selectable(name.c_str(), false) &&
                supported)
            {
                OpenDocument(entry.path());
            }

            if (!supported)
                ImGui::EndDisabled();
        }
    }
}

void IdeApplication::RenderProjectExplorer()
{
    if (!showProject_)
        return;

    if (!ImGui::Begin(
        "Проводник проекта",
        &showProject_))
    {
        ImGui::End();
        return;
    }

    if (ImGui::Button("Открыть папку..."))
        OpenProjectDialog();

    ImGui::Separator();

    if (projectDirectory_.empty())
    {
        ImGui::TextDisabled("Папка проекта не открыта.");
    }
    else
    {
        ImGui::TextWrapped(
            "%s",
            WideToUtf8(projectDirectory_.wstring()).c_str()
        );

        ImGui::Separator();
        RenderProjectDirectory(projectDirectory_);
    }

    ImGui::End();
}

void IdeApplication::RenderConsole()
{
    if (!showConsole_)
        return;

    if (!ImGui::Begin("Консоль", &showConsole_))
    {
        ImGui::End();
        return;
    }

    if (ImGui::Button("Очистить"))
        consoleText_.clear();

    ImGui::SameLine();

    if (ImGui::Button("Копировать"))
        ImGui::SetClipboardText(consoleText_.c_str());

    ImGui::SameLine();

    ImGui::Checkbox(
        "Автопрокрутка",
        &autoScrollConsole_
    );

    ImGui::Separator();

    ImGui::BeginChild(
        "ConsoleOutput",
        ImVec2(0.0f, 0.0f),
        false,
        ImGuiWindowFlags_HorizontalScrollbar
    );

    ImGui::TextUnformatted(
        consoleText_.data(),
        consoleText_.data() + consoleText_.size()
    );

    if (scrollConsoleToBottom_ && autoScrollConsole_)
        ImGui::SetScrollHereY(1.0f);

    scrollConsoleToBottom_ = false;

    ImGui::EndChild();
    ImGui::End();
}

void IdeApplication::FindNext()
{
    if (!activeDocument_ || searchBuffer_[0] == '\0')
        return;

    const std::string text =
        activeDocument_->editor.GetText();

    const std::string search = searchBuffer_;

    std::size_t position = text.find(
        search,
        searchOffset_
    );

    if (position == std::string::npos)
        position = text.find(search);

    if (position == std::string::npos)
        return;

    const auto start =
        OffsetToCoordinates(text, position);

    const auto end =
        OffsetToCoordinates(
            text,
            position + search.size()
        );

    activeDocument_->editor.SetSelection(
        start,
        end
    );

    activeDocument_->editor.SetCursorPosition(end);

    searchOffset_ = position + search.size();
}

void IdeApplication::ReplaceAll()
{
    if (!activeDocument_ || searchBuffer_[0] == '\0')
        return;

    std::string text =
        activeDocument_->editor.GetText();

    const std::string search = searchBuffer_;
    const std::string replacement = replaceBuffer_;

    std::size_t position = 0;
    std::size_t count = 0;

    while ((position = text.find(search, position)) !=
        std::string::npos)
    {
        text.replace(
            position,
            search.size(),
            replacement
        );

        position += replacement.size();
        ++count;
    }

    if (count > 0)
    {
        activeDocument_->editor.SetText(text);
        activeDocument_->dirty = true;
        searchOffset_ = 0;
    }
}

void IdeApplication::RenderSearch()
{
    if (!showSearch_)
        return;

    ImGui::SetNextWindowSize(
        ImVec2(440.0f, 150.0f),
        ImGuiCond_FirstUseEver
    );

    if (!ImGui::Begin(
        "Поиск и замена",
        &showSearch_))
    {
        ImGui::End();
        return;
    }

    ImGui::InputText(
        "Найти",
        searchBuffer_,
        std::size(searchBuffer_)
    );

    ImGui::InputText(
        "Заменить",
        replaceBuffer_,
        std::size(replaceBuffer_)
    );

    if (ImGui::Button("Найти далее"))
        FindNext();

    ImGui::SameLine();

    if (ImGui::Button("Заменить всё"))
        ReplaceAll();

    ImGui::End();
}

void IdeApplication::RenderStatusBar()
{
    ImGui::Begin(
        "Статус",
        nullptr,
        ImGuiWindowFlags_NoScrollbar
    );

    if (activeDocument_)
    {
        const auto position =
            activeDocument_->editor.GetCursorPosition();

        ImGui::Text(
            "Строка: %d  Столбец: %d | UTF-8 | %s",
            position.mLine + 1,
            position.mColumn + 1,
            runner_.IsRunning()
            ? "Выполняется"
            : "Готово"
        );
    }
    else
    {
        ImGui::TextDisabled("Нет открытого документа");
    }

    ImGui::End();
}

void IdeApplication::RenderAbout()
{
    if (!showAbout_)
        return;

    ImGui::OpenPopup("О LuaIDE");
    showAbout_ = false;

    if (ImGui::BeginPopupModal(
        "О LuaIDE",
        nullptr,
        ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("LuaIDE 2.0");
        ImGui::Separator();
        ImGui::TextUnformatted(
            "Dear ImGui + DirectX 11 + Lua 5.5"
        );
        ImGui::TextUnformatted(
            "Редактор Lua с вкладками и консолью."
        );

        ImGui::Spacing();

        if (ImGui::Button(
            "Закрыть",
            ImVec2(120.0f, 0.0f)))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void IdeApplication::Render()
{
    FlushConsole();
    ProcessShortcuts();

    RenderDockSpace();
    RenderToolbar();
    RenderDocuments();
    RenderProjectExplorer();
    RenderConsole();
    RenderSearch();
    RenderStatusBar();
    RenderAbout();
}

bool IdeApplication::RequestClose()
{
    if (runner_.IsRunning())
    {
        const int answer = MessageBoxW(
            nullptr,
            L"Скрипт выполняется.\n"
            L"Остановить его и закрыть LuaIDE?",
            L"LuaIDE",
            MB_YESNO | MB_ICONQUESTION
        );

        if (answer != IDYES)
            return false;

        runner_.Stop();
        runner_.Join();
    }

    bool hasDirtyDocuments = false;

    for (const auto& document : documents_)
    {
        if (document->dirty)
        {
            hasDirtyDocuments = true;
            break;
        }
    }

    if (!hasDirtyDocuments)
        return true;

    const int answer = MessageBoxW(
        nullptr,
        L"Сохранить все изменённые документы?",
        L"LuaIDE",
        MB_YESNOCANCEL | MB_ICONQUESTION
    );

    if (answer == IDCANCEL)
        return false;

    if (answer == IDNO)
        return true;

    for (auto& document : documents_)
    {
        if (document->dirty &&
            !SaveDocument(*document, false))
        {
            return false;
        }
    }

    return true;
}