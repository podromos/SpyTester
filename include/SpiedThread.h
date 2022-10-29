#ifndef SPYTESTER_SPIEDTHREAD_H
#define SPYTESTER_SPIEDTHREAD_H


#include <csignal>
#include <mutex>
#include <condition_variable>
#include "WatchPoint.h"

class Tracer;
class SpiedProgram;

class SpiedThread {
public:

    typedef enum {
        STOPPED,
        RUNNING,
        TERMINATED
    } E_State;

    SpiedThread(SpiedProgram& spiedProgram, Tracer& tracer, pid_t tid);
    SpiedThread(SpiedThread&& spiedThread) = default;
    SpiedThread(const SpiedThread& ) = delete;
    ~SpiedThread();

    pid_t getTid() const;

    E_State getState();
    void setState(E_State state);

    void handleSigTrap(int wstatus);
    void handleSigStop();

    void resume(int signum = 0);
    void singleStep();
    void stop();
    void terminate();

    void backtrace();
    void detach();

    WatchPoint* createWatchPoint();
    void deleteWatchPoint(WatchPoint* watchPoint);

private:
    const pid_t _tid;
    std::mutex _stateMutex;
    std::condition_variable _stateCV;
    E_State _state;

    bool _isSigTrapExpected;

    std::vector<std::pair<WatchPoint, bool>> _watchPoints;
    Tracer& _tracer;

    SpiedProgram& _spiedProgram;
};


#endif //SPYTESTER_SPIEDTHREAD_H
