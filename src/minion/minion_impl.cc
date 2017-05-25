#include "minion_impl.h"

#include <time.h>
#include <unistd.h>
#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <boost/scoped_ptr.hpp>
#include <cstdlib>
#include <gflags/gflags.h>
#include "logging.h"
#include "proto/app_master.pb.h"

DECLARE_string(master_nexus_path);
DECLARE_string(nexus_addr);
DECLARE_string(work_mode);
DECLARE_string(jobid);
DECLARE_bool(kill_task);
DECLARE_int32(suspend_time);
DECLARE_int64(flow_limit_10gb);
DECLARE_int64(flow_limit_1gb);

using baidu::common::Log;
using baidu::common::FATAL;
using baidu::common::INFO;
using baidu::common::WARNING;

namespace baidu {
namespace shuttle {

const std::string sBreakpointFile = "./task_running";

MinionImpl::MinionImpl() : ins_(FLAGS_nexus_addr),
                           stop_(false),
                           task_frozen_(false),
                           over_loaded_(false),
                           frozen_time_(0) {
    if (FLAGS_work_mode == "map") {
        executor_ = Executor::GetExecutor(kMap);
        work_mode_ =  kMap;
    } else if (FLAGS_work_mode == "reduce") {
        executor_ = Executor::GetExecutor(kReduce);
        work_mode_ = kReduce;
    } else if (FLAGS_work_mode == "map-only") {
        executor_ = Executor::GetExecutor(kMapOnly);
        work_mode_ = kMapOnly;
    } else {
        LOG(FATAL, "unkown work mode: %s", FLAGS_work_mode.c_str());
        abort();
    }
    if (FLAGS_kill_task) {
       galaxy::ins::sdk::SDKError err;
       ins_.Get(FLAGS_master_nexus_path, &master_endpoint_, &err);
       if (err == galaxy::ins::sdk::kOK) {
           Master_Stub* stub;
           rpc_client_.GetStub(master_endpoint_, &stub);
           if (stub != NULL) {
               boost::scoped_ptr<Master_Stub> stub_guard(stub);
               CheckUnfinishedTask(stub);
               _exit(0);
           }
       } else {
           LOG(WARNING, "fail to connect nexus");
       }
    }
    cur_task_id_ = -1;
    cur_attempt_id_ = -1;
    cur_task_state_ = kTaskUnknown;
    watch_dog_.AddTask(boost::bind(&MinionImpl::WatchDogTask, this));
}

MinionImpl::~MinionImpl() {
    delete executor_;
}

void MinionImpl::WatchDogTask() {
    double minute_load = 0.0;
    int numCPU = sysconf( _SC_NPROCESSORS_ONLN );
    FILE* file = fopen("/proc/loadavg", "r");
    if (file == NULL) {
        watch_dog_.DelayTask(1000, boost::bind(&MinionImpl::WatchDogTask, this));
        return;
    }
    fscanf(file, "%lf%*", &minute_load);
    fclose(file);
    int64_t network_limit = FLAGS_flow_limit_10gb;
    if (!netstat_.Is10gb()) {
       network_limit =  FLAGS_flow_limit_1gb;
    }
    if (minute_load > 1.5 * numCPU) {
        LOG(WARNING, "load average: %f, cores: %d", minute_load, numCPU);
        LOG(WARNING, "machine may be overloaded, so froze the task");
        if (!task_frozen_) {
            frozen_time_ = ::time(NULL);
        }
        task_frozen_ = true;
        over_loaded_ =  true;
        system("killall -SIGSTOP input_tool shuffle_tool tuo_merger 2>/dev/null");
    } else if (netstat_.GetSendSpeed() > network_limit ||
               netstat_.GetRecvSpeed() > network_limit) {
        LOG(WARNING, "traffic tx:%lld, rx:%lld",
            netstat_.GetSendSpeed(), netstat_.GetRecvSpeed());
        LOG(WARNING, "network traffic is busy, so froze the task");
        system("killall -SIGSTOP input_tool shuffle_tool tuo_merger 2>/dev/null");
        if (!task_frozen_) {
            frozen_time_ = ::time(NULL);
        }
        task_frozen_ = true;
    } else {
        if (task_frozen_ && minute_load < 0.8 * numCPU) {
            LOG(INFO, "machine seems healthy, so resume the task");
            system("killall -SIGCONT input_tool shuffle_tool tuo_merger 2>/dev/null");
            task_frozen_ = false;
            over_loaded_ = false;
        }
    }
    watch_dog_.DelayTask(1000, boost::bind(&MinionImpl::WatchDogTask, this));
}

void MinionImpl::Query(::google::protobuf::RpcController*,
                       const ::baidu::shuttle::QueryRequest* request,
                       ::baidu::shuttle::QueryResponse* response,
                       ::google::protobuf::Closure* done) {
    MutexLock locker(&mu_);
    if (over_loaded_) {
        done->Run();
        return;
    }
    if (task_frozen_) {
        time_t now = ::time(NULL);
        if (frozen_time_ + 300 < now) {
            done->Run();
            return;
        }
    }
    response->set_job_id(jobid_);
    response->set_task_id(cur_task_id_);
    response->set_attempt_id(cur_attempt_id_);
    response->set_task_state(cur_task_state_);
    if (request->has_detail() && request->detail()) {
        mu_.Unlock();
        TaskInfo task;
        task.set_task_id(response->task_id());
        task.set_attempt_id(response->attempt_id());
        response->set_log_msg(executor_->GetErrorMsg(task, work_mode_ != kReduce));
        mu_.Lock();
    }
    done->Run();
}

void MinionImpl::CancelTask(::google::protobuf::RpcController*,
                            const ::baidu::shuttle::CancelTaskRequest* request,
                            ::baidu::shuttle::CancelTaskResponse* response,
                            ::google::protobuf::Closure* done) {
    int32_t task_id = request->task_id();
    const std::string jobid = request->job_id();
    {
        MutexLock locker(&mu_);
        if (task_id != cur_task_id_ || jobid_ != jobid) {
            response->set_status(kNoSuchTask);
        } else {
            executor_->Stop(task_id);
            response->set_status(kOk);
        }
    }
    done->Run();
}

void MinionImpl::SetEndpoint(const std::string& endpoint) {
    LOG(INFO, "minon bind endpoint on : %s", endpoint.c_str());
    endpoint_ = endpoint;
}

void MinionImpl::SetJobId(const std::string& jobid) {
    LOG(INFO, "minion will work on job: %s", jobid.c_str());
    jobid_ = jobid;
}

void MinionImpl::SleepRandomTime() {
    double rn = rand() / (RAND_MAX+0.0);
    int random_period = static_cast<int>(rn * FLAGS_suspend_time);
    sleep(5 + random_period);
}

void MinionImpl::Loop() {
    srand(time(NULL));
    Master_Stub* stub;
    rpc_client_.GetStub(master_endpoint_, &stub);
    if (stub == NULL) {
        LOG(FATAL, "fail to get master stub");
    }
    boost::scoped_ptr<Master_Stub> stub_guard(stub);
    int task_count = 0;
    CheckUnfinishedTask(stub);
    while (!stop_) {
        LOG(INFO, "======== task:%d ========", ++task_count);
        ::baidu::shuttle::AssignTaskRequest request;
        ::baidu::shuttle::AssignTaskResponse response;
        request.set_endpoint(endpoint_);
        request.set_jobid(jobid_);
        request.set_work_mode(work_mode_);
        LOG(INFO, "endpoint: %s", endpoint_.c_str());
        LOG(INFO, "jobid_: %s", jobid_.c_str());
        while (!stop_) {
            bool ok = rpc_client_.SendRequest(stub, &Master_Stub::AssignTask,
                                              &request, &response, 5, 1);
            if (!ok) {
                LOG(WARNING, "fail to fetch task from master[%s]", master_endpoint_.c_str());
                SleepRandomTime();
                continue;
            } else {
                break;
            }
        }
        if (response.status() == kNoMore) {
            LOG(INFO, "master has no more task for minion, so exit.");
            break;
        } else if (response.status() == kNoSuchJob) {
            LOG(INFO, "the job may be finished.");
            break;
        } else if (response.status() == kSuspend) {
            LOG(INFO, "minion will suspend for a while");
            SleepRandomTime();
            continue;
        } else if (response.status() != kOk) {
            LOG(FATAL, "invalid response status: %s",
                Status_Name(response.status()).c_str());
        }
        const TaskInfo& task = response.task();
        SaveBreakpoint(task);
        executor_->SetEnv(jobid_, task, work_mode_);
        {
            MutexLock locker(&mu_);
            cur_task_id_ = task.task_id();
            cur_attempt_id_ = task.attempt_id();
            cur_task_state_ = kTaskRunning;
        }
        LOG(INFO, "try exec task: %s, %d, %d", jobid_.c_str(), cur_task_id_, cur_attempt_id_);
        TaskState task_state = executor_->Exec(task); //exec here~~
        {
            MutexLock locker(&mu_);
            cur_task_state_ = task_state;
        }
        LOG(INFO, "exec done, task state: %s", TaskState_Name(task_state).c_str());
        std::string error_msg;
        if (task_state == kTaskFailed) {
            error_msg = executor_->GetErrorMsg(task, (work_mode_ != kReduce));
        }

        std::map<std::string, int64_t> counters;
        if (task_state == kTaskCompleted
            && task.job().has_check_counters() && task.job().check_counters()) {
            executor_->ParseCounters(task, &counters, (work_mode_ != kReduce));
        }

        ::baidu::shuttle::FinishTaskRequest fn_request;
        ::baidu::shuttle::FinishTaskResponse fn_response;
        fn_request.set_jobid(jobid_);
        fn_request.set_task_id(task.task_id());
        fn_request.set_attempt_id(task.attempt_id());
        fn_request.set_task_state(task_state);
        fn_request.set_endpoint(endpoint_);
        fn_request.set_work_mode(work_mode_);
        fn_request.set_error_msg(error_msg);

        std::map<std::string, int64_t>::iterator it;
        for (it = counters.begin(); it != counters.end(); it++) {
            const std::string& key = it->first;
            int64_t value = it->second;
            ::baidu::shuttle::TaskCounter* ct = fn_request.add_counters();
            ct->set_key(key);
            ct->set_value(value);
        }

        while (!stop_) {
            bool ok = rpc_client_.SendRequest(stub, &Master_Stub::FinishTask,
                                         &fn_request, &fn_response, 5, 1);
            if (!ok) {
                LOG(WARNING, "fail to send task state to master");
                SleepRandomTime();
                continue;
            } else {
                if (fn_response.status() ==  kSuspend) {
                    LOG(WARNING, "wait a moment and then report finish");
                    SleepRandomTime();
                    continue;
                }
                break;
            }
        }
        ClearBreakpoint();
        if (task_state == kTaskFailed) {
            LOG(WARNING, "task state: %s", TaskState_Name(task_state).c_str());
            executor_->UploadErrorMsg(task, (work_mode_ != kReduce), error_msg);
            SleepRandomTime();
        }
    }

    {
        MutexLock locker(&mu_);
        stop_ = true;
    }
}

bool MinionImpl::IsStop() {
    return stop_;
}

bool MinionImpl::Run() {
    galaxy::ins::sdk::SDKError err;
    ins_.Get(FLAGS_master_nexus_path, &master_endpoint_, &err);
    if (err != galaxy::ins::sdk::kOK) {
        LOG(WARNING, "failed to fetch master endpoint from nexus, errno: %d", err);
        LOG(WARNING, "master_endpoint (%s) -> %s", FLAGS_master_nexus_path.c_str(),
            master_endpoint_.c_str());
        return false;
    }
    pool_.AddTask(boost::bind(&MinionImpl::Loop, this));
    return true;
}

void MinionImpl::CheckUnfinishedTask(Master_Stub* master_stub) {
    FILE* breakpoint = fopen(sBreakpointFile.c_str(), "r");
    int task_id;
    int attempt_id;
    if (breakpoint) {
        int n_ret = fscanf(breakpoint, "%d%d", &task_id, &attempt_id);
        if (n_ret != 2) {
            LOG(WARNING, "invalid breakpoint file");
            return;
        }
        fclose(breakpoint);
        ::baidu::shuttle::FinishTaskRequest fn_request;
        ::baidu::shuttle::FinishTaskResponse fn_response;
        LOG(WARNING, "found unfinished task: task_id: %d, attempt_id: %d", task_id, attempt_id);
        fn_request.set_jobid(FLAGS_jobid);
        fn_request.set_task_id(task_id);
        fn_request.set_attempt_id(attempt_id);
        fn_request.set_task_state(kTaskKilled);
        fn_request.set_endpoint(endpoint_);
        fn_request.set_work_mode(work_mode_);
        bool ok = rpc_client_.SendRequest(master_stub, &Master_Stub::FinishTask,
                                     &fn_request, &fn_response, 5, 1);
        if (!ok) {
            LOG(FATAL, "fail to report unfinished task to master");
            abort();
        }
    }
}

void MinionImpl::SaveBreakpoint(const TaskInfo& task) {
    FILE* breakpoint = fopen(sBreakpointFile.c_str(), "w");
    if (breakpoint) {
        fprintf(breakpoint, "%d %d\n", task.task_id(), task.attempt_id());
        fclose(breakpoint);
    }
}

void MinionImpl::ClearBreakpoint() {
    if (remove(sBreakpointFile.c_str()) != 0 ) {
        LOG(WARNING, "failed to remove breakponit file");
    }
}

}
}
