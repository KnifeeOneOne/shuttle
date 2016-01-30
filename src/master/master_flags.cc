#include <gflags/gflags.h>

// Used in galaxy_handler.cc
DEFINE_int32(galaxy_deploy_step, 30, "galaxy option to determine the step of deploy");
DEFINE_string(minion_path, "ftp://", "minion ftp path for galaxy to fetch");
DEFINE_string(nexus_server_list, "", "server list for nexus to store meta data");
DEFINE_string(nexus_root_path, "/shuttle/", "root of nexus path, compatible with galaxy nexus system");
DEFINE_string(master_port, "9917", "master listen port");
DEFINE_string(galaxy_address, "0.0.0.0:", "galaxy address for sdk");

// Used in resource_manager.cc
DEFINE_int32(input_block_size, 500 * 1024 * 1024, "max size of input that a single map can get");
DEFINE_int32(parallel_attempts, 5, "max running replica of a certain task");

// Used in gru.cc
DEFINE_int32(replica_begin, 100, "the last tasks that are suitable for end game strategy");
DEFINE_int32(replica_begin_percent, 10, "the last percentage of tasks for end game strategy");
DEFINE_int32(replica_num, 3, "max replicas of a single task");
DEFINE_int32(left_percent, 120, "percentage of left minions when there's no more resource for minion");
DEFINE_int32(first_sleeptime, 10, "timeout bound in seconds for a minion response");
DEFINE_int32(time_tolerance, 120, "longest time interval of the monitor sleep");

// Used in master_impl.cc
// nexus_server_list
DEFINE_bool(recovery, false, "whether fallen into recovery process at the beginning");
// nexus_root_path
DEFINE_string(master_lock_path, "master_lock", "the key used for master to lock");
DEFINE_string(master_path, "master", "the key used for minion to find master");
// master_port
DEFINE_int32(gc_interval, 600, "time interval for master recycling outdated job");
DEFINE_int32(backup_interval, 5000, "millisecond time interval for master backup jobs information");

