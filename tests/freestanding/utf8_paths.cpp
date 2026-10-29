// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "core/codec/LuaCodec.h"
#include "core/config/ConfigLoader.h"
#include "core/dp/PortRef.h"
#include "core/log/Sinks.h"

namespace {
namespace fs = std::filesystem;

std::string utf8(std::u8string_view text){
    return {text.begin(), text.end()};
}

std::string utf8Path(fs::path const& path){
    return utf8(path.u8string());
}

void require(bool condition, char const* message){
    if(!condition) throw std::runtime_error(message);
}

void writeFile(fs::path const& path, std::string const& content){
    std::ofstream file(path, std::ios::binary);
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    file.close();
    require(bool(file), "cannot write fixture");
}

std::string readFile(fs::path const& path){
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "expected file does not exist at its Unicode path");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void checkConfig(fs::path const& root){
    auto const path = root / u8"中文配置🚧.toml";
    writeFile(path, utf8(u8"[meta]\nproject = '现场配置'\n"));
    core::config::ConfigLoader loader;
    auto result = loader.loadFromToml(utf8Path(path));
    require(result && result->meta.project == utf8(u8"现场配置"), "Unicode config load failed");
    auto missing = loader.loadFromToml(utf8Path(root / u8"不存在.toml"));
    require(!missing && missing.error().front().field == "path", "missing config error lost");
    writeFile(path, "[meta\n");
    auto invalid = loader.loadFromToml(utf8Path(path));
    require(!invalid && invalid.error().front().field == "toml"
                    && invalid.error().front().lineNumber > 0, "TOML error location lost");
}

void checkLogging(fs::path const& root){
    auto const path = root / u8"嵌套日志目录" / u8"运行🚧.log";
    auto archive = [&](int index){
        auto result = path;
        result += "." + std::to_string(index);
        return result;
    };
    core::log::LogRecord record;
    {
        core::log::RollingFileSink sink(utf8Path(path), 1, 2);
        for(int i = 1; i <= 3; ++i){
            record.message = utf8(u8"中文日志") + std::to_string(i);
            sink.write(record);
        }
        sink.flush();
    }
    require(fs::exists(path), "Unicode active log missing");
    require(readFile(archive(1)).find(utf8(u8"中文日志3")) != std::string::npos, "latest Unicode archive wrong");
    require(readFile(archive(2)).find(utf8(u8"中文日志2")) != std::string::npos, "older Unicode archive wrong");
    require(!fs::exists(archive(3)), "archive retention exceeded");
    for(int i = 4; i <= 5; ++i){
        core::log::RollingFileSink sink(utf8Path(path), 0, 2);
        record.message = utf8(u8"追加日志") + std::to_string(i);
        sink.write(record);
    }
    auto const appended = readFile(path);
    require(appended.find(utf8(u8"追加日志4")) != std::string::npos
         && appended.find(utf8(u8"追加日志5")) != std::string::npos, "Unicode log append failed");
}

#ifdef CORE_TEST_HAS_LUA
void checkLua(fs::path const& root){
    auto const path = root / u8"中文脚本🚧.lua";
    std::string const source =
        "return { decode = function(raw, ctx) return raw[1] + 7 end,"
        "encode = function(value, ctx) return { value - 7 } end }";
    core::dp::PortRef ref;
    for(std::string const prefix : {std::string{}, std::string{"\xEF\xBB\xBF"},
                                   std::string{"#!/usr/bin/lua\n"}, std::string{"\xEF\xBB\xBF#!/usr/bin/lua\n"}}){
        writeFile(path, prefix + source);
        std::string error;
        auto codec = core::codec::LuaCodec::fromFile("unicode", utf8Path(path), {}, &error);
        require(bool(codec), "Unicode Lua script load failed");
        require(core::dp::toInt64(codec->decode({5}, ref)) == 12, "Lua decode changed");
        require(codec->encode(std::int64_t{12}, ref) == core::RegisterWords{5}, "Lua encode changed");
    }
    // luaL_loadfile accepted binary chunks too; preserve that behavior when
    // switching from the narrow file API to native-path reads.
    writeFile(path,
        "return { decode = function() return string.dump(function() " + source +
        " end) end, encode = function() return {} end }");
    std::string error;
    auto dumper = core::codec::LuaCodec::fromFile("dump", utf8Path(path), {}, &error);
    require(bool(dumper), "Lua bytecode fixture failed");
    auto const bytecode = std::get<std::string>(dumper->decode({}, ref));
    writeFile(path, std::string{"\xEF\xBB\xBF#!/usr/bin/lua\n"} + bytecode);
    auto compiled = core::codec::LuaCodec::fromFile("binary", utf8Path(path), {}, &error);
    require(compiled && core::dp::toInt64(compiled->decode({5}, ref)) == 12, "Lua bytecode load changed");
    writeFile(path, "#!/usr/bin/lua\n-- comment\n@\n");
    auto invalid = core::codec::LuaCodec::fromFile("invalid", utf8Path(path), {}, &error);
    require(!invalid && error.find(":3:") != std::string::npos, "Lua error line number changed");
    auto missing = core::codec::LuaCodec::fromFile("missing", utf8Path(root / u8"不存在.lua"), {}, &error);
    require(!missing && !error.empty(), "missing Lua error lost");
}
#endif

} // namespace

int main(){
    try {
        auto temp = fs::absolute(fs::temp_directory_path()).lexically_normal();
        if(temp.filename().empty()) temp = temp.parent_path();
        auto const root = temp / ("core-utf8-paths-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(root.is_absolute() && root.parent_path() == temp, "fixture must stay in TEMP");
        require(fs::create_directory(root), "fixture directory must be new");
        auto const folder = root / u8"现场目录🚧";
        fs::create_directory(folder);
        checkConfig(folder);
        checkLogging(folder);
#ifdef CORE_TEST_HAS_LUA
        checkLua(folder);
#endif
        // Only delete the absolute TEMP child this test successfully created.
        fs::remove_all(root);
        unsigned int acp = 0;
#ifdef _WIN32
        acp = GetACP();
#endif
        std::cout << "{\"acp\":" << acp << ",\"config\":true,\"rollingLog\":true,\"lua\":";
#ifdef CORE_TEST_HAS_LUA
        std::cout << "true";
#else
        std::cout << "null";
#endif
        std::cout << "}\n";
        return 0;
    } catch(std::exception const& error){
        std::cerr << error.what() << '\n';
        return 1;
    }
}
