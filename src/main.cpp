#include "config.hpp"
#include "dispatcher.hpp"
#include "history.hpp"
#include "http_server.hpp"
#include "inverter_model.hpp"
#include "inverter_state.hpp"
#include "logger.hpp"
#include "poller.hpp"

#include <spdlog/spdlog.h>

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [-D level] [-d db_path]\n"
              << "  -Dx   log verbosity 1..5 (1=error 2=warning 3=info "
                 "4=debug 5=trace)\n"
              << "  -dxxx override the telemetry history SQLite path\n";
}

constexpr const char* kDefaultConfigPath = "solarman_bridge.conf";

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

    spdlog::info("solarman_bridge starting; logger {}:{} serial {}, control {}",
                 cfg.logger.host, cfg.logger.port, cfg.logger.serial,
                 cfg.control.enabled ? "enabled" : "disabled");

    // Dispatcher first: it blocks SIGINT/SIGTERM for the whole process
    // before any worker thread is spawned, so only its signalfd sees them.
    Dispatcher    dispatcher;
    History       history(cfg.history.db_path, cfg.history.retention_days);
    InverterState state;

    // The poller runs on the dispatcher thread, so History is written only
    // from here; the HTTP thread reads it through its own connection.
    // load_config() has checked that the model exists.
    const InverterModel& model = *find_model(cfg.logger.model);
    Poller     poller(dispatcher, cfg.logger, model, state, history,
                      std::chrono::milliseconds(cfg.history.sample_period_ms));
    HttpServer http_server(history, state, model, cfg.logger, cfg.http,
                           cfg.control);
    http_server.start();

    dispatcher.run();

    spdlog::info("solarman_bridge stopping");
    http_server.stop();
    return EXIT_SUCCESS;
}
