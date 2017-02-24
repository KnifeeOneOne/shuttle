#include "job_tracker.h"

#include <boost/lexical_cast.hpp>
#include <boost/bind.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/algorithm/string.hpp>
#include <gflags/gflags.h>
#include <algorithm>
#include <iterator>
#include <string>
#include <sstream>
#include <set>
#include <cmath>
#include <sys/time.h>

#include "google/protobuf/repeated_field.h"
#include "logging.h"
#include "proto/minion.pb.h"
#include "resource_manager.h"
#include "master_impl.h"
#include "common/tools_util.h"
#include "timer.h"
#include "sort/sort_file.h"

DECLARE_int32(galaxy_deploy_step);
DECLARE_string(minion_path);
DECLARE_int32(first_sleeptime);
DECLARE_int32(time_tolerance);
DECLARE_int32(replica_num);
DECLARE_int32(replica_begin);
DECLARE_int32(replica_begin_percent);
DECLARE_int32(retry_bound);
DECLARE_int32(left_percent);
DECLARE_int32(max_counters_per_job);
DECLARE_int32(parallel_attempts);

namespace baidu {
namespace shuttle {

JobTracker::JobTracker(MasterImpl* master, ::baidu::galaxy::sdk::AppMaster* galaxy_sdk,
                       const JobDescriptor& job) :
                      master_(master),
                      galaxy_(galaxy_sdk),
                      state_(kPending),
                      map_allow_duplicates_(true),
                      reduce_allow_duplicates_(true),
                      map_(NULL),
                      map_manager_(NULL),
                      map_end_game_begin_(0),
                      map_killed_(0),
                      map_failed_(0),
                      reduce_begin_(0),
                      reduce_(NULL),
                      reduce_manager_(NULL),
                      reduce_end_game_begin_(0),
                      reduce_killed_(0),
                      reduce_failed_(0),
                      monitor_(NULL),
                      map_monitoring_(false),
                      reduce_monitoring_(false),
                      rpc_client_(NULL),
                      fs_(NULL),
                      start_time_(0),
                      finish_time_(0),
                      ignored_map_failures_(0),
                      ignored_reduce_failures_(0) {
    job_descriptor_.CopyFrom(job);
    job_id_ = GenerateJobId();

    if (!job_descriptor_.has_map_retry()) {
        job_descriptor_.set_map_retry(FLAGS_retry_bound);
    }
    if (!job_descriptor_.has_reduce_retry()) {
        job_descriptor_.set_reduce_retry(FLAGS_retry_bound);
    }
    if (job_descriptor_.has_reduce_total() && job_descriptor_.has_reduce_capacity()) {
        if (job_descriptor_.reduce_capacity() > job_descriptor_.reduce_total() * 2) {
            int scale_down_cap = std::max(job_descriptor_.reduce_total() * 2, 60);
            job_descriptor_.set_reduce_capacity(scale_down_cap);
        }
    }
    monitor_ = new ThreadPool(1);

    map_allow_duplicates_ = job_descriptor_.map_allow_duplicates();
    reduce_allow_duplicates_ = job_descriptor_.reduce_allow_duplicates();
}

JobTracker::~JobTracker() {
    // TODO SIGINT will call destruction function and kill jobs on Galaxy
    // Kill();
    if (map_manager_ != NULL) {
        delete map_manager_;
    }
    if (reduce_manager_ != NULL) {
        delete reduce_manager_;
    }
    {
        MutexLock lock(&alloc_mu_);
        for (std::vector<AllocateItem*>::iterator it = allocation_table_.begin();
                it != allocation_table_.end(); ++it) {
            delete *it;
        }
        if (rpc_client_ != NULL) {
            delete rpc_client_;
        }
    }
    if (fs_ != NULL) {
        delete fs_;
    }
}

void JobTracker::BuildOutputFsPointer() {
    FileSystem::Param output_param;
    const DfsInfo& output_dfs = job_descriptor_.output_dfs();
    if (!output_dfs.user().empty() && !output_dfs.password().empty()) {
        output_param["user"] = output_dfs.user();
        output_param["password"] = output_dfs.password();
    }
    if (boost::starts_with(job_descriptor_.output(), "hdfs://")) {
        std::string host;
        int port;
        ParseHdfsAddress(job_descriptor_.output(), &host, &port, NULL);
        output_param["host"] = host;
        output_param["port"] = boost::lexical_cast<std::string>(port);
        job_descriptor_.mutable_output_dfs()->set_host(host);
        job_descriptor_.mutable_output_dfs()->set_port(boost::lexical_cast<std::string>(port));
    } else if (!output_dfs.host().empty() && !output_dfs.port().empty()) {
        output_param["host"] = output_dfs.host();
        output_param["port"] = output_dfs.port();
    }

    fs_ = FileSystem::CreateInfHdfs(output_param);
    output_param_ = output_param;
}

Status JobTracker::BuildResourceManagers() {
    std::vector<std::string> inputs;
    const ::google::protobuf::RepeatedPtrField<std::string>& input_filenames = job_descriptor_.inputs();
    std::copy(input_filenames.begin(), input_filenames.end(), std::back_inserter(inputs));

    FileSystem::Param input_param;
    const DfsInfo& input_dfs = job_descriptor_.input_dfs();
    if(!input_dfs.user().empty() && !input_dfs.password().empty()) {
        input_param["user"] = input_dfs.user();
        input_param["password"] = input_dfs.password();
    }
    if (boost::starts_with(inputs[0], "hdfs://")) {
        std::string host;
        int port;
        ParseHdfsAddress(inputs[0], &host, &port, NULL);
        job_descriptor_.mutable_input_dfs()->set_host(host);
        job_descriptor_.mutable_input_dfs()->set_port(boost::lexical_cast<std::string>(port));
    } else if (!input_dfs.host().empty() && !input_dfs.port().empty()) {
        input_param["host"] = input_dfs.host();
        input_param["port"] = input_dfs.port();
    }

    if (job_descriptor_.input_format() == kNLineInput) {
        map_manager_ = new NLineResourceManager(inputs, input_param);
    } else {
        map_manager_ = new ResourceManager(inputs, input_param, job_descriptor_.split_size());
    }
    int sum_of_map = map_manager_->SumOfItem();
    job_descriptor_.set_map_total(sum_of_map);
    if (job_descriptor_.map_total() < 1) {
        LOG(INFO, "map input may not inexist, failed: %s", job_id_.c_str());
        job_descriptor_.set_reduce_total(0);
        state_ = kFailed;
        return kOpenFileFail;
    }

    if (job_descriptor_.job_type() == kMapReduceJob) {
        reduce_manager_ = new IdManager(job_descriptor_.reduce_total());
    }

    failed_count_.resize(sum_of_map, 0);
    return kOk;
}

void JobTracker::BuildEndGameCounters() {
    if (map_manager_ == NULL) {
        return;
    }
    int sum_of_map = map_manager_->SumOfItem();
    map_end_game_begin_ = sum_of_map - FLAGS_replica_begin;
    int temp = sum_of_map - sum_of_map * FLAGS_replica_begin_percent / 100;
    if (map_end_game_begin_ > temp) {
        map_end_game_begin_ = temp;
    }
    if (reduce_manager_ == NULL) {
        return;
    }
    reduce_begin_ = sum_of_map - sum_of_map * FLAGS_replica_begin_percent / 100;
    reduce_end_game_begin_ = reduce_manager_->SumOfItem() - FLAGS_replica_begin;
    temp = reduce_manager_->SumOfItem() * FLAGS_replica_begin_percent / 100;
    if (reduce_end_game_begin_ < temp) {
        reduce_end_game_begin_ = temp;
    }
}

Status JobTracker::Start() {
    start_time_ = common::timer::now_time();
    BuildOutputFsPointer();
    if (fs_->Exist(job_descriptor_.output())) {
        LOG(INFO, "output exists, failed: %s", job_id_.c_str());
        job_descriptor_.set_map_total(0);
        job_descriptor_.set_reduce_total(0);
        state_ = kFailed;
        return kWriteFileFail;
    }
    if (BuildResourceManagers() != kOk) {
        return kNoMore;
    }
    BuildEndGameCounters();
    rpc_client_ = new RpcClient();
    map_ = new Gru(galaxy_, &job_descriptor_, job_id_,
            (job_descriptor_.job_type() == kMapOnlyJob) ? kMapOnly : kMap);
    if (map_->Start() == kOk) {
        LOG(INFO, "start a new map reduce job: %s -> %s",
                job_descriptor_.name().c_str(), job_id_.c_str());
        return kOk;
    }
    LOG(WARNING, "galaxy report error when submitting a new job: %s",
            job_descriptor_.name().c_str());
    return kGalaxyError;
}

static inline JobPriority ParsePriority(const std::string& priority) {
    return priority == "kMonitor" ? kVeryHigh : (
               priority == "kOnline" ? kHigh : (
                   priority == "kOffline" ? kNormal : (
                       priority == "kBestEffort" ? kLow : kNormal
                   )
               )
           );
}

Status JobTracker::Update(const std::string& priority,
                          int map_capacity,
                          int reduce_capacity) {
    if (map_ != NULL) {
        if (map_->Update(priority, map_capacity) != kOk) {
            return kGalaxyError;
        }
        if (map_capacity != -1) {
            job_descriptor_.set_map_capacity(map_capacity);
        }
        if (!priority.empty()) {
            job_descriptor_.set_priority(ParsePriority(priority));
        }
    }

    if (reduce_ != NULL) {
        if (reduce_->Update(priority, reduce_capacity) != kOk) {
            return kGalaxyError;
        }
        if (reduce_capacity != -1) {
            job_descriptor_.set_reduce_capacity(reduce_capacity);
        }
        if (!priority.empty()) {
            job_descriptor_.set_priority(ParsePriority(priority));
        }
    }
    return kOk;
}

Status JobTracker::Kill(JobState end_state) {
    {
        MutexLock lock(&mu_);
        if (map_ != NULL) {
            LOG(INFO, "map minion finished, kill: %s", job_id_.c_str());
            delete map_;
            map_ = NULL;
        }

        if (reduce_ != NULL) {
            LOG(INFO, "reduce minion finished, kill: %s", job_id_.c_str());
            delete reduce_;
            reduce_ = NULL;
        }
        if (monitor_ != NULL) {
            delete monitor_;
            monitor_ = NULL;
        }

        state_ = end_state;
    }

    MutexLock lock(&alloc_mu_);
    for (std::vector<AllocateItem*>::iterator it = allocation_table_.begin();
            it != allocation_table_.end(); ++it) {
        if ((*it)->state == kTaskRunning) {
            (*it)->state = kTaskKilled;
            (*it)->period = std::time(NULL) - (*it)->alloc_time;
            (*it)->is_map ? ++map_killed_ : ++reduce_killed_;
        }
    }
    finish_time_ =  common::timer::now_time();
    delete rpc_client_;
    rpc_client_ = NULL;
    return kOk;
}

void JobTracker::CanMapDismiss(Status* status, const std::string& endpoint) {
    mu_.AssertHeld();
    int completed = map_manager_->Done();
    int not_done = job_descriptor_.map_total() - completed;
    int map_dismiss_minion_num = job_descriptor_.map_capacity() - (int)
        ::ceil( std::max(not_done, 5) * FLAGS_left_percent / 100.0);
    if (job_descriptor_.map_capacity() > not_done) {
        if ((int)map_dismissed_.size() >= map_dismiss_minion_num) {
            LOG(DEBUG, "assign map: suspend: %s", job_id_.c_str());
            if (status != NULL) {
                *status = kSuspend;
            }
        } else {
            map_dismissed_.insert(endpoint);
            LOG(INFO, "assign map: no more: %s, %s", job_id_.c_str(), endpoint.c_str());
            if (status != NULL) {
                *status = kNoMore;
            }
        }
    } else {
        *status = kSuspend;
    }
}

void JobTracker::CanReduceDismiss(Status* status, const std::string& endpoint) {
    mu_.AssertHeld();
    int completed = reduce_manager_->Done();
    int not_done = job_descriptor_.reduce_total() - completed;
    int reduce_dismiss_minion_num = job_descriptor_.reduce_capacity() - (int)
        ::ceil(std::max(not_done, 5) * FLAGS_left_percent / 100.0);
    if (job_descriptor_.reduce_capacity() > not_done) {
        if ((int)reduce_dismissed_.size() >= reduce_dismiss_minion_num) {
            LOG(DEBUG, "assign reduce: suspend: %s", job_id_.c_str());
            if (status != NULL) {
                *status = kSuspend;
            }
        } else {
            reduce_dismissed_.insert(endpoint);
            LOG(INFO, "assign reduce: no more: %s, %s", job_id_.c_str(), endpoint.c_str());
            if (status != NULL) {
                *status = kNoMore;
            }
        }
    } else {
        *status = kSuspend;
    }
}

ResourceItem* JobTracker::AssignMap(const std::string& endpoint, Status* status) {
    if (state_ == kPending) {
        state_ = kRunning;
    }
    ResourceItem* cur = map_manager_->GetItem();
    if (cur == NULL) {
        MutexLock lock(&alloc_mu_);
        while (!map_slug_.empty() &&
               !map_manager_->IsAllocated(map_slug_.front())) {
            LOG(INFO, "map_slug_.pop(): map_%d", map_slug_.front());
            map_slug_.pop();
        }
        if (map_slug_.empty()) {
            alloc_mu_.Unlock();
            mu_.Lock();
            CanMapDismiss(status, endpoint);
            mu_.Unlock();
            alloc_mu_.Lock();
            return NULL;
        }
        LOG(INFO, "get certain item for: map_%d", map_slug_.front());
        cur = map_manager_->GetCertainItem(map_slug_.front());
        map_slug_.pop();
        if (cur == NULL) {
            alloc_mu_.Unlock();
            mu_.Lock();
            CanMapDismiss(status, endpoint);
            mu_.Unlock();
            alloc_mu_.Lock();
            return NULL;
        }
    } else if (map_allow_duplicates_ && cur->no >= map_end_game_begin_) {
        MutexLock lock(&alloc_mu_);
        for (int i = 0; i < FLAGS_replica_num; ++i) {
            map_slug_.push(cur->no);
        }
    }
    {
        MutexLock lock(&mu_);
        if (cur->no >= map_end_game_begin_ && !map_monitoring_) {
            monitor_->AddTask(boost::bind(&JobTracker::KeepMonitoring, this, true));
            map_monitoring_ = true;
        }
    }
    AllocateItem* alloc = new AllocateItem();
    alloc->endpoint = endpoint;
    alloc->state = kTaskRunning;
    alloc->resource_no = cur->no;
    alloc->attempt = cur->attempt;
    alloc->state = kTaskRunning;
    alloc->is_map = true;
    alloc->alloc_time = std::time(NULL);
    alloc->period = -1;
    MutexLock lock(&alloc_mu_);
    allocation_table_.push_back(alloc);
    map_index_[alloc->resource_no][alloc->attempt] = alloc;
    time_heap_.push(alloc);
    LOG(INFO, "assign map: < no - %d, attempt - %d >, to %s: %s",
            alloc->resource_no, alloc->attempt, endpoint.c_str(), job_id_.c_str());
    if (status != NULL) {
        *status = kOk;
    }
    return cur;
}

IdItem* JobTracker::AssignReduce(const std::string& endpoint, Status* status) {
    if (state_ == kPending) {
        state_ = kRunning;
    }
    IdItem* cur = reduce_manager_->GetItem();
    if (cur == NULL) {
        MutexLock lock(&alloc_mu_);
        while (!reduce_slug_.empty() &&
               !reduce_manager_->IsAllocated(reduce_slug_.front())) {
            reduce_slug_.pop();
        }
        if (reduce_slug_.empty()) {
            alloc_mu_.Unlock();
            mu_.Lock();
            CanReduceDismiss(status, endpoint);
            mu_.Unlock();
            alloc_mu_.Lock();
            return NULL;
        }
        cur = reduce_manager_->GetCertainItem(reduce_slug_.front());
        reduce_slug_.pop();
        if (cur == NULL) {
            alloc_mu_.Unlock();
            mu_.Lock();
            CanReduceDismiss(status, endpoint);
            mu_.Unlock();
            alloc_mu_.Lock();
            return NULL;
        }
    } else if (reduce_allow_duplicates_ && cur->no >= reduce_end_game_begin_) {
        MutexLock lock(&alloc_mu_);
        for (int i = 0; i < FLAGS_replica_num; ++i) {
            reduce_slug_.push(cur->no);
        }
    }
    {
        MutexLock lock(&mu_);
        if (cur->no >= reduce_end_game_begin_ && !reduce_monitoring_) {
            monitor_->AddTask(boost::bind(&JobTracker::KeepMonitoring, this, false));
            reduce_monitoring_ = true;
        }
    }
    AllocateItem* alloc = new AllocateItem();
    alloc->endpoint = endpoint;
    alloc->state = kTaskRunning;
    alloc->resource_no = cur->no;
    alloc->attempt = cur->attempt;
    alloc->state = kTaskRunning;
    alloc->is_map = false;
    alloc->alloc_time = std::time(NULL);
    alloc->period = -1;
    MutexLock lock(&alloc_mu_);
    allocation_table_.push_back(alloc);
    reduce_index_[alloc->resource_no][alloc->attempt] = alloc;
    time_heap_.push(alloc);
    LOG(INFO, "assign reduce: < no - %d, attempt - %d >, to %s: %s",
            alloc->resource_no, alloc->attempt, endpoint.c_str(), job_id_.c_str());
    if (status != NULL) {
        *status = kOk;
    }
    return cur;
}

void JobTracker::CancelOtherAttempts(
        const std::map<int, std::map<int, AllocateItem*> >& lookup_index,
        int no, int attempt) {
    Minion_Stub* stub = NULL;
    MutexLock lock(&alloc_mu_);
    if (rpc_client_ == NULL) {
        return;
    }
    std::map<int, std::map<int, AllocateItem*> >::const_iterator it;
    std::map<int, AllocateItem*>::const_iterator jt;   
    it = lookup_index.find(no);
    if (it != lookup_index.end()) {
        for (jt = it->second.begin(); jt != it->second.end(); jt++) {
            AllocateItem* candidate = jt->second;
            if (candidate->attempt == attempt) {
                continue;
            }
            candidate->state = kTaskCanceled;
            candidate->period = std::time(NULL) - candidate->alloc_time;
            rpc_client_->GetStub(candidate->endpoint, &stub);
            boost::scoped_ptr<Minion_Stub> stub_guard(stub);
            LOG(INFO, "cancel %s task: job:%s, task:%d, attempt:%d",
                candidate->is_map ? "map" : "reduce",
                job_id_.c_str(), candidate->resource_no, candidate->attempt);
            CancelTaskRequest* request = new CancelTaskRequest();
            CancelTaskResponse* response = new CancelTaskResponse();
            request->set_job_id(job_id_);
            request->set_task_id(candidate->resource_no);
            request->set_attempt_id(candidate->attempt);
            boost::function<void (const CancelTaskRequest*, CancelTaskResponse*, bool, int) > callback;
            callback = boost::bind(&JobTracker::CancelCallback, this, _1, _2, _3, _4);
            rpc_client_->AsyncRequest(stub, &Minion_Stub::CancelTask,
                                      request, response, callback , 2, 1);
        }
    }
}

Status JobTracker::FinishMap(int no, int attempt, TaskState state, 
                             const std::string& err_msg,
                             const std::map<std::string, int64_t>& counters) {
    AllocateItem* cur = NULL;
    {
        MutexLock lock(&alloc_mu_);
        std::map<int, std::map<int, AllocateItem*> >::iterator it;
        std::map<int, AllocateItem*>::iterator jt;
        it = map_index_.find(no);
        if (it != map_index_.end()) {
            jt = it->second.find(attempt);
            if (jt != it->second.end()) {
                AllocateItem* candidate = jt->second;
                if (candidate->state == kTaskRunning) {
                    cur = candidate;
                }
            }
        }
    }

    if (cur == NULL) {
        LOG(WARNING, "try to finish an inexist map task: < no - %d, attempt - %d >: %s",
                no, attempt, job_id_.c_str());
        return kNoMore;
    }
    LOG(INFO, "finish a map task: < no - %d, attempt - %d >, state %s: %s",
            cur->resource_no, cur->attempt, TaskState_Name(state).c_str(), job_id_.c_str());
    if (state == kTaskMoveOutputFailed) {
        if (!map_manager_->IsDone(cur->resource_no)) {
            state = kTaskFailed;
        } else {
            state = kTaskCanceled;
        }
    }
    std::string cur_node;
    size_t port_pos = cur->endpoint.find(":");
    if (port_pos != std::string::npos) {
        cur_node = cur->endpoint.substr(0, port_pos);
    }

    bool finished = false;
    {
        MutexLock lock(&mu_);
        if (state == kTaskFailed && 
            ignore_failure_mappers_.find(cur->resource_no) 
               != ignore_failure_mappers_.end()) {
            LOG(WARNING, "make %s,%d to be fake-completed", job_id_.c_str(), 
                cur->resource_no);
            state = kTaskCompleted;
            if (job_descriptor_.job_type() != kMapOnlyJob) {//mapper of map-reduce
                Status w_status;
                SortFileWriter* writer = SortFileWriter::Create(kHdfsFile, &w_status);
                if (w_status == kOk) {
                    std::stringstream ss;
                    ss << job_descriptor_.output()
                       << "/_temporary/shuffle/" 
                       << "map_" << cur->resource_no << "/0.sort";
                    std::string fake_sort_file = ss.str();
                    LOG(WARNING, "make a empty sort file: %s", fake_sort_file.c_str());
                    FileSystem::Param the_param = output_param_;
                    mu_.Unlock();
                    writer->Open(fake_sort_file, the_param);
                    w_status = writer->Close();
                    if (w_status != kOk) {
                        state = kTaskFailed;
                    }
                    mu_.Lock();
                }
                if (writer) {
                    delete writer;
                }
            }
        }
        switch (state) {
        case kTaskCompleted:
            if (!map_manager_->FinishItem(cur->resource_no)) {
                LOG(WARNING, "ignore finish map request: %s, %d",
                        job_id_.c_str(), cur->resource_no);
                state = kTaskCanceled;
                break;
            }
            AccumulateCounters(counters);
            int completed = map_manager_->Done();
            LOG(INFO, "complete a map task(%d/%d): %s",
                    completed, map_manager_->SumOfItem(), job_id_.c_str());
            if (completed == reduce_begin_ && job_descriptor_.job_type() != kMapOnlyJob) {
                LOG(INFO, "map phrase nearly ends, pull up reduce tasks: %s", job_id_.c_str());
                reduce_ = new Gru(galaxy_, &job_descriptor_, job_id_, kReduce);
                if (reduce_->Start() != kOk) {
                    LOG(WARNING, "reduce failed due to galaxy issue: %s", job_id_.c_str());
                    error_msg_ = "Failed to submit job on Galaxy\n";
                    mu_.Unlock();
                    master_->RetractJob(job_id_, kFailed);
                    mu_.Lock();
                    finished = true;
                    state_ = kFailed;
                    break;
                }
            }
            if (completed == map_manager_->SumOfItem()) {
                if (job_descriptor_.job_type() == kMapOnlyJob) {
                    LOG(INFO, "map-only job finish: %s", job_id_.c_str());
                    std::string tmp_work_dir = job_descriptor_.output() + "/_temporary";
                    mu_.Unlock();
                    fs_->Remove(tmp_work_dir);
                    master_->RetractJob(job_id_, kCompleted);
                    mu_.Lock();
                    finished = true;
                    state_ = kCompleted;
                } else {
                    LOG(INFO, "map phrase ends now: %s", job_id_.c_str());
                    failed_count_.resize(0);
                    failed_count_.resize(reduce_manager_->SumOfItem(), 0);
                    failed_nodes_.clear();
                    mu_.Unlock();
                    {
                        MutexLock lock(&alloc_mu_);
                        std::vector<AllocateItem*> rest;
                        while (!time_heap_.empty()) {
                            if (!time_heap_.top()->is_map) {
                                rest.push_back(time_heap_.top());
                            }
                            time_heap_.pop();
                        }
                        for (std::vector<AllocateItem*>::iterator it = rest.begin();
                                it != rest.end(); ++it) {
                            time_heap_.push(*it);
                        }
                    }
                    if (monitor_ != NULL) {
                        monitor_->Stop(false);
                    }
                    mu_.Lock();
                    if (monitor_ != NULL) {
                        delete monitor_;
                    }
                    monitor_ = new ThreadPool(1);
                    if (reduce_monitoring_) {
                        monitor_->AddTask(boost::bind(&JobTracker::KeepMonitoring,
                                    this, false));
                    }
                    if (map_ != NULL) {
                        LOG(INFO, "map minion finished, kill: %s", job_id_.c_str());
                        delete map_;
                        map_ = NULL;
                    }
                }
            }
            break;
        case kTaskFailed:
            map_manager_->ReturnBackItem(cur->resource_no);
            //only increment failed_count when fail on different nodes
            if (failed_nodes_[cur->resource_no].find(cur_node) == failed_nodes_[cur->resource_no].end()) {
                ++ failed_count_[cur->resource_no];
                failed_nodes_[cur->resource_no].insert(cur_node);
                LOG(WARNING, "failed map task: job_id: %s, no: %d, aid: %d, node: %s",
                    job_id_.c_str(), cur->resource_no, cur->attempt, cur_node.c_str());
            }
            ++ map_failed_;
            if (failed_count_[cur->resource_no] >= job_descriptor_.map_retry()) {
                if (ignored_map_failures_ < job_descriptor_.ignore_map_failures()) {
                    ignore_failure_mappers_.insert(cur->resource_no);
                    ignored_map_failures_++;
                    LOG(WARNING, "ignore failure of %s,%d", job_id_.c_str(), cur->resource_no);
                    break;
                }
                LOG(INFO, "map failed, kill job: %s", job_id_.c_str());
                LOG(WARNING, "=== error msg ===");
                LOG(WARNING, "%s", err_msg.c_str());
                error_msg_ = err_msg;
                mu_.Unlock(); 
                master_->RetractJob(job_id_, kFailed);
                mu_.Lock();
                finished = true;
                state_ = kFailed;
            }
            break;
        case kTaskKilled:
            map_manager_->ReturnBackItem(cur->resource_no);
            ++ map_killed_;
            break;
        case kTaskCanceled:
            if (!map_manager_->IsDone(cur->resource_no)) {
                map_manager_->ReturnBackItem(cur->resource_no);
            }
            break;
        default:
            LOG(WARNING, "unfamiliar task finish status: %d", state);
            return kNoMore;
        }
    }
    {
        MutexLock lock(&alloc_mu_);
        cur->state = state;
        cur->period = std::time(NULL) - cur->alloc_time;
        if (map_allow_duplicates_ &&
            (state == kTaskKilled || state == kTaskFailed) ) {
            map_slug_.push(cur->resource_no);
        }
    }

    if (state != kTaskCompleted) {
        return kOk;
    }
    if (!map_allow_duplicates_) {
        return kOk;
    }
    CancelOtherAttempts(map_index_, no, attempt);
    if (finished) {
        MutexLock lock(&alloc_mu_);
        delete rpc_client_;
        rpc_client_ = NULL;
    }
    return kOk;
}

Status JobTracker::FinishReduce(int no, int attempt, TaskState state, 
                                const std::string& err_msg,
                                const std::map<std::string, int64_t>& counters) {
    if (map_manager_ && map_manager_->Done() < job_descriptor_.map_total()
        && state != kTaskKilled) {
        LOG(WARNING, "reduce finish too early, wait a moment");
        return kSuspend;
    }
    AllocateItem* cur = NULL;
    {
        MutexLock lock(&alloc_mu_);
        std::map<int, std::map<int, AllocateItem*> >::iterator it;
        std::map<int, AllocateItem*>::iterator jt;
        it = reduce_index_.find(no);
        if (it != reduce_index_.end()) {
            jt = it->second.find(attempt);
            if (jt != it->second.end()) {
                AllocateItem* candidate = jt->second;
                if (candidate->state == kTaskRunning) {
                    cur = candidate;
                }
            }
        }
    }
    if (cur == NULL) {
        LOG(WARNING, "try to finish an inexist reduce task: < no - %d, attempt - %d >: %s",
                no, attempt, job_id_.c_str());
        return kNoMore;
    }
    LOG(INFO, "finish a reduce task: < no - %d, attempt - %d >, state %s: %s",
            cur->resource_no, cur->attempt, TaskState_Name(state).c_str(), job_id_.c_str());
    if (state == kTaskMoveOutputFailed) {
        if (!reduce_manager_->IsDone(cur->resource_no)) {
            state = kTaskFailed;
        } else {
            state = kTaskCanceled;
        }
    }
    std::string cur_node;
    size_t port_pos = cur->endpoint.find(":");
    if (port_pos != std::string::npos) {
        cur_node = cur->endpoint.substr(0, port_pos);
    }

    bool finished = false;
    {
        MutexLock lock(&mu_);
        if (state == kTaskFailed && 
            ignore_failure_reducers_.find(cur->resource_no) 
               != ignore_failure_reducers_.end()) {
            LOG(WARNING, "make %s,%d to be fake-completed", job_id_.c_str(), 
                cur->resource_no);
            state = kTaskCompleted;
        }
        switch (state) {
        case kTaskCompleted:
            if (!reduce_manager_->FinishItem(cur->resource_no)) {
                LOG(WARNING, "ignore finish reduce request: %s, %d",
                    job_id_.c_str(), cur->resource_no);
                state = kTaskCanceled;
                break;
            }
            AccumulateCounters(counters);
            int completed = reduce_manager_->Done();
            LOG(INFO, "complete a reduce task(%d/%d): %s",
                    completed, reduce_manager_->SumOfItem(), job_id_.c_str());
            if (completed == reduce_manager_->SumOfItem()) {
                LOG(INFO, "map-reduce job finish: %s", job_id_.c_str());
                std::string work_dir = job_descriptor_.output() + "/_temporary";
                LOG(INFO, "remove temp work directory: %s", work_dir.c_str());
                mu_.Unlock();
                if (!fs_->Remove(work_dir)) {
                    LOG(WARNING, "remove temp failed");
                }
                master_->RetractJob(job_id_, kCompleted);
                mu_.Lock();
                finished = true;
                state_ = kCompleted;
            }
            break;
        case kTaskFailed:
            reduce_manager_->ReturnBackItem(cur->resource_no);
            //only increment failed_count when fail on different nodes
            if (failed_nodes_[cur->resource_no].find(cur_node) == failed_nodes_[cur->resource_no].end()) {
                ++ failed_count_[cur->resource_no];
                failed_nodes_[cur->resource_no].insert(cur_node);
                LOG(WARNING, "failed reduce task: job_id: %s, no: %d, aid: %d, node: %s",
                              job_id_.c_str(), cur->resource_no, cur->attempt, cur_node.c_str());
            }
            ++ reduce_failed_;
            if (failed_count_[cur->resource_no] >= job_descriptor_.reduce_retry()) {
                if (ignored_reduce_failures_ < job_descriptor_.ignore_reduce_failures()) {
                    ignore_failure_reducers_.insert(cur->resource_no);
                    ignored_reduce_failures_++;
                    LOG(WARNING, "ignore failure of %s,%d", job_id_.c_str(), cur->resource_no);
                    break;
                }
                LOG(INFO, "reduce failed, kill job: %s", job_id_.c_str());
                LOG(WARNING, "=== error msg ===");
                LOG(WARNING, "%s", err_msg.c_str());
                error_msg_ = err_msg;
                mu_.Unlock();
                master_->RetractJob(job_id_, kFailed);
                mu_.Lock();
                finished = true;
                state_ = kFailed;
            }
            break;
        case kTaskKilled:
            reduce_manager_->ReturnBackItem(cur->resource_no);
            ++ reduce_killed_;
            break;
        case kTaskCanceled:
            if (!reduce_manager_->IsDone(cur->resource_no)) {
                reduce_manager_->ReturnBackItem(cur->resource_no);
            }
            break;
        default:
            LOG(WARNING, "unfamiliar task finish status: %d", state);
            return kNoMore;
        }
    }
    {
        MutexLock lock(&alloc_mu_);
        cur->state = state;
        cur->period = std::time(NULL) - cur->alloc_time;
        if (reduce_allow_duplicates_&&
            (state == kTaskKilled || state == kTaskFailed) ) {
            reduce_slug_.push(cur->resource_no);
        }
    }
    if (state != kTaskCompleted) {
        return kOk;
    }
    if (!reduce_allow_duplicates_) {
        return kOk;
    }
    CancelOtherAttempts(reduce_index_, no, attempt);
    if (finished) {
        MutexLock lock(&alloc_mu_);
        delete rpc_client_;
        rpc_client_ = NULL;
    }
    return kOk;
}

void JobTracker::CancelCallback(const CancelTaskRequest* request, CancelTaskResponse* response, bool fail, int eno) {
    delete request;
    delete response;
    if (fail) {
        LOG(WARNING, "fail to cancel task, err: %d", eno);
    }
}

TaskStatistics JobTracker::GetMapStatistics() {
    int pending = 0, running = 0, completed = 0;
    if (map_manager_ != NULL) {
        pending = map_manager_->Pending();
        running = map_manager_->Allocated();
        completed = map_manager_->Done();
    }
    MutexLock lock(&mu_);
    TaskStatistics task;
    task.set_total(job_descriptor_.map_total());
    task.set_pending(pending);
    task.set_running(running);
    task.set_failed(map_failed_);
    task.set_killed(map_killed_);
    task.set_completed(completed);
    return task;
}

TaskStatistics JobTracker::GetReduceStatistics() {
    int pending = 0, running = 0, completed = 0;
    if (reduce_manager_ != NULL) {
        pending = reduce_manager_->Pending();
        running = reduce_manager_->Allocated();
        completed = reduce_manager_->Done();
    }
    MutexLock lock(&mu_);
    TaskStatistics task;
    task.set_total(job_descriptor_.reduce_total());
    task.set_pending(pending);
    task.set_running(running);
    task.set_failed(reduce_failed_);
    task.set_killed(reduce_killed_);
    task.set_completed(completed);
    return task;
}

void JobTracker::Replay(const std::vector<AllocateItem>& history, std::vector<IdItem>& table, bool is_map) {
    for (size_t i = 0; i < table.size(); ++i) {
        table[i].no = i;
        table[i].attempt = 0;
        table[i].status = kResPending;
        table[i].allocated = 0;
    }
    for (std::vector<AllocateItem>::const_iterator it = history.begin();
            it != history.end(); ++it) {
        if (static_cast<size_t>(it->resource_no) >= table.size() || it->is_map != is_map) {
            continue;
        }
        IdItem& cur = table[it->resource_no];
        cur.attempt = it->attempt;
        //LOG(INFO, "replay: %s_%d: %s", it->is_map? "map": "reduce", it->resource_no, TaskState_Name(it->state).c_str());
        switch(it->state) {
        case kTaskRunning:
            if (cur.status != kResDone) {
                cur.status = kResAllocated;
                ++ cur.allocated;
            }
            break;
        case kTaskCompleted:
            cur.status = kResDone;
            cur.allocated = 0;
            break;
        default: break;
        }
    }
}

bool JobTracker::Load(const std::string& jobid, const JobState state,
                      const std::vector<AllocateItem>& data,
                      const std::vector<ResourceItem>& resource,
                      int32_t start_time,
                      int32_t finish_time) {
    LOG(INFO, "reload job: %s, data.size(): %d", jobid.c_str(), data.size());
    job_id_ = jobid;
    state_ = state;
    start_time_ = start_time;
    finish_time_ = finish_time;
    if (state_ == kRunning || state_ == kPending) {
        BuildOutputFsPointer();
    }
    if (job_descriptor_.map_total() != 0) {
        std::vector<std::string> input;
        FileSystem::Param param;
        map_manager_ = new ResourceManager(input, param, job_descriptor_.split_size());

        std::vector<IdItem> id_data;
        id_data.resize(job_descriptor_.map_total());
        if (resource.size() != id_data.size()) {
            LOG(WARNING, "resource reload fail, %d, %d",resource.size(), id_data.size());
            return false;
        }
        Replay(data, id_data, true);

        std::vector<ResourceItem> res_data;
        res_data.resize(resource.size());
        std::copy(resource.begin(), resource.end(), res_data.begin());
        std::copy(id_data.begin(), id_data.end(), res_data.begin());
        map_manager_->Load(res_data);
    }
    if (job_descriptor_.reduce_total() != 0) {
        reduce_manager_ = new IdManager(job_descriptor_.reduce_total());
        std::vector<IdItem> id_data;
        id_data.resize(reduce_manager_->SumOfItem());
        Replay(data, id_data, false);
        reduce_manager_->Load(id_data);
    }
    BuildEndGameCounters();
    bool is_map = true;
    failed_count_.resize(job_descriptor_.map_total());
    if (map_manager_ && map_manager_->Done() == job_descriptor_.map_total()) {
        is_map = false;
        failed_count_.resize(0);
        failed_count_.resize(job_descriptor_.reduce_total());
    }
    MutexLock lock(&alloc_mu_);
    if (state_ == kRunning) {
        monitor_->AddTask(boost::bind(&JobTracker::KeepMonitoring, this, is_map));
        if (is_map) {
            map_monitoring_ = true;
        } else {
            reduce_monitoring_ = true;
        }
    }
    if (state_ == kRunning || state_ == kPending) {
        rpc_client_ = new RpcClient();
    } else {
        delete monitor_;
        monitor_ = NULL;
    }
    for (std::vector<AllocateItem>::const_iterator it = data.begin();
            it != data.end(); ++it) {
        AllocateItem* alloc = new AllocateItem(*it);
        allocation_table_.push_back(alloc);
        if (alloc->is_map) {
            map_index_[alloc->resource_no][alloc->attempt] = alloc;
        } else {
            reduce_index_[alloc->resource_no][alloc->attempt] = alloc;
        }
        int& cur_killed = alloc->is_map ? map_killed_ : reduce_killed_;
        int& cur_failed = alloc->is_map ? map_failed_ : reduce_failed_;
        switch(alloc->state) {
        case kTaskRunning: time_heap_.push(alloc); break;
        case kTaskFailed: ++ cur_failed; break;
        case kTaskKilled: ++ cur_killed; break;
        default: break;
        }
    }
    return true;
}

const std::vector<AllocateItem> JobTracker::HistoryForDump() {
    MutexLock lock(&alloc_mu_);
    std::vector<AllocateItem> copy;
    for (std::vector<AllocateItem*>::iterator it = allocation_table_.begin();
            it != allocation_table_.end(); ++it) {
        copy.push_back(*(*it));
    }
    return copy;
}

const std::vector<ResourceItem> JobTracker::InputDataForDump() {
    return map_manager_ == NULL ? std::vector<ResourceItem>() : map_manager_->Dump();
}

std::string JobTracker::GenerateJobId() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    const time_t seconds = tv.tv_sec;
    struct tm t;
    localtime_r(&seconds, &t);
    std::stringstream ss;
    char time_buf[32] = { 0 };
    ::strftime(time_buf, 32, "%Y%m%d_%H%M%S", &t);
    ss << "job_" << time_buf << "_"
       << boost::lexical_cast<std::string>(random());
    return ss.str();
}

void JobTracker::KeepMonitoring(bool map_now) {
    // Dynamic determination of delay check
    LOG(INFO, "[monitor] %s monitor starts to check timeout: %s",
            map_now ? "map" : "reduce", job_id_.c_str());
    std::vector<int> time_used;
    bool need_random_query = false;
    {
        MutexLock lock(&alloc_mu_);
        for (std::vector<AllocateItem*>::iterator it = allocation_table_.begin();
                it != allocation_table_.end(); ++it) {
            if ((*it)->is_map == map_now && (*it)->state == kTaskCompleted) {
                time_used.push_back((*it)->period);
            }
        }
        double rn = rand() / (RAND_MAX+0.0);
        LOG(INFO, "random query: %f", rn);
        if (rn < 0.3) {
            need_random_query = true;
            LOG(INFO, "need random query");
        }
    }
    time_t timeout = 0;
    if (!time_used.empty()) {
        std::sort(time_used.begin(), time_used.end());
        timeout = time_used[time_used.size() / 2];
        timeout += timeout / 5;
        LOG(INFO, "[monitor] calc timeout bound, %ld: %s", timeout, job_id_.c_str());
    } else if (!need_random_query){
        monitor_->DelayTask(FLAGS_first_sleeptime * 1000,
                boost::bind(&JobTracker::KeepMonitoring, this, map_now));
        LOG(INFO, "[monitor] will now rest for %ds: %s", FLAGS_first_sleeptime, job_id_.c_str());
        return;
    }
    bool not_allow_duplicates = (map_now && !map_allow_duplicates_ || (!map_now) && !reduce_allow_duplicates_);

    // timeout will NOT be 0 since monitor will be terminated if no tasks is finished
    // sleep_time is always no greater than timeout
    time_t sleep_time = std::min((time_t)FLAGS_time_tolerance, timeout);
    unsigned int counter = 10;
    std::vector<AllocateItem*> returned_item;
    alloc_mu_.Lock();
    time_t now = std::time(NULL);
    while (counter-- != 0 && !time_heap_.empty()) {
        AllocateItem* top = time_heap_.top();
        if (now - top->alloc_time < sleep_time) {
            break;
        }
        time_heap_.pop();
        if (top->state != kTaskRunning) {
            ++ counter;
            continue;
        }
        if (top->is_map != map_now) {
            ++ counter;
            returned_item.push_back(top);
            continue;
        }
        if (not_allow_duplicates || (now - top->alloc_time < timeout) || need_random_query) {
            QueryRequest request;
            QueryResponse response;
            Minion_Stub* stub = NULL;
            rpc_client_->GetStub(top->endpoint, &stub);
            boost::scoped_ptr<Minion_Stub> stub_guard(stub);
            alloc_mu_.Unlock();
            LOG(INFO, "[monitor] query %s with <%d, %d>: %s", top->endpoint.c_str(),
                    top->resource_no, top->attempt, job_id_.c_str());
            bool ok = rpc_client_->SendRequest(stub, &Minion_Stub::Query,
                                               &request, &response, 5, 1);
            alloc_mu_.Lock();
            if (ok && response.job_id() == job_id_ &&
                    response.task_id() == top->resource_no &&
                    response.attempt_id() == top->attempt) {
                ++ counter;
                returned_item.push_back(top);
                continue;
            }
            if (ok && (map_now && !map_manager_->IsAllocated(top->resource_no) ||
                    !map_now && reduce_manager_ != NULL && !reduce_manager_->IsAllocated(top->resource_no))) {
                if (top->state == kTaskRunning) {
                    top->state = kTaskKilled;
                    top->period = std::time(NULL) - top->alloc_time;
                    map_now ? ++map_killed_ : ++reduce_killed_;
                }
                ++ counter;
                continue;
            }
            LOG(INFO, "[monitor] query error, returned %s, <%d, %d>: %s",
                    ok ? "ok" : "error", response.task_id(), response.attempt_id(),
                    job_id_.c_str());
            top->state = kTaskKilled;
            top->period = std::time(NULL) - top->alloc_time;
            map_now ? ++map_killed_ : ++reduce_killed_;
        }
        if (map_now) {
            if (top->attempt >= FLAGS_parallel_attempts - 1
                && top->state == kTaskRunning) {
                ++ counter;
                returned_item.push_back(top); //check again
                if (map_slug_.size() > map_index_.size()) {
                    continue;
                }
            }
            if (top->state == kTaskKilled) {
                map_manager_->ReturnBackItem(top->resource_no);
            }
            map_slug_.push(top->resource_no);
        } else {
            if (top->attempt >= FLAGS_parallel_attempts - 1
                && top->state == kTaskRunning) {
                ++ counter;
                returned_item.push_back(top); //check again
                if (reduce_slug_.size() > reduce_index_.size()) {
                    continue;
                }
            }
            if (top->state == kTaskKilled && reduce_manager_ != NULL) {
                reduce_manager_->ReturnBackItem(top->resource_no);
            }
            reduce_slug_.push(top->resource_no);
        }
        LOG(INFO, "Reallocate a long no-response tasks: < no - %d, attempt - %d>: %s",
                top->resource_no, top->attempt, job_id_.c_str());
        LOG(INFO, "map_slug size: %d, reduce_slug size: %d", map_slug_.size(), reduce_slug_.size());
    }
    for (std::vector<AllocateItem*>::iterator it = returned_item.begin();
            it != returned_item.end(); ++it) {
        time_heap_.push(*it);
    }
    alloc_mu_.Unlock();
    monitor_->DelayTask(sleep_time * 1000,
            boost::bind(&JobTracker::KeepMonitoring, this, map_now));
    LOG(INFO, "[monitor] will now rest for %ds: %s", sleep_time, job_id_.c_str());
}

bool JobTracker::AccumulateCounters(const std::map<std::string, int64_t>& counters){
    mu_.AssertHeld();
    if (counters_.size() > (size_t)FLAGS_max_counters_per_job) {
        LOG(WARNING, "too many counters: %d", counters_.size());
        return false;
    }
    std::map<std::string, int64_t>::const_iterator it;
    for (it = counters.begin(); it != counters.end(); it++) {
        const std::string& key = it->first;
        int64_t value = it->second;
        counters_[key] += value;
    }
    return true;
}

void JobTracker::FillCounters(ShowJobResponse* response) {
    assert(response);
    MutexLock lock(&mu_);
    std::map<std::string, int64_t>::iterator it;
    for (it = counters_.begin(); it != counters_.end(); it++) {
        TaskCounter* counter = response->add_counters();
        counter->set_key(it->first);
        counter->set_value(it->second);
    }
}

}
}

