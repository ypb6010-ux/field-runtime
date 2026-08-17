// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "DriverRegistry.h"

#include <array>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#include "core/driver/DriverApi.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace core::gateway {
namespace {

using LibraryHandle =
#if defined(_WIN32)
    HMODULE;
#else
    void*;
#endif

LibraryHandle openLibrary(std::string const& path, std::string& error) {
#if defined(_WIN32)
    auto handle = LoadLibraryA(path.c_str());
    if (!handle) error = "LoadLibrary failed with code " + std::to_string(GetLastError());
    return handle;
#else
    auto handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        auto const* detail = dlerror();
        error = detail ? detail : "dlopen failed";
    }
    return handle;
#endif
}

void closeLibrary(LibraryHandle handle) {
    if (!handle) return;
#if defined(_WIN32)
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif
}

void* findSymbol(LibraryHandle handle, char const* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(handle, name));
#else
    return dlsym(handle, name);
#endif
}

} // namespace

class DriverRegistry::Impl {
public:
    struct Entry {
        Impl* owner = nullptr;
        std::string id;
        std::string library;
        std::string config;
        LibraryHandle handle = nullptr;
        FieldRuntimeDriverApiV1 const* api = nullptr;
        FieldRuntimeDriverHostV1 host{};
        void* instance = nullptr;
        std::string state = "loaded";
        std::string error;
        std::thread worker;
        std::mutex mutex;
        std::condition_variable condition;
        std::deque<std::function<void()>> tasks;
        bool stopping = false;

        void post(std::function<void()> task) {
            {
                std::lock_guard lock(mutex);
                tasks.push_back(std::move(task));
            }
            condition.notify_one();
        }

        void run() {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock lock(mutex);
                    condition.wait(lock, [&] { return stopping || !tasks.empty(); });
                    if (stopping && tasks.empty()) return;
                    task = std::move(tasks.front());
                    tasks.pop_front();
                }
                task();
            }
        }
    };

    static void publish(void* context, char const* deviceId,
                        char const* targetId, std::uint8_t const* data,
                        std::size_t size) {
        auto* entry = static_cast<Entry*>(context);
        if (!entry || !entry->owner->dataCallback) return;
        std::vector<std::uint8_t> copy;
        if (data && size) copy.assign(data, data + size);
        entry->owner->dataCallback(entry->id, deviceId ? deviceId : "",
                                   targetId ? targetId : "", std::move(copy));
    }

    static void log(void* context, int level, char const* message) {
        auto* entry = static_cast<Entry*>(context);
        if (entry && entry->owner->logCallback) {
            entry->owner->logCallback(entry->id, level, message ? message : "");
        }
    }

    void stopEntry(Entry& entry) {
        if (entry.worker.joinable()) {
            auto stopped = std::make_shared<std::promise<void>>();
            auto future = stopped->get_future();
            entry.post([&entry, stopped] {
                if (entry.instance && entry.api && entry.api->stop) {
                    entry.api->stop(entry.instance);
                }
                if (entry.instance && entry.api && entry.api->destroy) {
                    entry.api->destroy(entry.instance);
                    entry.instance = nullptr;
                }
                stopped->set_value();
            });
            future.wait();
            {
                std::lock_guard lock(entry.mutex);
                entry.stopping = true;
            }
            entry.condition.notify_one();
            entry.worker.join();
        }
        closeLibrary(entry.handle);
        entry.handle = nullptr;
        entry.state = "stopped";
    }

    std::map<std::string, std::unique_ptr<Entry>> entries;
    DataCallback dataCallback;
    LogCallback logCallback;
};

DriverRegistry::DriverRegistry() : m_impl(std::make_unique<Impl>()) {}
DriverRegistry::~DriverRegistry() { stop(); }

void DriverRegistry::setDataCallback(DataCallback callback) {
    m_impl->dataCallback = std::move(callback);
}

void DriverRegistry::setLogCallback(LogCallback callback) {
    m_impl->logCallback = std::move(callback);
}

bool DriverRegistry::load(std::vector<config::DriverConfig> const& drivers,
                          std::string const& configDirectory,
                          std::string& error) {
    stop();
    for (auto const& config : drivers) {
        if (!config.enabled) continue;
        auto entry = std::make_unique<Impl::Entry>();
        entry->owner = m_impl.get();
        entry->id = config.id;
        entry->config = config.config;
        auto const* configuredRoot = std::getenv("FIELDRUNTIME_DRIVER_DIR");
        auto const root = configuredRoot && *configuredRoot
            ? std::filesystem::path(configuredRoot)
            : std::filesystem::path(configDirectory) / "drivers";
        auto path = root / config.library;
        entry->library = std::filesystem::absolute(path).lexically_normal().string();
        entry->handle = openLibrary(entry->library, entry->error);
        if (!entry->handle) {
            error = "driver '" + entry->id + "': " + entry->error;
            return false;
        }
        auto getApi = reinterpret_cast<FieldRuntimeDriverGetApiFn>(
            findSymbol(entry->handle, "fieldRuntimeDriverGetApi"));
        if (!getApi) {
            error = "driver '" + entry->id + "' has no fieldRuntimeDriverGetApi";
            closeLibrary(entry->handle);
            return false;
        }
        entry->api = getApi(FIELD_RUNTIME_DRIVER_ABI_V1);
        if (!entry->api || entry->api->abi_version != FIELD_RUNTIME_DRIVER_ABI_V1
            || entry->api->struct_size < sizeof(FieldRuntimeDriverApiV1)
            || !entry->api->create || !entry->api->destroy
            || !entry->api->start || !entry->api->stop || !entry->api->write) {
            error = "driver '" + entry->id + "' returned an incompatible API";
            closeLibrary(entry->handle);
            return false;
        }
        if (entry->api->driver_id && entry->id != entry->api->driver_id) {
            error = "configured driver id does not match adapter id '"
                  + std::string(entry->api->driver_id) + "'";
            closeLibrary(entry->handle);
            return false;
        }
        entry->host = {entry.get(), &Impl::publish, &Impl::log};
        entry->worker = std::thread([ptr = entry.get()] { ptr->run(); });
        m_impl->entries.emplace(entry->id, std::move(entry));
    }
    return true;
}

void DriverRegistry::start() {
    for (auto& [id, entry] : m_impl->entries) {
        (void)id;
        entry->post([ptr = entry.get()] {
            std::array<char, 512> error{};
            ptr->instance = ptr->api->create(ptr->config.c_str(), &ptr->host,
                                             error.data(), error.size());
            if (!ptr->instance) {
                std::lock_guard lock(ptr->mutex);
                ptr->state = "error";
                ptr->error = error.data();
                return;
            }
            if (!ptr->api->start(ptr->instance, error.data(), error.size())) {
                std::lock_guard lock(ptr->mutex);
                ptr->state = "error";
                ptr->error = error.data();
                return;
            }
            {
                std::lock_guard lock(ptr->mutex);
                ptr->state = "running";
                ptr->error.clear();
            }
        });
    }
}

void DriverRegistry::stop() {
    for (auto& [id, entry] : m_impl->entries) {
        (void)id;
        m_impl->stopEntry(*entry);
    }
    m_impl->entries.clear();
}

bool DriverRegistry::write(std::string const& driverId,
                           std::string deviceId, std::string targetId,
                           std::vector<std::uint8_t> data,
                           WriteCallback done) {
    auto const it = m_impl->entries.find(driverId);
    if (it == m_impl->entries.end()) return false;
    auto* entry = it->second.get();
    entry->post([entry, deviceId = std::move(deviceId),
                 targetId = std::move(targetId), data = std::move(data),
                 done = std::move(done)]() mutable {
        std::array<char, 512> error{};
        bool running = false;
        {
            std::lock_guard lock(entry->mutex);
            running = entry->state == "running";
        }
        auto const ok = entry->instance && running
            && entry->api->write(entry->instance, deviceId.c_str(),
                                 targetId.c_str(), data.data(), data.size(),
                                 error.data(), error.size());
        if (done) done(ok != 0, ok ? std::string{} : std::string(error.data()));
    });
    return true;
}

std::vector<DriverSnapshot> DriverRegistry::snapshots() const {
    std::vector<DriverSnapshot> out;
    for (auto const& [id, entry] : m_impl->entries) {
        std::lock_guard lock(entry->mutex);
        out.push_back({id, entry->library, entry->state, entry->error});
    }
    return out;
}

} // namespace core::gateway
