#include "Configuration.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <iostream>

#include "json/json.h"

#include "AggregateEventStatistics.hpp"
#include "CommandLineConfiguration.hpp"
#include "EventDispatcher.hpp"
#include "EventStatistics.hpp"
#include "IndividualEventStatistics.hpp"
#include "NullEventStatistics.hpp"
#include "Partitioner.hpp"
#include "STLLTSFQueue.hpp"
#include "ProfileGuidedPartitioner.hpp"
#include "RoundRobinPartitioner.hpp"
#include "SequentialEventDispatcher.hpp"
#include "TimeWarpMatternGVTManager.hpp"
#include "TimeWarpLocalGVTManager.hpp"
#include "TimeWarpEventDispatcher.hpp"
#include "utility/memory.hpp"
#include "TimeWarpMPICommunicationManager.hpp"
#include "TimeWarpPeriodicStateManager.hpp"
#include "TimeWarpAggressiveOutputManager.hpp"
#include "TimeWarpFileStreamManager.hpp"
#include "TimeWarpEventSet.hpp"
#include "TimeWarpTerminationManager.hpp"

namespace {
const static std::string DEFAULT_CONFIG = R"x({

// If max-sim-time > 0, the simulation will halt once the time has been reached
"max-sim-time": 0,

// Valid options are "sequential" and "time-warp"
"simulation-type": "sequential",

"statistics": {
    // Valid options are "none", "json", "csv", "graphviz", and "metis".
    // "json" and "csv" are individual statistics, others are aggregate.
    "type": "none",
    // If statistics-type is not "none", save the output in this file
    "file": "statistics.out"
},

"time-warp" : {
    "gvt-calculation": {
        "period": 1000
    },

    "state-saving": {
        "type": "periodic",
        "period": 10
    },

    "cancellation": "aggressive",

    "worker-threads": 3,

    "scheduler-count": 1
},

"partitioning": {
    // Valid options are "default", "round-robin" and "profile-guided".
    // "default" will use user provided partitioning if given, else
    // "round-robin".
    "type": "default",
    // The path to the statistics file that was created from a previous run.
    // Only used if "partitioning-type" is "provided-guided".
    "file": "statistics.out"
}

})x";


// Recursively copy the values of b into a. Both a and b must be objects.
void update(Json::Value& a, Json::Value& b) {
    if (!a.isObject() || !b.isObject()) { return; }

    for (const auto& key : b.getMemberNames()) {
        if (a[key].isObject()) {
            update(a[key], b[key]);
        } else {
            a[key] = b[key];
        }
    }
}

Json::Value parseJsonFile(std::string filename) {
    Json::Reader reader;
    std::ifstream input(filename);

    if (!input.is_open()) {
        throw std::runtime_error(std::string("Could not open configuration file ") + filename);
    }

    Json::Value root;
    auto success = reader.parse(input, root);
    input.close();

    if (!success) {
        throw std::runtime_error(std::string("Failed to parse configuration\n") +
                                 reader.getFormattedErrorMessages());
    }

    return root;
}

} // namespace

namespace warped {

Configuration::Configuration(const std::string& config_file_name, unsigned int max_sim_time)
    : config_file_name_(config_file_name), max_sim_time_(max_sim_time), root_(nullptr)
{ readUserConfig(); }

Configuration::Configuration(const std::string& model_description, int argc,
                             const char* const* argv,
                             const std::vector<TCLAP::Arg*>& cmd_line_args)
    : config_file_name_(""), max_sim_time_(0), root_(make_unique<Json::Value>())
{ init(model_description, argc, argv, cmd_line_args); }

Configuration::Configuration(const std::string& model_description, int argc,
                             const char* const* argv)
    : config_file_name_(""), max_sim_time_(0), root_(make_unique<Json::Value>())
{ init(model_description, argc, argv, {}); }

Configuration::~Configuration() = default;

void Configuration::readUserConfig() {
    if (!config_file_name_.empty()) {
        auto root2 = parseJsonFile(config_file_name_);
        update(*root_, root2);
    }
}

void Configuration::init(const std::string& model_description, int argc, const char* const* argv,
                         const std::vector<TCLAP::Arg*>& cmd_line_args) {
    Json::Reader reader;

    if (!reader.parse(DEFAULT_CONFIG, *root_)) {
        throw std::runtime_error(std::string("Failed to parse default configuration\n") +
                                 reader.getFormattedErrorMessages());
    }

    // Update the JSON root with any command line args given
    bool should_dump;
    CommandLineConfiguration clc(*root_);
    std::tie(should_dump, config_file_name_) = clc.parse(model_description, argc, argv, cmd_line_args);

    readUserConfig();

    if (should_dump) {
        std::cout << *root_ << std::endl;
        std::exit(0);
    }
}

bool Configuration::checkTimeWarpConfigs(unsigned int local_config_id, unsigned int *all_config_ids,
    std::shared_ptr<TimeWarpCommunicationManager> comm_manager) {

    comm_manager->gatherUint(&local_config_id, all_config_ids);
    if (comm_manager->getID() == 0) {
        for (unsigned int i = 0; i < comm_manager->getNumProcesses(); i++) {
            if (all_config_ids[i] != local_config_id) {
                return false;
            }
        }
    }
    return true;
}

std::tuple<std::unique_ptr<EventDispatcher>, unsigned int>
Configuration::makeDispatcher() {
    std::shared_ptr<TimeWarpCommunicationManager> comm_manager =
                std::make_shared<TimeWarpMPICommunicationManager>();
    int num_partitions = comm_manager->initialize();

    unsigned int local_config_id;
    unsigned int *all_config_ids = new unsigned int[comm_manager->getNumProcesses()];
    std::string invalid_string;

    // MAX SIMULATION_TIME
    if ((*root_)["max-sim-time"].isUInt()) {
        max_sim_time_ = (*root_)["max-sim-time"].asUInt();
    }
    if (!checkTimeWarpConfigs(max_sim_time_, all_config_ids, comm_manager)) {
        invalid_string += std::string("\tMaximum simulation time\n");
    }

    auto simulation_type = (*root_)["simulation-type"].asString();
    if (simulation_type == "time-warp") {
        local_config_id = 1;
        if(!checkTimeWarpConfigs(local_config_id, all_config_ids, comm_manager)) {
            invalid_string += std::string("\tSimulation type\n");
        }

        std::unique_ptr<TimeWarpEventSet> event_set = make_unique<TimeWarpEventSet>();

        // WORKER THREADS
        int num_worker_threads = (*root_)["time-warp"]["worker-threads"].asInt();
        if (!checkTimeWarpConfigs(num_worker_threads, all_config_ids, comm_manager)) {
            invalid_string += std::string("\tNumber of worker threads\n");
        }

        // SCHEDULE QUEUES
        int num_schedulers = (*root_)["time-warp"]["scheduler-count"].asInt();
        if (!checkTimeWarpConfigs(num_schedulers, all_config_ids, comm_manager)) {
            invalid_string += std::string("\tNumber of schedule queues\n");
        }

        // STATE MANAGER
        std::unique_ptr<TimeWarpStateManager> state_manager;
        int state_period = 0;
        auto state_saving_type = (*root_)["time-warp"]["state-saving"]["type"].asString();
        if (state_saving_type == "periodic") {
            // local_config_id == 0 for periodic
            local_config_id = 0;
            if (!checkTimeWarpConfigs(local_config_id, all_config_ids, comm_manager)) {
                invalid_string += std::string("\tState-saving type\n");
            }

            state_period = (*root_)["time-warp"]["state-saving"]["period"].asInt();
            if (!checkTimeWarpConfigs(state_period, all_config_ids, comm_manager)) {
                invalid_string += std::string("\tState saving period\n");
            }

            state_manager = make_unique<TimeWarpPeriodicStateManager>(state_period);
        }

        // OUTPUT MANAGER
        std::unique_ptr<TimeWarpOutputManager> output_manager;
        auto cancellation_type = (*root_)["time-warp"]["cancellation"].asString();
        if (cancellation_type == "aggressive") {
            // local_config_id == 0 for aggressive
            local_config_id = 0;
            if (!checkTimeWarpConfigs(local_config_id, all_config_ids, comm_manager)) {
                invalid_string += std::string("\tCancellation type\n");
            }
            output_manager = make_unique<TimeWarpAggressiveOutputManager>();
        }

        // GVT
        int gvt_period = (*root_)["time-warp"]["gvt-calculation"]["period"].asInt();
        if (!checkTimeWarpConfigs(gvt_period, all_config_ids, comm_manager)) {
            invalid_string += std::string("\tGVT period\n");
        }
        std::unique_ptr<TimeWarpMatternGVTManager> mattern_gvt_manager
            = make_unique<TimeWarpMatternGVTManager>(comm_manager, gvt_period);
        std::unique_ptr<TimeWarpLocalGVTManager> local_gvt_manager =
            make_unique<TimeWarpLocalGVTManager>();

        // TERMINATION
        std::unique_ptr<TimeWarpTerminationManager> termination_manager =
            make_unique<TimeWarpTerminationManager>(comm_manager);

        // FILE STREAMS
        std::unique_ptr<TimeWarpFileStreamManager> twfs_manager =
            make_unique<TimeWarpFileStreamManager>();

        if (!invalid_string.empty()) {
            throw std::runtime_error(std::string("Configuration files do not match, \
check the following configurations:\n") + invalid_string);
        }

        if (comm_manager->getID() == 0) {
#ifdef NDEBUG
            std::cout << "OPTIMIZED" << std::endl;
#else
            std::cout << "DEBUG" << std::endl;
#endif
            std::cout << "Simulation type: " << simulation_type << std::endl;
            std::cout << "Number of processes: " << num_partitions << std::endl;
            std::cout << "Number of worker threads: " << num_worker_threads << std::endl;
            std::cout << "Number of Schedule queues: " << num_schedulers << std::endl;
            std::cout << "State-saving type: " << state_saving_type << std::endl;
            std::cout << "Cancellation type: " << cancellation_type << std::endl;
            if (state_saving_type == "periodic")
                std::cout << "\tPeriod: " << state_period << " events" << std::endl;
            std::cout << "GVT Period: " << gvt_period << " ms" << std::endl;
            std::cout << "Max simulation time: " \
                << (max_sim_time_ ? std::to_string(max_sim_time_) : "infinity") << std::endl;
        }

        return std::make_tuple(make_unique<TimeWarpEventDispatcher>(max_sim_time_,
            num_worker_threads, num_schedulers, comm_manager, std::move(event_set),
            std::move(mattern_gvt_manager), std::move(local_gvt_manager), std::move(state_manager),
            std::move(output_manager), std::move(twfs_manager), std::move(termination_manager)),
            num_partitions);
    }

    if (comm_manager->getNumProcesses() > 1) {
            throw std::runtime_error("Sequential simulation must be single process\n");
    }

    std::cout << "Simulation type: Sequential" << std::endl;

    // Return a SequentialEventDispatcher by default
    std::unique_ptr<EventStatistics> stats;
    auto statistics_type = (*root_)["statistics"]["type"].asString();
    auto statistics_file = (*root_)["statistics"]["file"].asString();
    std::cout << "Statistics type: " << statistics_type << std::endl;
    std::cout << "Statistics file: " << statistics_file << std::endl;
    if (statistics_type == "json") {
        auto type = IndividualEventStatistics::OutputType::Json;
        stats = make_unique<IndividualEventStatistics>(statistics_file, type);
    } else if (statistics_type == "csv") {
        auto type = IndividualEventStatistics::OutputType::Csv;
        stats = make_unique<IndividualEventStatistics>(statistics_file, type);
    } else if (statistics_type == "graphviz") {
        auto type = AggregateEventStatistics::OutputType::Graphviz;
        stats = make_unique<AggregateEventStatistics>(statistics_file, type);
    } else if (statistics_type == "metis") {
        auto type = AggregateEventStatistics::OutputType::Metis;
        stats = make_unique<AggregateEventStatistics>(statistics_file, type);
    } else {
        stats = make_unique<NullEventStatistics>();
    }
    return std::make_tuple(make_unique<SequentialEventDispatcher>(max_sim_time_,
        std::move(stats)), 1);
}

std::unique_ptr<Partitioner> Configuration::makePartitioner() {
    auto partitioner_type = (*root_)["partitioning"]["type"].asString();
    if (partitioner_type == "default" || partitioner_type == "round-robin") {
        return make_unique<RoundRobinPartitioner>();
    } else if (partitioner_type == "profile-guided") {
        auto filename = (*root_)["partitioning"]["file"].asString();
        return make_unique<ProfileGuidedPartitioner>(filename);
    }
    throw std::runtime_error(std::string("Invalid partitioning type: ") + partitioner_type);
}

std::unique_ptr<Partitioner>
Configuration::makePartitioner(std::unique_ptr<Partitioner> user_partitioner) {
    const auto& partitioner_type = (*root_)["partitioning"];
    if (partitioner_type == "default") {
        return std::move(user_partitioner);
    }
    return makePartitioner();
}

} // namespace warped
