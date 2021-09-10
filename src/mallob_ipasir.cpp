
#include <fstream>
#include <iomanip>
#include <functional>
#include <iostream>
#include <atomic>

#include "json.hpp"
#include "ipasir.h"
#include "mallob_ipasir.hpp"

// Global variable to enumerate several distinct IPASIR instances
std::atomic_int ipasirSolverIndex = 0;

MallobIpasir::MallobIpasir() :
        _api_directory(MALLOB_BASE_DIRECTORY + std::string("/.api/jobs.") + MALLOB_API_INDEX + std::string("/")),
        _solver_id(ipasirSolverIndex++) {}

int MallobIpasir::solve() {

    _model.clear();
    _failed_assumptions.clear();

    // Write formula
    std::string formulaFilename = "/tmp/ipasir_mallob_" 
        + std::to_string(getpid()) + "_" 
        + std::to_string(_solver_id) + ".cnf";
    std::cout << "Writing " << _num_cls << " clauses and " << _assumptions.size() 
        << " assumptions to " << formulaFilename << std::endl;
    std::ofstream fOut(formulaFilename);
    fOut << "p cnf " << _num_vars << " " << _num_cls << "\n";
    for (int lit : _formula) {
        if (lit == 0) fOut << "0\n";
        else fOut << lit << " ";
    }
    if (!_assumptions.empty()) {
        fOut << "a ";
        for (int lit : _assumptions) {
            fOut << lit << " ";
        }
        fOut << "0\n";
    }
    fOut.close();

    // Write JSON
    std::string jobName = getJobName(_revision);
    nlohmann::json j = { 
        {"user", "ipasir"}, 
        {"name", jobName}, 
        {"file", formulaFilename}, 
        {"priority", 1.000}, 
        {"wallclock-limit", "0"}, 
        {"cpu-limit", "0"}, 
        {"incremental", true}
    };
    if (_revision > 0) {
        j["precursor"] = "ipasir." + getJobName(_revision-1);
    }
    std::string jsonFilename = _api_directory + "/new/ipasir." + jobName + ".json";
    std::ofstream o(jsonFilename);
    o << std::setw(4) << j << std::endl;
    o.close();

    // Wait for a response
    int resultcode = 0;
    std::string resultFilename = _api_directory + "/done/ipasir." + jobName + ".json";
    while (true) {
        usleep(1000 * 10); // 10 milliseconds

        if (_terminate_callback(_terminate_data)) {
            // Terminate catched!
            // TODO cause Mallob to stop this iteration
            break;
        }

        // Try to parse result
        try {
            std::ifstream i(resultFilename);
            i >> j;
        } catch (...) {
            continue;
        }

        // Success!
        resultcode = j["result"]["resultcode"];
        if (resultcode == 10) {
            // SAT
            _model = j["result"]["solution"].get<std::vector<int>>();
        } else if (resultcode == 20) {
            // UNSAT
            auto failedVec = j["result"]["solution"].get<std::vector<int>>();
            _failed_assumptions.insert(failedVec.begin(), failedVec.end());
        } else {
            // UNKNOWN
        }

        remove(resultFilename.c_str());
        break;
    }

    _revision++;

    _formula.clear();
    _assumptions.clear();
    _num_cls = 0;

    return resultcode;
}

void MallobIpasir::destruct() {
    if (_revision == 0) return;

    // Write a JSON notifying Mallob to destroy the job
    std::string jobName = getJobName(_revision);
    nlohmann::json j = { 
        {"user", "ipasir"}, 
        {"name", jobName},
        {"priority", 1.000}, 
        {"wallclock-limit", "0"}, 
        {"cpu-limit", "0"}, 
        {"incremental", true},
        {"done", true}
    };
    if (_revision > 0) {
        j["precursor"] = "ipasir." + getJobName(_revision-1);
    }
    std::string jsonFilename = _api_directory + "/new/ipasir." + jobName + ".json";
    std::ofstream o(jsonFilename);
    o << std::setw(4) << j << std::endl;
}


// IPASIR C methods

MallobIpasir* get(void* solver) {return (MallobIpasir*)solver;}

const char * ipasir_signature () {return "Mallob IPASIR JSON bridge";}
void* ipasir_init () {return new MallobIpasir();}
void ipasir_release (void * solver) {get(solver)->destruct(); delete get(solver);}
void ipasir_add (void * solver, int32_t lit_or_zero) {get(solver)->addLiteral(lit_or_zero);}
void ipasir_assume (void * solver, int32_t lit) {get(solver)->addAssumption(lit);}
int ipasir_solve (void * solver) {return get(solver)->solve();}
int32_t ipasir_val (void * solver, int32_t lit) {return get(solver)->getValue(lit);}
int ipasir_failed (void * solver, int32_t lit) {return get(solver)->isAssumptionFailed(lit) ? 1 : 0;}
void ipasir_set_terminate (void * solver, void * data, int (*terminate)(void * data)) 
    {get(solver)->setTerminateCallback(data, terminate);}

// TODO implement?
void ipasir_set_learn (void * solver, void * data, int max_length, void (*learn)(void * data, int32_t * clause)) {}
