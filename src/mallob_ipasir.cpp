
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

MallobIpasir::MallobIpasir(bool incremental) :
        _api_directory(MALLOB_BASE_DIRECTORY + std::string("/.api/jobs.") + MALLOB_API_INDEX + std::string("/")),
        _solver_id(ipasirSolverIndex++), _incremental(incremental) {}

int MallobIpasir::solve() {

    _model.clear();
    _failed_assumptions.clear();

    // Write formula
    std::string formulaFilename = "/tmp/ipasir_mallob_" 
        + std::to_string(getpid()) + "_" 
        + std::to_string(_solver_id) + "_" + std::to_string(_revision) + ".cnf";
    std::cout << "Writing " << _num_cls << " clauses and " << _assumptions.size() 
        << " assumptions to " << formulaFilename << std::endl;
    std::ofstream fOut(formulaFilename);

    int numCls = _num_cls + (_incremental ? 0 : _assumptions.size());
    fOut << "p cnf " << _num_vars << " " << numCls << "\n";
    for (int lit : _formula) {
        if (lit == 0) fOut << "0\n";
        else fOut << lit << " ";
    }
    if (!_assumptions.empty()) {
        if (_incremental) {
            fOut << "a ";
            for (int lit : _assumptions) {
                fOut << lit << " ";
            }
            fOut << "0\n";
        } else {
            for (int lit : _assumptions) {
                fOut << lit << " 0\n";
            }
        }
    }
    fOut.close();

    // Write JSON
    std::string jobName = getJobName(_revision);
    nlohmann::json j = { 
        {"user", "ipasir"}, 
        {"name", jobName}, 
        {"application", "SAT"},
        {"file", formulaFilename}, 
        {"priority", 1.000}, 
        {"wallclock-limit", "0"}, 
        {"cpu-limit", "0"}, 
        {"incremental", _incremental}
    };
    if (_incremental && _revision > 0) {
        j["precursor"] = "ipasir." + getJobName(_revision-1);
    }
    writeJson(j, _api_directory + "/new/ipasir." + jobName + ".json");

    // Wait for a response
    int resultcode = 0;
    std::string resultFilename = _api_directory + "/done/ipasir." + jobName + ".json";
    bool hasInterrupted = false;

    while (true) {
        usleep(1000 * 10); // 10 milliseconds

        if (!hasInterrupted && _terminate_callback != nullptr && _terminate_callback(_terminate_data) != 0) {
            // Terminate catched! Send interrupt over interface.
            // Still wait for a normal answer from the job result interface.
            nlohmann::json jInterrupt = {
                {"user", "ipasir"},
                {"name", jobName},
                {"application", "SAT"},
                {"incremental", _incremental},
                {"interrupt", true}
            };
            writeJson(jInterrupt, _api_directory + "/new/ipasir." + jobName + ".interrupt.json");
            // Do not repeat this interrupt even if it takes a while for the job to return.
            hasInterrupted = true;
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

    if (_incremental) {
        _formula.clear();
        _assumptions.clear();
        _num_cls = 0;
    }

    return resultcode;
}

void MallobIpasir::branchedSolve(void * data, int (*terminate)(void * data), void (*callbackAtFinish)(int result, void* solver, void* data)) {

    MallobIpasir* child = new MallobIpasir(/*incremental=*/false);
    child->_formula = _formula;
    child->_assumptions = _assumptions;
    child->_num_vars = _num_vars;
    child->_num_cls = _num_cls;
    child->setTerminateCallback(data, terminate);

    _branched_threads.emplace_back([data, child, callbackAtFinish]() {
        int result = child->solve();
        callbackAtFinish(result, child, data);
    });

    _assumptions.clear();
}

void MallobIpasir::destruct() {

    for (auto& thread : _branched_threads) thread.join();

    if (_revision == 0) return;

    if (_incremental) {
        // Write a JSON notifying Mallob to destroy the job
        std::string jobName = getJobName(_revision);
        nlohmann::json j = { 
            {"user", "ipasir"}, 
            {"name", jobName},
            {"application", "SAT"},
            {"incremental", _incremental},
            {"done", true},
            {"precursor", "ipasir." + getJobName(_revision-1)}
        };
        writeJson(j, _api_directory + "/new/ipasir." + jobName + ".json");
    }
}

void MallobIpasir::writeJson(nlohmann::json& json, const std::string& file) {
    std::ofstream o(file);
    o << std::setw(4) << json << std::endl;
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

// Addition to the interface: enables to create a non-incremental solver which can then be branched
// via mallob_ipasir_branched_solve ().
void* mallob_ipasir_init (bool incremental) {return new MallobIpasir(incremental);} 

// Addition to the interface: branch off a child solver on the current formulae / assumptions,
// call the provided callback as soon as solving is done. Clears assumptions in the parent solver.
void mallob_ipasir_branched_solve (void * solver, void * data, int (*terminate)(void * data), void (*callback_done)(int result, void* child_solver, void* data)) {
    get(solver)->branchedSolve(data, terminate, callback_done);
}