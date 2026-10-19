#include "mods/action_worker.hpp"
#include "mods/manager_commands.hpp"
#include "mods/fixture_observation.hpp"
#include <stdexcept>
#include <utility>

namespace sc13::mods {
ActionWorker::ActionWorker(Handler handler):control_(std::make_shared<Control>()),closeAdmission_(control_->closeAdmission) {
    static_assert(sizeof(ScheduledJob)<=128);
    static_assert(sizeof(Control)<=sizeof(PriorStorage)+256);
    if(!handler)throw std::invalid_argument("Action worker requires a handler");
    control_->handler=std::move(handler);
    thread_=std::jthread([control=control_](std::stop_token stop){Run(control,stop);});
}
ActionWorker::~ActionWorker() {
    {
        const std::scoped_lock lock(control_->mutex);
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        fixture::WorkerRegion region;
#endif
        control_->stopping=true;control_->closeAdmission->store(true);
    }
    // @summary The sole worker unconditionally detaches/disposes pending ownership before clearing busy and joining.
    thread_.request_stop();control_->wake.notify_all();
    if(thread_.joinable())thread_.join();
}
bool ActionWorker::Submit(ModAction action) {
    if(scripting::ScriptRuntime::OnScriptLane())return false;
    std::string status="Preparing mod operation in background...";
    const std::scoped_lock lock(control_->mutex);
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    fixture::WorkerRegion region;
#endif
    if(control_->stopping||control_->closeAdmission->load()||control_->busy.load())return false;
    control_->status.swap(status);
    control_->pending.emplace<ModAction>(std::move(action));control_->busy.store(true);
    control_->wake.notify_one();return true;
}
std::string ActionWorker::Status()const{
    if(scripting::ScriptRuntime::OnScriptLane())return {};
    const std::scoped_lock lock(control_->mutex);
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    fixture::WorkerRegion region;
#endif
    return control_->status;
}
bool ActionWorker::Busy()const noexcept{return control_->busy.load();}
bool ActionWorker::TryCloseObservation(bool& busy)noexcept {
    std::unique_lock lock(control_->mutex,std::try_to_lock);if(!lock.owns_lock())return false;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    fixture::WorkerRegion region;
#endif
    busy=control_->busy.load()||!std::holds_alternative<std::monostate>(control_->pending);return true;
}
void ActionWorker::Run(const std::shared_ptr<Control>& control,std::stop_token stop) {
    for(;;) {
        Pending action;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        std::shared_ptr<const std::function<void()>> barrier;
        std::shared_ptr<const std::function<void(ActionQueueObservation)>> beforeDetach;
#endif
        {
            std::unique_lock lock(control->mutex);
            control->wake.wait(lock,[&]{return control->stopping||stop.stop_requested()||!std::holds_alternative<std::monostate>(control->pending);});
            if(control->stopping||stop.stop_requested())break;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
            ActionQueueObservation observed;
            {fixture::WorkerRegion region;beforeDetach=control->beforeDetach;observed={std::holds_alternative<ScheduledJob>(control->pending),control->busy.load(),control->stopping,control->closeAdmission->load(),fixture::Thread()};}
            if(beforeDetach){
                lock.unlock();
                // @summary Only a bounded observation runs here; an escaped fixture exception fails the test process.
                try{(*beforeDetach)(observed);}catch(...){std::terminate();}
                lock.lock();
                if(control->stopping||stop.stop_requested())break;
                if(std::holds_alternative<std::monostate>(control->pending))continue;
            }
            fixture::WorkerRegion region;
#endif
            action.swap(control->pending);
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
            barrier=control->executionBarrier;
#endif
        }
        std::string result,retiredStatus;
        const bool ui=std::holds_alternative<ModAction>(action);
        try {
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
            if(barrier)(*barrier)();
#endif
            if(ui)result=control->handler(std::get<ModAction>(action));
            else if(auto* job=std::get_if<ScheduledJob>(&action))ManagerCommands::ExecuteScheduled(*job);
        }catch(...){/* @summary Accepted Lua/host work is never replayed. */}
        action.emplace<std::monostate>();
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        barrier.reset();
        beforeDetach.reset();
#endif
        {
            const std::scoped_lock lock(control->mutex);
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
            fixture::WorkerRegion region;
#endif
            if(ui){retiredStatus.swap(control->status);control->status.swap(result);}
        }
        // @summary All operation-local allocation/dependencies die before busy clears, outside W.
        std::string{}.swap(retiredStatus);std::string{}.swap(result);
        {const std::scoped_lock lock(control->mutex);
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        fixture::WorkerRegion region;
#endif
        control->busy.store(false);}
    }
    Pending abandoned;
    Handler handler;
    std::string status;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    std::shared_ptr<const std::function<void()>> barrier;
    std::shared_ptr<const std::function<void(ActionQueueObservation)>> beforeDetach;
#endif
    {
        const std::scoped_lock lock(control->mutex);
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        fixture::WorkerRegion region;
#endif
        control->stopping=true;control->closeAdmission->store(true);
        abandoned.swap(control->pending);handler.swap(control->handler);status.swap(control->status);
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        barrier.swap(control->executionBarrier);
        beforeDetach.swap(control->beforeDetach);
#endif
    }
    abandoned.emplace<std::monostate>();handler={};std::string{}.swap(status);
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    barrier.reset();
    beforeDetach.reset();
#endif
    {const std::scoped_lock lock(control->mutex);
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    fixture::WorkerRegion region;
#endif
    control->busy.store(false);}
}
}
