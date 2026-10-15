#include "config.hpp"
#include "dispatcher.hpp"
#include "history.hpp"
#include "http_server.hpp"
#include "inverter_model.hpp"
#include "inverter_state.hpp"
#include "logger.hpp"
#include "poller.hpp"
#include "v5_client.hpp"

#include <spdlog/spdlog.h>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <string>

namespace {

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [-D level] [-d db_path]\n"
              << "  -Dx   log verbosity 1..5 (1=error 2=warning 3=info "
                 "4=debug 5=trace)\n"
              << "  -dxxx override the telemetry history SQLite path\n";
}

constexpr const char* kDefaultConfigPath = "solarman_bridge.conf";

// Requests a logger's queue holds per inverter behind it: a few cycles. A
// queue that keeps growing means the poll period is shorter than the link
// needs, and the excess is dropped.
constexpr size_t kQueuePerInverter = 16;

}  // namespace

int main(int argc, char** argv) {
    std::string cli_db_path;
    int cli_log_level = -1;

    int opt;
    while ((opt = getopt(argc, argv, "D:d:h")) != -1) {
        switch (opt) {
            case 'D':
                cli_log_level = std::atoi(optarg);
                if (cli_log_level < 1 || cli_log_level > 5) {
                    std::cerr << "log level must be in [1..5]\n";
                    return EXIT_FAILURE;
                }
                break;
            case 'd': cli_db_path = optarg; break;
            case 'h':
            default:
                print_usage(argv[0]);
                return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    const char* env_path = std::getenv("SOLARMAN_BRIDGE_CONFIG");
    std::string cfg_path = env_path ? env_path : kDefaultConfigPath;

    AppConfig cfg;
    std::string err;
    if (!load_config(cfg_path, cfg, err)) {
        std::cerr << "config error: " << err << "\n";
        return EXIT_FAILURE;
    }

    if (cli_log_level > 0)    cfg.log_level       = cli_log_level;
    if (!cli_db_path.empty()) cfg.history.db_path = cli_db_path;

    init_logger(cfg.log_level, cfg.log_file);

    spdlog::info("solarman_bridge starting; {} inverter(s), control {}",
                 cfg.inverters.size(), cfg.control.enabled ? "enabled" : "disabled");

    // Dispatcher first: it blocks SIGINT/SIGTERM for the whole process
    // before any worker thread is spawned, so only its signalfd sees them.
    Dispatcher dispatcher;
    History    history(cfg.history.db_path, cfg.history.retention_days);

    // One V5 client per logger, shared by the inverters behind it; their
    // queue has room for a few cycles of each.
    std::map<std::string, const LoggerConfig*> loggers;
    std::map<std::string, std::unique_ptr<V5Client>> clients;
    for (const LoggerConfig& l : cfg.loggers) loggers[l.name] = &l;
    for (const InverterConfig& inv : cfg.inverters) {
        auto& c = clients[inv.logger];
        if (c) continue;
        const LoggerConfig& l = *loggers.at(inv.logger);
        const size_t users = std::count_if(cfg.inverters.begin(), cfg.inverters.end(),
            [&](const InverterConfig& i) { return i.logger == l.name; });
        c = std::make_unique<V5Client>(dispatcher, l.host, l.port, l.serial,
                                       std::chrono::milliseconds(l.timeout_ms),
                                       std::chrono::milliseconds(l.reconnect_ms),
                                       kQueuePerInverter * users);
    }

    // The pollers run on the dispatcher thread, so History is written only
    // from here; the HTTP thread reads it through its own connection.
    // load_config() has checked the models and the logger references.
    std::deque<InverterState>        states;
    std::vector<std::unique_ptr<Poller>> pollers;
    std::vector<InverterView>        views;
    for (const InverterConfig& inv : cfg.inverters) {
        const InverterModel& model = *find_model(inv.model);
        InverterState& state = states.emplace_back();
        const int id = history.inverter_id(inv.name);
        pollers.push_back(std::make_unique<Poller>(
            dispatcher, *clients.at(inv.logger), inv, model, state, history, id,
            std::chrono::milliseconds(cfg.history.sample_period_ms)));
        views.push_back({inv, *loggers.at(inv.logger), model, state, id});
    }

    HttpServer http_server(history, std::move(views), cfg.http, cfg.control);
    http_server.start();

    dispatcher.run();

    spdlog::info("solarman_bridge stopping");
    http_server.stop();
    return EXIT_SUCCESS;
}
