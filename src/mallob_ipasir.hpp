
#ifndef DOMPASCH_MALLOB_IPASIR_HPP
#define DOMPASCH_MALLOB_IPASIR_HPP

#include <string>
#include <vector>
#include <set>
#include <unistd.h>
#include <thread>
#include <mutex>
#include <condition_variable>

#include "event_poller.hpp"

#ifndef MALLOB_BASE_DIRECTORY
#define MALLOB_BASE_DIRECTORY "."
#endif

#ifndef MALLOB_API_INDEX
#define MALLOB_API_INDEX "0"
#endif

class MallobIpasir {

public:
    static EventPoller* _event_poller;
    enum Interface {SOCKET, FILESYSTEM};
    enum FormulaTransfer {FILE, NAMED_PIPE};

private:
    Interface _interface;
    FormulaTransfer _formula_transfer;
    std::string _api_directory;
    int _solver_id;
    bool _incremental = true;

    std::vector<int> _formula;
    std::vector<int> _assumptions;
    int _num_vars = 0;
    int _num_cls = 0;
    int _revision = 0;

    bool _presubmitted = false;
    int _fd_formula = -1;

    int _fd_inotify = -1;
    int _fd_inotify_watcher = -1;
    std::vector<char> _inotify_buffer;

    int (*_terminate_callback) (void*) = nullptr;
    void* _terminate_data;

    std::vector<int> _model;
    std::set<int> _failed_assumptions;

    std::vector<std::thread> _branched_threads;

    std::mutex _branch_mutex;
    std::condition_variable _branch_cond_var;

    int _fd_socket;

public:
    MallobIpasir(Interface interface, bool incremental);
    
    std::string getSignature() const;

    void addLiteral(int lit) {
        _formula.push_back(lit);
        if (_presubmitted && _formula.size() >= 512) {
            completeWrite(_fd_formula, (char*)_formula.data(), _formula.size()*sizeof(int));
            _formula.clear();
        }
        if (lit == 0) {
            _num_cls++;
        } else {
            _num_vars = std::max(_num_vars, std::abs(lit));
        }
    }
    void addAssumption(int lit) {
        _assumptions.push_back(lit);
        _num_vars = std::max(_num_vars, std::abs(lit));
    }

    int solve();

    void branchedSolve(void * data, int (*terminate)(void* data), void (*callbackAtFinish)(int result, void* solver, void* data));

    void destruct();

    int getValue(int lit) {
        return (lit < 0 ? -1 : 1) * _model[std::abs(lit)];
    }
    bool isAssumptionFailed(int lit) {
        return _failed_assumptions.count(lit);
    }
    void setTerminateCallback(void * data, int (*terminate)(void * data)) {
        _terminate_callback = terminate;
        _terminate_data = data;
    }

    void submitJob();

private:

    std::string getJobName(int revision);
    std::string getFormulaName();
    
    void writeJson(nlohmann::json& json, const std::string& file);
    std::optional<nlohmann::json> readJson(const std::string& file);
    
    void setupConnection();
    void sendJson(nlohmann::json& json);
    std::optional<nlohmann::json> receiveJson();

    void writeFormula(const std::string& file);
    void pipeFormula(const std::string& pipe);
    
    void completeWrite(int fd, const char* data, int numBytes);
    void completeRead(int fd, char* data, int numBytes);
};

#endif
