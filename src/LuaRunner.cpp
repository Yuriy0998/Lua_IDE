#include "LuaRunner.hpp"

#include <utility>

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace
{
    struct RunnerContext
    {
        LuaRunner* runner = nullptr;
        LuaRunner::OutputCallback* output = nullptr;

        const std::string* source = nullptr;
        const std::string* chunkName = nullptr;
        const std::string* moduleDirectory = nullptr;

        std::atomic<bool>* stopRequested = nullptr;
    };

    RunnerContext* GetContext(lua_State* state)
    {
        return *static_cast<RunnerContext**>(
            lua_getextraspace(state)
            );
    }

    void Emit(
        RunnerContext* context,
        const char* text,
        std::size_t size) noexcept
    {
        if (!context || !context->output || !text || size == 0)
            return;

        try
        {
            (*context->output)(text, size);
        }
        catch (...)
        {
            // Исключения не должны проходить через Lua C API.
        }
    }

    int LuaPrint(lua_State* state)
    {
        const int count = lua_gettop(state);

        luaL_Buffer buffer;
        luaL_buffinit(state, &buffer);

        for (int i = 1; i <= count; ++i)
        {
            if (i > 1)
                luaL_addchar(&buffer, '\t');

            std::size_t length = 0;
            const char* value = luaL_tolstring(state, i, &length);

            luaL_addlstring(&buffer, value, length);
            lua_pop(state, 1);
        }

        luaL_addchar(&buffer, '\n');
        luaL_pushresult(&buffer);

        std::size_t length = 0;
        const char* result = lua_tolstring(state, -1, &length);

        Emit(GetContext(state), result, length);
        return 0;
    }

    int BlockExit(lua_State* state)
    {
        return luaL_error(
            state,
            "os.exit is disabled inside LuaIDE."
        );
    }

    void StopHook(lua_State* state, lua_Debug*)
    {
        RunnerContext* context = GetContext(state);

        if (context &&
            context->stopRequested &&
            context->stopRequested->load(
                std::memory_order_relaxed))
        {
            luaL_error(state, "Execution stopped by user.");
        }
    }

    int Traceback(lua_State* state)
    {
        lua_sethook(state, nullptr, 0, 0);

        const char* message = lua_tostring(state, 1);

        luaL_traceback(
            state,
            state,
            message ? message : "Unknown Lua error",
            1
        );

        return 1;
    }

    int ExecuteChunk(lua_State* state)
    {
        RunnerContext* context = static_cast<RunnerContext*>(
            lua_touserdata(state, 1)
            );

        lua_settop(state, 0);
        luaL_openlibs(state);

        lua_pushcfunction(state, LuaPrint);
        lua_setglobal(state, "print");

        lua_getglobal(state, "os");

        if (lua_istable(state, -1))
        {
            lua_pushcfunction(state, BlockExit);
            lua_setfield(state, -2, "exit");
        }

        lua_pop(state, 1);

        // Не позволяем выполняемому скрипту отключить StopHook.
        lua_getglobal(state, "debug");

        if (lua_istable(state, -1))
        {
            lua_pushnil(state);
            lua_setfield(state, -2, "sethook");
        }

        lua_pop(state, 1);

        if (!context->moduleDirectory->empty())
        {
            lua_getglobal(state, "package");
            lua_getfield(state, -1, "path");

            const char* oldPath = lua_tostring(state, -1);

            lua_pushfstring(
                state,
                "%s/?.lua;%s/?/init.lua;%s",
                context->moduleDirectory->c_str(),
                context->moduleDirectory->c_str(),
                oldPath ? oldPath : ""
            );

            lua_setfield(state, -3, "path");
            lua_pop(state, 2);
        }

        lua_sethook(state, StopHook, LUA_MASKCOUNT, 1000);

        const int loadResult = luaL_loadbufferx(
            state,
            context->source->data(),
            context->source->size(),
            context->chunkName->c_str(),
            "t"
        );

        if (loadResult != LUA_OK)
            return lua_error(state);

        lua_call(state, 0, 0);
        return 0;
    }
}

LuaRunner::~LuaRunner()
{
    Stop();
    Join();
}

bool LuaRunner::Run(
    std::string source,
    std::string chunkName,
    std::string moduleDirectory,
    OutputCallback outputCallback,
    FinishedCallback finishedCallback)
{
    if (running_.exchange(true))
        return false;

    Join();

    stopRequested_.store(false, std::memory_order_relaxed);

    try
    {
        worker_ = std::thread(
            [
                this,
                source = std::move(source),
                chunkName = std::move(chunkName),
                moduleDirectory = std::move(moduleDirectory),
                output = std::move(outputCallback),
                finished = std::move(finishedCallback)
            ]() mutable
            {
                bool success = false;

                lua_State* state = luaL_newstate();

                if (!state)
                {
                    static constexpr char message[] =
                        "Cannot create Lua state.\n";

                    if (output)
                        output(message, sizeof(message) - 1);
                }
                else
                {
                    RunnerContext context;
                    context.runner = this;
                    context.output = &output;
                    context.source = &source;
                    context.chunkName = &chunkName;
                    context.moduleDirectory = &moduleDirectory;
                    context.stopRequested = &stopRequested_;

                    *static_cast<RunnerContext**>(
                        lua_getextraspace(state)
                        ) = &context;

                    lua_pushcfunction(state, Traceback);
                    const int tracebackIndex = lua_gettop(state);

                    lua_pushcfunction(state, ExecuteChunk);
                    lua_pushlightuserdata(state, &context);

                    const int result = lua_pcall(
                        state,
                        1,
                        0,
                        tracebackIndex
                    );

                    lua_sethook(state, nullptr, 0, 0);

                    if (result == LUA_OK)
                    {
                        success = true;

                        static constexpr char message[] =
                            "--- Finished ---\n";

                        Emit(
                            &context,
                            message,
                            sizeof(message) - 1
                        );
                    }
                    else
                    {
                        std::size_t length = 0;
                        const char* error = lua_tolstring(
                            state,
                            -1,
                            &length
                        );

                        if (error)
                            Emit(&context, error, length);

                        Emit(&context, "\n--- Stopped/error ---\n", 23);
                    }

                    lua_close(state);
                }

                running_.store(false, std::memory_order_release);

                try
                {
                    if (finished)
                        finished(success);
                }
                catch (...)
                {
                }
            }
        );
    }
    catch (...)
    {
        running_.store(false, std::memory_order_release);
        throw;
    }

    return true;
}

void LuaRunner::Stop() noexcept
{
    stopRequested_.store(true, std::memory_order_relaxed);
}

void LuaRunner::Join()
{
    if (worker_.joinable())
        worker_.join();
}

bool LuaRunner::IsRunning() const noexcept
{
    return running_.load(std::memory_order_acquire);
}