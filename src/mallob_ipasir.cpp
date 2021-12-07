
#include <fstream>
#include <iomanip>
#include <functional>
#include <iostream>
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/inotify.h>

#include "json.hpp"
#include "ipasir.h"
#include "mallob_ipasir.hpp"
#include "glob.hpp"

// Global variable to enumerate several distinct IPASIR instances
std::atomic_int ipasirSolverIndex = 0;

EventPoller* MallobIpasir::_event_poller = nullptr;

MallobIpasir::MallobIpasir(Interface interface, bool incremental) :
        _interface(interface), _formula_transfer(NAMED_PIPE),
        _api_directory(MALLOB_BASE_DIRECTORY + std::string("/.api/jobs.") + MALLOB_API_INDEX + std::string("/")),
        _solver_id(ipasirSolverIndex++), _incremental(incremental) {

    std::cout << getSignature() << std::endl;

    if (_event_poller == nullptr) {
        _event_poller = new EventPoller(_api_directory + "/out/");
    }
}

std::string MallobIpasir::getSignature() const {
    return "Mallob IPASIR bridge (incremental=" + std::string(_incremental ? "yes":"no")
        + ", interface=" + std::string(_interface == FILESYSTEM ? "filesys":"socket")
        + ", transfer=" + std::string(_formula_transfer == FILE ? "file":"namedpipe")
        + ")";
}

void MallobIpasir::submitJob() {

    if (_presubmitted) return;

    _model.clear();
    _failed_assumptions.clear();

    std::string formulaBaseString = getFormulaName();
    
    if (_formula_transfer == FILE) {
        formulaBaseString += ".cnf";
        writeFormula(formulaBaseString);
    } else {
        formulaBaseString += ".pipe";
        mkfifo(formulaBaseString.c_str(), 0666);
    }

    // Generate JSON
    std::string jobName = getJobName(_revision);
    nlohmann::json j = { 
        {"user", "ipasir"}, 
        {"name", jobName}, 
        {"application", "SAT"},
        {"file", formulaBaseString}, 
        {"priority", 1.000}, 
        {"wallclock-limit", "0"}, 
        {"cpu-limit", "0"}, 
        {"incremental", _incremental},
        {"content-mode", _formula_transfer == FILE ? "text" : "raw"}
    };
    if (_incremental && _revision > 0) {
        j["precursor"] = "ipasir." + getJobName(_revision-1);
    }

    // Submit JSON
    if (_interface == FILESYSTEM) {
        writeJson(j, _api_directory + "/in/ipasir." + jobName + ".json");
    } else {
        sendJson(j);
    }

    _presubmitted = true;
    _fd_formula = open(formulaBaseString.c_str(), O_WRONLY);
}

int MallobIpasir::solve() {

    if (!_presubmitted) {
        submitJob();
    }

    if (_formula_transfer == NAMED_PIPE) {
        pipeFormula(getFormulaName());
        close(_fd_formula);
        _fd_formula = -1;
    }

    // Wait for a response
    int resultcode = 0;
    std::string jobName = getJobName(_revision);
    std::string resultFilename = _api_directory + "/out/ipasir." + jobName + ".json";
    std::string resultBasename = "ipasir." + jobName + ".json";
    bool hasInterrupted = false;

    while (true) {

        // Check termination / interruption
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
            if (_interface == FILESYSTEM) {
                writeJson(jInterrupt, _api_directory + "/in/ipasir." + jobName + ".interrupt.json");
            } else {
                sendJson(jInterrupt);
            }
            // Do not repeat this interrupt even if it takes a while for the job to return.
            hasInterrupted = true;
        }

        if (!_event_poller->poll(resultBasename)) continue;

        // Fitting event in the directory occurred: Try to parse result
        auto optJson = readJson(resultFilename);
        if (!optJson) continue;
        auto j = std::move(optJson.value());

        // Success!
        resultcode = j["result"]["resultcode"];
        if (resultcode == 10) {
            // SAT
            if (j["result"].contains("solution-file")) {

                // Read solution from named pipe
                auto solutionPipe = j["result"]["solution-file"].get<std::string>();
                int fd = open(solutionPipe.c_str(), O_RDONLY);
                int solutionSize = j["result"]["solution-size"].get<int>();
                _model.clear();
                _model.resize(solutionSize);
                std::cout << "Reading solution : " << solutionSize << " ints" << std::endl;
                int numRead = 0;
                while (numRead < solutionSize * sizeof(int)) {
                    int n = read(fd, ((char*)_model.data())+numRead, 
                        solutionSize*sizeof(int) - numRead);
                    if (n <= 0) break;
                    numRead += n;
                }
                std::cout << "Read solution of size " << _model.size() << " ("
                    << _model[0] << "," << _model[1] << ",...," 
                    << _model[_model.size()-2] << "," << _model[_model.size()-1]  
                    << ")" << std::endl;
                close(fd);
            } else {
                _model = j["result"]["solution"].get<std::vector<int>>();
            }
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
    _presubmitted = false;
    _event_poller->unregister(resultBasename);

    if (_incremental) {
        _formula.clear();
        _assumptions.clear();
        _num_cls = 0;
    }

    return resultcode;
}

void MallobIpasir::branchedSolve(void * data, int (*terminate)(void * data), void (*callbackAtFinish)(int result, void* solver, void* data)) {

    MallobIpasir* child = new MallobIpasir(_interface, /*incremental=*/false);
    child->_formula = _formula;
    child->_assumptions = _assumptions;
    child->_num_vars = _num_vars;
    child->_num_cls = _num_cls;
    child->setTerminateCallback(data, terminate);

    std::string jobName = child->getJobName(child->_revision);
    _branched_signals[jobName] = std::pair<std::mutex*, std::condition_variable*>(
        &child->_branch_mutex, &child->_branch_cond_var);

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
        writeJson(j, _api_directory + "/in/ipasir." + jobName + ".json");
    }

    // Clean up descriptors
    if (_fd_inotify_watcher != -1) inotify_rm_watch(_fd_inotify, _fd_inotify_watcher);
    if (_fd_inotify != -1) close(_fd_inotify);
}





std::string MallobIpasir::getJobName(int revision) {
    return "job-" + std::to_string(getpid()) 
        + "_" + std::to_string(_solver_id) 
        + "-" + std::to_string(revision);
}

std::string MallobIpasir::getFormulaName() {
    return "/tmp/ipasir_mallob_" 
            + std::to_string(getpid()) + "_" 
            + std::to_string(_solver_id) + "_" + std::to_string(_revision);
}

void MallobIpasir::writeJson(nlohmann::json& json, const std::string& file) {
    std::ofstream o(file);
    o << std::setw(4) << json << std::endl;
}

void MallobIpasir::setupConnection() {
    sockaddr_un address;
	address.sun_family = AF_UNIX;
	
	// Make a socket file
	if ((_fd_socket = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		perror("Socket cannot be instantiated");
		exit(EXIT_FAILURE);
	}

	// Destination
    auto globResult = cppGlob("/tmp/mallob_*.*.sk");
    assert(!globResult.empty());
    std::string destination = globResult.back();

	memcpy(address.sun_path, "", destination.size() + 1);

	// Connect - blocking call
	// STREAM are need to connect to the other hand before
	// starting a transfer.
	//
	// The file descriptor is marked as connected
	printf("Connecting...\n");
	if (connect(_fd_socket, (sockaddr*) &address, sizeof(address)) == -1) {
		if(errno == ECONNREFUSED) printf("ECONNREFUSED\n");
		close(_fd_socket);
		perror("Cannot connect");
		exit(EXIT_FAILURE);
	}
	printf("Established connection with: %s\n", address.sun_path);

	// Receiving
	size_t size;
    /*
	do {
		memset(message, 0, MAX_MESSAGE_SIZE);
		size = recv(_fd_socket, message, MAX_MESSAGE_SIZE, 0);
		if(size == -1) { 
			if (errno == ECONNRESET) printf("ECONNRESET\n");
			close(fd);
			perror("Receiver"); exit(EXIT_FAILURE); 
		}
		printf("Receive %lu B\nMessage: %s\n\n", size, message);
	} while (strcmp(message, "quit") != 0);
    */
}

std::optional<nlohmann::json> MallobIpasir::readJson(const std::string& file) {
    std::optional<nlohmann::json> opt;
    try {
        nlohmann::json j;
        std::ifstream i(file);
        i >> j;
        opt.emplace(std::move(j));
        std::cout << "Received " << file << std::endl;
    } catch (...) {}
    return opt;
}

void MallobIpasir::sendJson(nlohmann::json& json) {

}

void MallobIpasir::writeFormula(const std::string& formulaFilename) {
    
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
}

void MallobIpasir::pipeFormula(const std::string& pipeFilename) {
    int zero = 0;

    // Write clause literals with separation zeroes
    write(_fd_formula, _formula.data(), _formula.size()*sizeof(int));

    if (_incremental) {
        // Incremental mode: 
        // Write an empty clause to signal assumptions
        write(_fd_formula, &zero, sizeof(int));
        // Write assumptions (without any separation zeroes)
        write(_fd_formula, _assumptions.data(), _assumptions.size()*sizeof(int));
    } else {
        // Non-incremental mode: write assumptions as unit clauses
        for (int a : _assumptions) {
            write(_fd_formula, &a, sizeof(int));
            write(_fd_formula, &zero, sizeof(int));
        }
    }
}


// IPASIR C methods

MallobIpasir* get(void* solver) {return (MallobIpasir*)solver;}

// Addition to the interface: enables to create a non-incremental solver which can then be branched
// via mallob_ipasir_branched_solve ().
void* mallob_ipasir_init (bool incremental) {return new MallobIpasir(/*interface=*/MallobIpasir::Interface::FILESYSTEM, /*incremental=*/incremental);} 

const char * ipasir_signature () {return "Mallob IPASIR JSON bridge";}
void* ipasir_init () {return mallob_ipasir_init (/*incremental=*/true);}
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

// Addition to the interface: branch off a child solver on the current formulae / assumptions,
// call the provided callback as soon as solving is done. Clears assumptions in the parent solver.
void mallob_ipasir_branched_solve (void * solver, void * data, int (*terminate)(void * data), void (*callback_done)(int result, void* child_solver, void* data)) {
    get(solver)->branchedSolve(data, terminate, callback_done);
}

void mallob_ipasir_presubmit (void * solver) {get(solver)->submitJob();}
