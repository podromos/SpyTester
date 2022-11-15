#include <sys/ptrace.h>
#include <iostream>
#include <cstring>
#include <wait.h>
#include <unistd.h>
#include "../include/Tracer.h"
#include "../include/SpiedProgram.h"

#include <sys/syscall.h>
#include <dlfcn.h>

#define gettid() syscall(SYS_gettid)

Tracer::Tracer(SpiedProgram &spiedProgram)
: _spiedProgram(spiedProgram), _state(NOT_STARTED)
{
    if(sem_init(&_cmdsSem, 0, 0) == -1){
        std::cerr << __FUNCTION__ << " : semaphore initialization failed : " << strerror(errno) << std::endl;
        throw std::invalid_argument("invalid sem init");
    }

    std::unique_lock lk(_stateMutex);

    pthread_attr_init(&_attr);

    int ret = pthread_create(&_cmdHandler, &_attr, commandHandler, this);
    if(ret !=0)
    {
        std::cerr << __FUNCTION__ << " : pthread_create failed : " << strerror(ret) << std::endl;
        throw std::invalid_argument("pthread_create failed");
    }

    _stateCV.wait(lk, [this]{return _state == STARTING;});
}

void * Tracer::commandHandler(void * tracer) {
    auto t = (Tracer*) tracer;

    if(t == nullptr)
    {
        std::cerr << __FUNCTION__ << " : unexpected pid " << getpid() << std::endl;
        return nullptr;
    }
    t->initTracer();
    t->handleCommand();

    return nullptr;
}

void Tracer::initTracer() {
    _tracerTid = gettid();

    // Create spied program process
    int cloneFlags = CLONE_FS | CLONE_FILES | SIGCHLD | CLONE_VM;
    _starterTid = clone(preStarter, _spiedProgram.getStackTop(), cloneFlags, this);

    if(_starterTid == -1){
        std::cerr << __FUNCTION__ <<" : clone failed : "<< strerror(errno) <<std::endl;
        return;
    }

    // Wait for spied program process to be created
    int wstatus;
    if(waitpid(_starterTid, &wstatus, 0) == -1){
        std::cerr << "waitpid failed : " << strerror(errno) << std::endl;
    }
    if (WIFSTOPPED(wstatus)){
        if(ptrace(PTRACE_SETOPTIONS, _starterTid, nullptr, PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK) == -1){
            std::cerr << __FUNCTION__ <<" : PTRACE_O_TRACECLONE failed : "<< strerror(errno) <<std::endl;
        }
        if(ptrace(PTRACE_CONT, _starterTid, nullptr, nullptr) == -1){
            std::cerr << __FUNCTION__ <<" : PTRACE_CONT failed for starter" << std::endl;
        }
    }

    // Wait for pre starter to create starter thread
    if(waitpid(_starterTid, &wstatus, 0) == -1){
        std::cerr << __FUNCTION__ << " : waitpid failed (create starter): " << strerror(errno) << std::endl;
    }

    _traceeTid = wait(&wstatus);
    if((_traceeTid == -1) || !WIFSTOPPED(wstatus)){
        std::cerr << "wait failed : " << strerror(errno) << std::endl;
    }

    // Start event handler
    int ret = pthread_create(&_evtHandler, &_attr, eventHandler, this);
    if(ret != 0){
        std::cerr << __FUNCTION__ <<" : pthread_create failed : "<< strerror(ret) << std::endl;
    }
}

int Tracer::preStarter(void * tracer) {

    auto t = (Tracer*) tracer;

    if(ptrace(PTRACE_TRACEME, 0, NULL, NULL) == -1) {
        std::cerr<< __FUNCTION__ << " : PTRACE_TRACEME failed : " << strerror(errno) << std::endl;
        std::exit(1);
    }

    if( raise(SIGTRAP) != 0)
    {
        std::cerr<<__FUNCTION__<<" : ptrace failed : " << strerror(errno) << std::endl;
        std::exit(1);
    }
    pthread_create(&(t->_starter), &(t->_attr), starter, t->_spiedProgram.getProgParam());
    return 0;
}

void *Tracer::starter(void *param) {
    auto progParam = (struct ProgParam *) param;

    _asm_starter(progParam->entryPoint, progParam->argc, progParam->argv, progParam->envp);

    return nullptr;
}

void * Tracer::eventHandler(void* tracer) {
    auto t = (Tracer *) tracer;
    if (t == nullptr) {
        std::cerr << __FUNCTION__ << " : unexpected pid " << getpid() << std::endl;
        return nullptr;
    }

    t->handleEvent();

    return nullptr;
}

void Tracer::handleCommand() {
    std::cout << __FUNCTION__ << " : start handling commands!" << std::endl;
    setState(STARTING);

    while(sem_wait(&_cmdsSem) == 0)
    {
        //Get next command to execute
        _cmdsMutex.lock();
        while(!_commands.empty()) {
            auto& command = _commands.front();
            _cmdsMutex.unlock();

            command();

            _cmdsMutex.lock();
            _commands.pop();
        }
        if(_state == STOPPING){
            // At this point we know the queue is empty, so we can stop handling command
            _cmdsMutex.unlock();
            break;
        }
        _cmdsMutex.unlock();
    }

    std::cout << __FUNCTION__ << " : stop handling commands!" << std::endl;
    setState(STOPPED);
}

void Tracer::handleEvent() {
    std::cout << __FUNCTION__ << " : start handling events!" << std::endl;

    int wstatus;
    pid_t tid;
    // Wait until all child threads exit
    while((tid = waitpid(-1, &wstatus, WCONTINUED)) > 0)
    {
        // Ignore starterThread
        if(tid == _starterTid) continue;

        SpiedThread &thread = _spiedProgram.getSpiedThread(tid);

        if (WIFSTOPPED(wstatus)) {
            int signum = WSTOPSIG(wstatus);
            thread.setState(SpiedThread::STOPPED, signum);

            std::cout << __FUNCTION__ << " : thread " << tid << " stopped : SIG = " << signum << std::endl;
            switch (signum) {
                case SIGTRAP:
                    thread.handleSigTrap(wstatus);
                    break;
                default:
                    break;
            }
        } else if (WIFCONTINUED(wstatus)) {
            thread.setState(SpiedThread::RUNNING);
            std::cout << __FUNCTION__ << " : thread " << tid << " resumed!" << std::endl;
        } else if (WIFEXITED(wstatus)) {
            thread.setState(SpiedThread::TERMINATED, WEXITSTATUS(wstatus));
            std::cout << __FUNCTION__ << " : thread " << tid << " exits with code " << WEXITSTATUS(wstatus)
                      << std::endl;
        } else if (WIFSIGNALED(wstatus)) {
            thread.setState(SpiedThread::TERMINATED, WTERMSIG(wstatus));
            std::cout << __FUNCTION__ << " : thread " << tid << " exits with signal " << WTERMSIG(wstatus)
                      << std::endl;
        }
    }

    setState(STOPPING);
    std::cout << __FUNCTION__ << ": stop handling events!" << std::endl;

    // Wakeup command handler
    sem_post(&_cmdsSem);
}

void Tracer::start() {
    if(_state == STARTING) {
        setState(TRACING);
        SpiedThread& sp  = _spiedProgram.getSpiedThread(_traceeTid);
    }
    else{
        std::cout << __FUNCTION__ << " : tracer has already been started " << std::endl;
    }
}

bool Tracer::command(std::function<bool()> &&cmd, bool sync) {
    bool res = false;

    if(gettid() == _tracerTid) {
        res = cmd();
    } else if(sync) {
        std::condition_variable cmdResCV;
        std::mutex cmdResMutex;

        std::unique_lock lk(cmdResMutex);
        bool cmdExecuted = false;

        _cmdsMutex.lock();

        if(_state != TRACING && _state != STARTING){
            _cmdsMutex.unlock();
            return false;
        }

        _commands.emplace([&cmdResMutex, &cmdResCV, &cmd, &res, &cmdExecuted]() {
            cmdResMutex.lock();

            res = cmd();
            cmdExecuted = true;

            cmdResMutex.unlock();
            cmdResCV.notify_all();

            return res;
        });
        _cmdsMutex.unlock();
        sem_post(&_cmdsSem);

        cmdResCV.wait(lk, [&cmdExecuted] { return cmdExecuted; });
    } else {
        _cmdsMutex.lock();
        _commands.emplace(std::move(cmd));
        _cmdsMutex.unlock();
        sem_post(&_cmdsSem);

        res = true;
    }

    return res;
}

pid_t Tracer::getTraceePid() const {
    return _starterTid;
}

bool Tracer::isTracerThread() const {
    return gettid() == _tracerTid;
}

void Tracer::setState(Tracer::E_State state) {
    _stateMutex.lock();
    _state = state;
    _stateMutex.unlock();
    _stateCV.notify_all();
}

Tracer::~Tracer() {
    std::unique_lock lk(_stateMutex);
    if(_state != STOPPED){
        // wait for tracing to be stopped
        _stateCV.wait(lk, [this](){return _state==STOPPED;});
    }

    pthread_join(_cmdHandler, nullptr);
    pthread_join(_evtHandler, nullptr);
    pthread_join(_starter, nullptr);
    pthread_attr_destroy(&_attr);
    sem_destroy(&_cmdsSem);

    std::cout<< __FUNCTION__ << std::endl;
}
