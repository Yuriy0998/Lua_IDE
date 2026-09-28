#pragma once

#include "LuaRunner.hpp"
#include "TextEditor.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class IdeApplication
{
public:
    IdeApplication();
    ~IdeApplication();

    IdeApplication(const IdeApplication&) = delete;
    IdeApplication& operator=(const IdeApplication&) = delete;

    void Render();

    // Возвращает true, если приложение можно закрыть.
    bool RequestClose();

private:
    struct Document
    {
        std::filesystem::path path;
        std::string name = "Без имени.lua";

        TextEditor editor;

        bool dirty = false;
        bool requestSelect = false;
    };

    std::vector<std::unique_ptr<Document>> documents_;
    Document* activeDocument_ = nullptr;

    std::filesystem::path projectDirectory_;

    LuaRunner runner_;

    std::mutex consoleMutex_;
    std::string pendingConsole_;
    std::string consoleText_;

    bool autoScrollConsole_ = true;
    bool scrollConsoleToBottom_ = false;

    bool showProject_ = true;
    bool showConsole_ = true;
    bool showSearch_ = false;
    bool showAbout_ = false;

    char searchBuffer_[256]{};
    char replaceBuffer_[256]{};

    std::size_t searchOffset_ = 0;

    void NewDocument();
    void OpenDocumentDialog();
    void OpenDocument(const std::filesystem::path& path);
    void OpenProjectDialog();

    bool SaveDocument(Document& document, bool saveAs);
    bool ConfirmCloseDocument(Document& document);
    void CloseDocument(Document& document);

    void RunDocument();
    void StopDocument();

    void QueueConsole(const char* text, std::size_t size);
    void FlushConsole();

    void RenderDockSpace();
    void RenderMainMenu();
    void RenderToolbar();
    void RenderDocuments();
    void RenderProjectExplorer();
    void RenderProjectDirectory(
        const std::filesystem::path& directory
    );
    void RenderConsole();
    void RenderSearch();
    void RenderStatusBar();
    void RenderAbout();

    void ProcessShortcuts();

    void FindNext();
    void ReplaceAll();

    static std::string ReadFile(
        const std::filesystem::path& path
    );

    static void WriteFile(
        const std::filesystem::path& path,
        const std::string& text
    );
};