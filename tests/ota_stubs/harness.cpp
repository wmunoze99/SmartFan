#include "sdk.hpp"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <string>

// Test-only access to a single finite check; never redefine access in production.
#define private public
#include "ota_manager.hpp"
#undef private
#include "firmware_variant.hpp"

namespace {
using fake::require;
using OtaStage = ::OtaStage;

const char *stage_name(OtaStage stage)
{
    switch (stage) {
    case OtaStage::idle: return "idle";
    case OtaStage::downloading: return "downloading";
    case OtaStage::verifying: return "verifying";
    case OtaStage::restarting: return "restarting";
    }
    throw std::runtime_error("unknown stage");
}

struct Fixture {
    bool ready = true;
    std::vector<OtaProgress> events;
    OtaManager manager;

    static bool is_ready(void *context) { return static_cast<Fixture *>(context)->ready; }
    static void progress(const OtaProgress &event, void *context)
    {
        auto &f = *static_cast<Fixture *>(context);
        require(f.manager.update_in_progress() == (event.stage != OtaStage::idle),
                "callback saw incorrect update flag");
        require(event.stage == OtaStage::idle || f.ready, "progress before readiness guard");
        f.events.push_back(event);
        fake::runtime.trace.push_back(std::string("progress:") + stage_name(event.stage));
    }
    static OtaManager::Config config(Fixture *self, bool callback)
    {
        // Six-field callers remain valid; progress_callback must default to nullptr.
        OtaManager::Config result{"https://example.test/smart_fan.bin", 3, 60, 7, is_ready, self};
        require(result.progress_callback == nullptr, "callback default broke old config aggregates");
        if (callback) result.progress_callback = progress;
        return result;
    }
    explicit Fixture(bool callback = true) : manager(config(this, callback))
    {
        fake::runtime = {};
        require(manager.start() == ESP_OK, "manager start failed");
        require(fake::runtime.task != nullptr, "start did not capture task");
        manager.set_network_connected(true);
    }
    esp_err_t check() { return manager.check_for_update(); }
    void task_once()
    {
        try {
            fake::runtime.task(fake::runtime.context);
            throw std::runtime_error("OTA task returned unexpectedly");
        } catch (const fake::Stop &) {}
        require(!manager.check_in_progress_.load(), "task leaked check flag");
        require(fake::runtime.waits.front() == 3 * configTICK_RATE_HZ, "startup interval changed");
    }
    void silent(esp_err_t expected, unsigned aborts)
    {
        require(check() == expected, "ordinary check changed return semantics");
        require(events.empty(), "ordinary check emitted UI progress");
        require(!manager.update_in_progress(), "ordinary check leaked update flag");
        require(fake::runtime.aborts == aborts && fake::runtime.finishes == 0 &&
                fake::runtime.restarts == 0 && !fake::runtime.alive, "ordinary check cleanup mismatch");
        require(fake::runtime.delays.empty(), "ordinary check delayed");
    }
    void downloads(std::initializer_list<uint32_t> expected, uint32_t total)
    {
        std::vector<uint32_t> actual;
        for (const auto &event : events) {
            if (event.stage == OtaStage::downloading) {
                actual.push_back(event.received_bytes);
                require(event.total_bytes == total, "download total mismatch");
            }
        }
        require(actual == std::vector<uint32_t>(expected), "download callback throttle/initial bytes mismatch");
    }
    void success(uint32_t total, bool callback = true)
    {
        auto &r = fake::runtime;
        require(check() == ESP_OK, "installation failed");
        require(!r.alive && r.finishes == 1 && r.aborts == 0 && r.restarts == 1,
                "success did not finish/restart exactly once");
        require(manager.update_in_progress(), "update guard cleared before restart");
        if (!callback) {
            require(events.empty() && r.delays.empty(), "null callback emitted progress or delayed");
            return;
        }
        require(events.size() >= 3 && events.front().stage == OtaStage::downloading,
                "missing initial download event");
        const auto &verify = events[events.size() - 2];
        const auto &restart = events.back();
        require(verify.stage == OtaStage::verifying && verify.received_bytes == total &&
                verify.total_bytes == total, "verification must report full bytes/total");
        require(restart.stage == OtaStage::restarting && restart.received_bytes == total &&
                restart.total_bytes == total, "missing restart phase");
        const auto position = [&](const char *value) {
            const auto it = std::find(r.trace.begin(), r.trace.end(), value);
            require(it != r.trace.end(), std::string("missing operation: ") + value);
            return it - r.trace.begin();
        };
        require(position("complete") < position("progress:verifying") &&
                position("progress:verifying") < position("finish") &&
                position("finish") < position("progress:restarting") &&
                position("progress:restarting") < position("delay") &&
                position("delay") < position("restart"), "phase/finish/delay/restart order changed");
        require(r.delays == std::vector<TickType_t>{pdMS_TO_TICKS(300)}, "expected one 300ms flush delay");
    }
    void failure(esp_err_t expected, unsigned aborts, unsigned finishes, bool callback = true)
    {
        auto &r = fake::runtime;
        require(check() == expected, "installation changed error semantics");
        require(!manager.update_in_progress() && !r.alive, "failure leaked update flag/handle");
        require(r.aborts == aborts && r.finishes == finishes && r.restarts == 0 && r.delays.empty(),
                "failure cleanup/reboot mismatch");
        if (callback) {
            require(events.size() >= 2 && events.front().stage == OtaStage::downloading,
                    "failure script did not start UI");
            const auto &last = events.back();
            require(last.stage == OtaStage::idle && last.received_bytes == 0 && last.total_bytes == 0,
                    "failure did not emit idle reset");
            require(std::none_of(events.begin(), events.end(), [](const auto &event) {
                return event.stage == OtaStage::restarting;
            }), "failure emitted restarting");
            const auto verifies = std::count_if(events.begin(), events.end(), [](const auto &event) {
                return event.stage == OtaStage::verifying;
            });
            require(verifies == finishes, "verification emitted before download completed");
        } else {
            require(events.empty(), "null callback unexpectedly invoked");
        }
    }
};

void scenario(const std::string &name)
{
    if (name == "handle_guard") {
        for (bool finish : {false, true}) {
            fake::runtime = {};
            fake::runtime.alive = true;
            if (finish) esp_https_ota_finish(&fake::runtime);
            else esp_https_ota_abort(&fake::runtime);
            bool rejected = false;
            try { esp_https_ota_get_image_size(&fake::runtime); }
            catch (const std::runtime_error &) { rejected = true; }
            require(rejected, "stub failed to detect released handle use");
        }
        return;
    }
    Fixture f(name != "null_success" && name != "null_failure");
    auto &r = fake::runtime;
    if (name == "current" || name == "older") {
        std::strcpy(r.remote.version, name == "current" ? "1.0.0" : "0.9.9");
        f.silent(ESP_OK, 1);
    } else if (name == "begin_failure") {
        r.begin_result = ESP_FAIL;
        f.silent(ESP_FAIL, 0);
    } else if (name == "header_failure") {
        r.header_result = ESP_FAIL;
        f.silent(ESP_FAIL, 1);
    } else if (name == "invalid_remote" || name == "invalid_current") {
        std::strcpy(name == "invalid_remote" ? r.remote.version : r.current.version, "not-a-version");
        f.silent(ESP_ERR_INVALID_VERSION, 1);
    } else if (name == "not_ready") {
        f.ready = false;
        f.silent(ESP_ERR_INVALID_STATE, 1);
    } else if (name == "task_not_ready" || name == "task_current") {
        f.ready = name != "task_not_ready";
        std::strcpy(r.remote.version, "1.0.0");
        f.task_once();
        require(f.events.empty() && !f.manager.update_in_progress(), "ordinary task emitted progress");
        require(r.aborts == (f.ready ? 1U : 0U), "task readiness guard changed");
        require(r.trace.empty() == !f.ready, "not-ready task attempted network OTA");
        require(r.waits.back() == (f.ready ? 60U : 7U) * configTICK_RATE_HZ, "task interval changed");
    } else if (name == "known") {
        r.steps = {{999, ESP_ERR_HTTPS_OTA_IN_PROGRESS}, {1000, ESP_ERR_HTTPS_OTA_IN_PROGRESS},
                   {1999, ESP_ERR_HTTPS_OTA_IN_PROGRESS}, {2000, ESP_ERR_HTTPS_OTA_IN_PROGRESS},
                   {100000, ESP_OK}};
        f.success(100000);
        f.downloads({512, 1000, 2000, 100000}, 100000);
        require(f.events.size() == 6, "100% downloading must remain distinct from verifying");
    } else if (name == "large") {
        r.descriptor_bytes = 1000000000;
        r.total = 2000000000;
        r.steps = {{1019999999, ESP_ERR_HTTPS_OTA_IN_PROGRESS}, {1020000000, ESP_ERR_HTTPS_OTA_IN_PROGRESS},
                   {1999999999, ESP_ERR_HTTPS_OTA_IN_PROGRESS}, {2000000000, ESP_ERR_HTTPS_OTA_IN_PROGRESS},
                   {2147483647, ESP_OK}};
        f.success(2000000000);
        f.downloads({1000000000, 1020000000, 1999999999, 2000000000}, 2000000000);
    } else if (name == "chunked") {
        r.total = -1;
        r.steps = {{16895, ESP_ERR_HTTPS_OTA_IN_PROGRESS}, {16896, ESP_ERR_HTTPS_OTA_IN_PROGRESS},
                   {33279, ESP_ERR_HTTPS_OTA_IN_PROGRESS}, {33280, ESP_ERR_HTTPS_OTA_IN_PROGRESS},
                   {40000, ESP_OK}};
        f.success(40000);
        f.downloads({512, 16896, 33280}, 0);
    } else if (name == "zero_total" || name == "negative_total") {
        r.total = name == "zero_total" ? 0 : -17;
        f.success(100000);
        f.downloads({512, 100000}, 0);
    } else if (name == "negative_received") {
        r.descriptor_bytes = -1;
        r.steps = {{-17, ESP_ERR_HTTPS_OTA_IN_PROGRESS}, {0, ESP_ERR_HTTPS_OTA_IN_PROGRESS}, {100000, ESP_OK}};
        f.success(100000);
        f.downloads({0, 100000}, 100000);
    } else if (name == "perform_failure" || name == "null_failure" || name == "retry") {
        r.steps = {{1000, ESP_ERR_HTTPS_OTA_IN_PROGRESS}, {2000, ESP_FAIL}};
        f.failure(ESP_FAIL, 1, 0, name != "null_failure");
        if (name == "retry") {
            r = {};
            f.events.clear();
            f.success(100000);
        }
    } else if (name == "incomplete") {
        r.complete = false;
        f.failure(ESP_ERR_INVALID_SIZE, 1, 0);
    } else if (name == "finish_failure") {
        r.finish_result = ESP_ERR_INVALID_VERSION;
        f.failure(ESP_ERR_INVALID_VERSION, 0, 1);
        const auto verify = std::find(r.trace.begin(), r.trace.end(), "progress:verifying");
        const auto finish = std::find(r.trace.begin(), r.trace.end(), "finish");
        const auto idle = std::find(r.trace.begin(), r.trace.end(), "progress:idle");
        require(verify < finish && finish < idle, "failed finish phase ordering mismatch");
    } else if (name == "null_success") {
        f.success(100000, false);
    } else if (name == "manual") {
        f.manager.set_network_connected(false);
        require(f.manager.request_check() == ESP_ERR_INVALID_STATE, "offline manual check allowed");
        f.manager.set_network_connected(true);
        f.ready = false;
        require(f.manager.request_check() == ESP_ERR_INVALID_STATE, "fan-on manual check allowed");
        f.ready = true;
        f.manager.check_in_progress_.store(true);
        require(f.manager.request_check() == ESP_ERR_INVALID_STATE, "duplicate manual check allowed");
        f.manager.check_in_progress_.store(false);
        f.manager.update_in_progress_.store(true);
        require(f.manager.request_check() == ESP_ERR_INVALID_STATE, "manual check allowed while updating");
        f.manager.update_in_progress_.store(false);
        require(f.manager.request_check() == ESP_OK && r.notify_gives == 1 && f.events.empty(),
                "manual request must notify task without emitting UI");
        require(f.manager.start() == ESP_ERR_INVALID_STATE, "duplicate start allowed");
        std::strcpy(r.remote.version, "1.0.0");
        f.silent(ESP_OK, 1);
        require(r.url == std::string("https://example.test/smart_fan-") + firmware::kDisplayVariant + ".bin",
                "variant URL rewrite lost");
    } else if (name == "rollback") {
        require(f.manager.confirm_running_image() == ESP_OK && r.confirmations == 0, "valid image reconfirmed");
        r.image_state = ESP_OTA_IMG_PENDING_VERIFY;
        r.confirm_result = ESP_FAIL;
        require(f.manager.confirm_running_image() == ESP_FAIL && r.confirmations == 1,
                "rollback confirmation result lost");
        r.state_result = ESP_FAIL;
        require(f.manager.confirm_running_image() == ESP_OK && r.confirmations == 1,
                "partition-state error semantics changed");
        require(f.events.empty() && r.trace.empty(), "rollback invoked progress/network");
    } else {
        throw std::runtime_error("unknown scenario: " + name);
    }
}
} // namespace

int main(int argc, char **argv)
{
    try {
        require(argc == 2, "usage: ota_manager_test SCENARIO");
        scenario(argv[1]);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "OTA harness: " << error.what() << '\n';
        return 1;
    }
}
