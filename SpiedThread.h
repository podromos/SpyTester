#ifndef SPYTESTER_SPIEDTHREAD_H
#define SPYTESTER_SPIEDTHREAD_H


#include <csignal>
#include <mutex>
#include <condition_variable>
#include "TracingCommand.h"

class Tracer;
class SpiedProgram;

class SpiedThread {
    const pid_t _tid;

    std::mutex _isRunningMutex;
    std::condition_variable _isRunningCV;
    bool _isRunning;
    bool _isSigTrapExpected;

    Tracer& _tracer;
    SpiedProgram& _spiedProgram;

    using SpiedThreadCmd = TracingCommand<SpiedThread>;

public:
    SpiedThread(SpiedProgram& spiedProgram, Tracer& tracer, pid_t tid);

    ~SpiedThread();

    pid_t getTid() const;
    bool isRunning() ;

    void setRunning(bool isRunning);

    void handleSigTrap();

    void resume();
    void singleStep();
    void stop();
};


#endif //SPYTESTER_SPIEDTHREAD_H