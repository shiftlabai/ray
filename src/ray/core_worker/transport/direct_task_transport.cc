// Copyright 2017 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ray/core_worker/transport/direct_task_transport.h"

#include "ray/core_worker/transport/dependency_resolver.h"

namespace ray {
namespace core {

Status CoreWorkerDirectTaskSubmitter::SubmitTask(TaskSpecification task_spec) {
  RAY_LOG(DEBUG) << "Submit task " << task_spec.TaskId();
  num_tasks_submitted_++;

  resolver_.ResolveDependencies(task_spec, [this, task_spec](Status status) {
    if (!status.ok()) {
      RAY_LOG(WARNING) << "Resolving task dependencies failed " << status.ToString();
      RAY_UNUSED(task_finisher_->FailOrRetryPendingTask(
          task_spec.TaskId(), rpc::ErrorType::DEPENDENCY_RESOLUTION_FAILED, &status));
      return;
    }
    RAY_LOG(DEBUG) << "Task dependencies resolved " << task_spec.TaskId();
    if (task_spec.IsActorCreationTask()) {
      // If gcs actor management is enabled, the actor creation task will be sent to
      // gcs server directly after the in-memory dependent objects are resolved. For
      // more details please see the protocol of actor management based on gcs.
      // https://docs.google.com/document/d/1EAWide-jy05akJp6OMtDn58XOK7bUyruWMia4E-fV28/edit?usp=sharing
      auto actor_id = task_spec.ActorCreationId();
      auto task_id = task_spec.TaskId();
      RAY_LOG(DEBUG) << "Creating actor via GCS actor id = : " << actor_id;
      RAY_CHECK_OK(actor_creator_->AsyncCreateActor(
          task_spec,
          [this, actor_id, task_id](Status status, const rpc::CreateActorReply &reply) {
            if (status.ok()) {
              RAY_LOG(DEBUG) << "Created actor, actor id = " << actor_id;
              // Copy the actor's reply to the GCS for ref counting purposes.
              rpc::PushTaskReply push_task_reply;
              push_task_reply.mutable_borrowed_refs()->CopyFrom(reply.borrowed_refs());
              task_finisher_->CompletePendingTask(task_id, push_task_reply,
                                                  reply.actor_address());
            } else {
              RAY_LOG(ERROR) << "Failed to create actor " << actor_id
                             << " with status: " << status.ToString();
              RAY_UNUSED(task_finisher_->FailOrRetryPendingTask(
                  task_id, rpc::ErrorType::ACTOR_CREATION_FAILED, &status));
            }
          }));
      return;
    }

    bool keep_executing = true;
    {
      absl::MutexLock lock(&mu_);
      if (cancelled_tasks_.find(task_spec.TaskId()) != cancelled_tasks_.end()) {
        cancelled_tasks_.erase(task_spec.TaskId());
        keep_executing = false;
      }
      if (keep_executing) {
        // Note that the dependencies in the task spec are mutated to only contain
        // plasma dependencies after ResolveDependencies finishes.
        const SchedulingKey scheduling_key(
            task_spec.GetSchedulingClass(), task_spec.GetDependencyIds(),
            task_spec.IsActorCreationTask() ? task_spec.ActorCreationId()
                                            : ActorID::Nil(),
            task_spec.GetRuntimeEnvHash());
        auto &scheduling_key_entry = scheduling_key_entries_[scheduling_key];
        scheduling_key_entry.task_queue.push_back(task_spec);
        scheduling_key_entry.resource_spec = task_spec;

        if (!scheduling_key_entry.AllPipelinesToWorkersFull(
                max_tasks_in_flight_per_worker_)) {
          // The pipelines to the current workers are not full yet, so we don't need more
          // workers.

          // Find a worker with a number of tasks in flight that is less than the maximum
          // value (max_tasks_in_flight_per_worker_) and call OnWorkerIdle to send tasks
          // to that worker
          for (auto active_worker_addr : scheduling_key_entry.active_workers) {
            RAY_CHECK(worker_to_lease_entry_.find(active_worker_addr) !=
                      worker_to_lease_entry_.end());
            auto &lease_entry = worker_to_lease_entry_[active_worker_addr];
            if (!lease_entry.PipelineToWorkerFull(max_tasks_in_flight_per_worker_)) {
              OnWorkerIdle(active_worker_addr, scheduling_key, false,
                           lease_entry.assigned_resources);
              // If we find a worker with a non-full pipeline, all we need to do is to
              // submit the new task to the worker in question by calling OnWorkerIdle
              // once. We don't need to worry about other tasks in the queue because the
              // queue cannot have other tasks in it if there are active workers with
              // non-full pipelines.
              break;
            }
          }
        }
        RequestNewWorkerIfNeeded(scheduling_key);
      }
    }
    if (!keep_executing) {
      RAY_UNUSED(task_finisher_->FailOrRetryPendingTask(
          task_spec.TaskId(), rpc::ErrorType::TASK_CANCELLED, nullptr));
    }
  });
  return Status::OK();
}

void CoreWorkerDirectTaskSubmitter::AddWorkerLeaseClient(
    const rpc::WorkerAddress &addr, std::shared_ptr<WorkerLeaseInterface> lease_client,
    const google::protobuf::RepeatedPtrField<rpc::ResourceMapEntry> &assigned_resources,
    const SchedulingKey &scheduling_key) {
  client_cache_->GetOrConnect(addr.ToProto());
  int64_t expiration = current_time_ms() + lease_timeout_ms_;
  LeaseEntry new_lease_entry =
      LeaseEntry(std::move(lease_client), expiration, assigned_resources, scheduling_key);
  worker_to_lease_entry_.emplace(addr, new_lease_entry);

  auto &scheduling_key_entry = scheduling_key_entries_[scheduling_key];
  RAY_CHECK(scheduling_key_entry.active_workers.emplace(addr).second);
  RAY_CHECK(scheduling_key_entry.active_workers.size() >= 1);
}

void CoreWorkerDirectTaskSubmitter::ReturnWorker(const rpc::WorkerAddress addr,
                                                 bool was_error,
                                                 const SchedulingKey &scheduling_key) {
  RAY_LOG(DEBUG) << "Returning worker " << addr.worker_id << " to raylet "
                 << addr.raylet_id;
  auto &scheduling_key_entry = scheduling_key_entries_[scheduling_key];
  RAY_CHECK(scheduling_key_entry.active_workers.size() >= 1);
  auto &lease_entry = worker_to_lease_entry_[addr];
  RAY_CHECK(lease_entry.lease_client);
  RAY_CHECK(lease_entry.tasks_in_flight == 0);
  RAY_CHECK(lease_entry.WorkerIsStealing() == false);

  // Decrement the number of active workers consuming tasks from the queue associated
  // with the current scheduling_key
  scheduling_key_entry.active_workers.erase(addr);
  if (scheduling_key_entry.CanDelete()) {
    // We can safely remove the entry keyed by scheduling_key from the
    // scheduling_key_entries_ hashmap.
    scheduling_key_entries_.erase(scheduling_key);
  }

  auto status =
      lease_entry.lease_client->ReturnWorker(addr.port, addr.worker_id, was_error);
  if (!status.ok()) {
    RAY_LOG(ERROR) << "Error returning worker to raylet: " << status.ToString();
  }
  worker_to_lease_entry_.erase(addr);
}

bool CoreWorkerDirectTaskSubmitter::FindOptimalVictimForStealing(
    const SchedulingKey &scheduling_key, rpc::WorkerAddress thief_addr,
    rpc::Address *victim_raw_addr) {
  auto &scheduling_key_entry = scheduling_key_entries_[scheduling_key];

  // Check that there is at least one worker (other than the thief) with the current
  // SchedulingKey and that there are stealable tasks
  if (scheduling_key_entry.active_workers.size() <= 1 ||
      !scheduling_key_entry.StealableTasks()) {
    return false;
  }

  // Iterate through the active workers with the relevant SchedulingKey, and select the
  // best one for stealing by updating the victim_raw_addr (pointing to the designated
  // victim) every time we find a candidate that is better than the incumbent. A candidate
  // is better if: (1) the incumbent victim is the thief -- because this choice would be
  // illegal (thief cannot steal from itself), so any alternative choice is better (2) the
  // candidate is not the thief (otherwise, again, it cannot be designated as the victim),
  // and it has more stealable tasks than the incumbent victim
  *victim_raw_addr = scheduling_key_entry.active_workers.begin()->ToProto();

  for (auto candidate_it = scheduling_key_entry.active_workers.begin();
       candidate_it != scheduling_key_entry.active_workers.end(); candidate_it++) {
    const rpc::WorkerAddress &candidate_addr = *candidate_it;
    const auto &candidate_entry = worker_to_lease_entry_[candidate_addr];

    const rpc::WorkerAddress victim_addr = rpc::WorkerAddress(*victim_raw_addr);
    RAY_CHECK(worker_to_lease_entry_.find(victim_addr) != worker_to_lease_entry_.end());
    const auto &victim_entry = worker_to_lease_entry_[victim_addr];

    // Update the designated victim if the alternative candidate is a better choice than
    // the incumbent victim
    if (victim_addr.worker_id == thief_addr.worker_id ||
        ((candidate_entry.tasks_in_flight > victim_entry.tasks_in_flight) &&
         candidate_addr.worker_id != thief_addr.worker_id)) {
      // We copy the candidate's rpc::Address (instead of its rpc::WorkerAddress) because
      // objects of type 'rpc::WorkerAddress' cannot be assigned as their copy
      // assignment operator is implicitly deleted
      *victim_raw_addr = candidate_addr.ToProto();
    }
  }

  const rpc::WorkerAddress victim_addr = rpc::WorkerAddress(*victim_raw_addr);
  // We can't steal unless we can find a thief and a victim with distinct addresses/worker
  // ids. In fact, if we allow stealing among workers with the same address/worker id, we
  // will also necessarily enable self-stealing.
  if ((victim_addr == thief_addr) || victim_addr.worker_id == thief_addr.worker_id) {
    RAY_LOG(DEBUG) << "No victim available with address distinct from thief!";
    RAY_LOG(DEBUG) << "victim_addr.worker_id: " << victim_addr.worker_id
                   << " thief_addr.worker_id: " << thief_addr.worker_id;
    return false;
  }

  const auto &victim_entry = worker_to_lease_entry_[victim_addr];
  // Double check that the victim has the correct SchedulingKey
  RAY_CHECK(victim_entry.scheduling_key == scheduling_key);
  RAY_LOG(DEBUG) << "Victim is worker " << victim_addr.worker_id << " and has "
                 << victim_entry.tasks_in_flight << " tasks in flight, "
                 << " among which we estimate that " << victim_entry.tasks_in_flight / 2
                 << " are available for stealing";
  RAY_CHECK(scheduling_key_entry.total_tasks_in_flight >= victim_entry.tasks_in_flight);

  if ((victim_entry.tasks_in_flight / 2) < 1) {
    RAY_LOG(DEBUG) << "The designated victim does not have enough tasks to steal.";
    return false;
  }

  return true;
}

void CoreWorkerDirectTaskSubmitter::StealTasksOrReturnWorker(
    const rpc::WorkerAddress &thief_addr, bool was_error,
    const SchedulingKey &scheduling_key,
    const google::protobuf::RepeatedPtrField<rpc::ResourceMapEntry> &assigned_resources) {
  auto &thief_entry = worker_to_lease_entry_[thief_addr];
  // Check that the thief still retains its lease_client, and it has no tasks in flights
  RAY_CHECK(thief_entry.lease_client);
  RAY_CHECK(thief_entry.tasks_in_flight == 0);
  RAY_CHECK(thief_entry.WorkerIsStealing() == false);

  // Return the worker if there was an error or the lease has expired.
  if ((was_error || current_time_ms() > thief_entry.lease_expiration_time)) {
    RAY_LOG(DEBUG) << "Returning worker " << thief_addr.worker_id
                   << " due to error or lease expiration";
    ReturnWorker(thief_addr, was_error, scheduling_key);
    return;
  }

  // Search for a suitable victim
  rpc::Address victim_raw_addr;
  if (!FindOptimalVictimForStealing(scheduling_key, thief_addr, &victim_raw_addr)) {
    // If stealing was enabled, we can now cancel any pending new workeer lease request,
    // because stealing is now possible this time.
    if (max_tasks_in_flight_per_worker_ > 1) {
      CancelWorkerLeaseIfNeeded(scheduling_key);
    }
    ReturnWorker(thief_addr, was_error, scheduling_key);
    return;
  }
  // If we get here, stealing must be enabled.
  RAY_CHECK(max_tasks_in_flight_per_worker_ > 1);
  rpc::WorkerAddress victim_addr = rpc::WorkerAddress(victim_raw_addr);
  RAY_CHECK(worker_to_lease_entry_.find(victim_addr) != worker_to_lease_entry_.end());

  thief_entry.SetWorkerIsStealing();

  // By this point, we have ascertained that the victim is available for stealing, so we
  // can go ahead with the RPC
  RAY_LOG(DEBUG) << "Executing StealTasks RPC!";
  rpc::StealTasksRequest request;
  request.mutable_thief_addr()->CopyFrom(thief_addr.ToProto());
  auto &victim_client = *client_cache_->GetOrConnect(victim_addr.ToProto());
  auto victim_wid = victim_addr.worker_id;

  RAY_UNUSED(victim_client.StealTasks(
      request, [this, scheduling_key, victim_wid, victim_addr, thief_addr](
                   Status status, const rpc::StealTasksReply &reply) {
        absl::MutexLock lock(&mu_);

        // Obtain the thief's lease entry (after ensuring that it still exists)
        RAY_CHECK(worker_to_lease_entry_.find(thief_addr) !=
                  worker_to_lease_entry_.end());

        auto &thief_entry = worker_to_lease_entry_[thief_addr];
        RAY_CHECK(thief_entry.WorkerIsStealing());

        // Compute number of tasks stolen
        size_t number_of_tasks_stolen = reply.stolen_tasks_ids_size();
        RAY_LOG(DEBUG) << "We stole " << number_of_tasks_stolen << " tasks "
                       << "from worker: " << victim_wid;

        thief_entry.SetWorkerDoneStealing();

        // push all tasks to the front of the queue
        for (size_t i = 0; i < number_of_tasks_stolen; i++) {
          // Get the task_id of the stolen task, and obtain the corresponding task_spec
          // from the TaskManager
          TaskID stolen_task_id = TaskID::FromBinary(reply.stolen_tasks_ids(i));
          RAY_CHECK(task_finisher_->GetTaskSpec(stolen_task_id));
          auto stolen_task_spec = *(task_finisher_->GetTaskSpec(stolen_task_id));

          // delete the stolen task from the executing_tasks map if it is still there.
          executing_tasks_.erase(stolen_task_id);

          auto &scheduling_key_entry = scheduling_key_entries_[scheduling_key];

          // Add the task to the queue
          RAY_LOG(DEBUG) << "Adding stolen task " << stolen_task_spec.TaskId()
                         << " back to the queue (of current size="
                         << scheduling_key_entry.task_queue.size() << ")!";
          scheduling_key_entry.task_queue.push_front(stolen_task_spec);
        }
        // call OnWorkerIdle to ship the task to the thief
        OnWorkerIdle(thief_addr, scheduling_key, /*error=*/!status.ok(),
                     thief_entry.assigned_resources);
      }));
}

void CoreWorkerDirectTaskSubmitter::OnWorkerIdle(
    const rpc::WorkerAddress &addr, const SchedulingKey &scheduling_key, bool was_error,
    const google::protobuf::RepeatedPtrField<rpc::ResourceMapEntry> &assigned_resources) {
  auto &lease_entry = worker_to_lease_entry_[addr];
  if (!lease_entry.lease_client) {
    return;
  }

  auto &scheduling_key_entry = scheduling_key_entries_[scheduling_key];
  auto &current_queue = scheduling_key_entry.task_queue;
  // Return the worker if there was an error executing the previous task,
  // the lease is expired; Steal or return the worker if there are no more applicable
  // queued tasks and the worker is not stealing.
  if ((was_error || current_time_ms() > lease_entry.lease_expiration_time) ||
      (current_queue.empty() && !lease_entry.WorkerIsStealing())) {
    RAY_CHECK(scheduling_key_entry.active_workers.size() >= 1);

    // Return the worker only if there are no tasks in flight
    if (lease_entry.tasks_in_flight == 0) {
      StealTasksOrReturnWorker(addr, was_error, scheduling_key, assigned_resources);
    }
  } else {
    auto &client = *client_cache_->GetOrConnect(addr.ToProto());

    while (!current_queue.empty() &&
           !lease_entry.PipelineToWorkerFull(max_tasks_in_flight_per_worker_)) {
      auto task_spec = current_queue.front();
      // Increment the number of tasks in flight to the worker
      lease_entry.tasks_in_flight++;

      // Increment the total number of tasks in flight to any worker associated with the
      // current scheduling_key

      RAY_CHECK(scheduling_key_entry.active_workers.size() >= 1);
      scheduling_key_entry.total_tasks_in_flight++;

      executing_tasks_.emplace(task_spec.TaskId(), addr);
      PushNormalTask(addr, client, scheduling_key, task_spec, assigned_resources);
      current_queue.pop_front();
    }
    // If stealing is not an option, we can cancel the request for new worker leases
    if (max_tasks_in_flight_per_worker_ == 1) {
      CancelWorkerLeaseIfNeeded(scheduling_key);
    }
  }
  RequestNewWorkerIfNeeded(scheduling_key);
}

void CoreWorkerDirectTaskSubmitter::CancelWorkerLeaseIfNeeded(
    const SchedulingKey &scheduling_key) {
  auto &scheduling_key_entry = scheduling_key_entries_[scheduling_key];
  auto &task_queue = scheduling_key_entry.task_queue;
  if (!task_queue.empty() || scheduling_key_entry.StealableTasks()) {
    // There are still pending tasks, or there are tasks that can be stolen by a new
    // worker, so let the worker lease request succeed.
    return;
  }

  RAY_LOG(DEBUG)
      << "Task queue is empty, and there are no stealable tasks; canceling lease request";

  for (auto &pending_lease_request : scheduling_key_entry.pending_lease_requests) {
    // There is an in-flight lease request. Cancel it.
    auto lease_client = GetOrConnectLeaseClient(&pending_lease_request.second);
    auto &task_id = pending_lease_request.first;
    RAY_LOG(DEBUG) << "Canceling lease request " << task_id;
    lease_client->CancelWorkerLease(
        task_id, [this, scheduling_key](const Status &status,
                                        const rpc::CancelWorkerLeaseReply &reply) {
          absl::MutexLock lock(&mu_);
          if (status.ok() && !reply.success()) {
            // The cancellation request can fail if the raylet does not have
            // the request queued. This can happen if: a) due to message
            // reordering, the raylet has not yet received the worker lease
            // request, or b) we have already returned the worker lease
            // request. In the former case, we should try the cancellation
            // request again. In the latter case, the in-flight lease request
            // should already have been removed from our local state, so we no
            // longer need to cancel.
            CancelWorkerLeaseIfNeeded(scheduling_key);
          }
        });
  }
}

std::shared_ptr<WorkerLeaseInterface>
CoreWorkerDirectTaskSubmitter::GetOrConnectLeaseClient(
    const rpc::Address *raylet_address) {
  std::shared_ptr<WorkerLeaseInterface> lease_client;
  RAY_CHECK(raylet_address != nullptr);
  if (NodeID::FromBinary(raylet_address->raylet_id()) != local_raylet_id_) {
    // A remote raylet was specified. Connect to the raylet if needed.
    NodeID raylet_id = NodeID::FromBinary(raylet_address->raylet_id());
    auto it = remote_lease_clients_.find(raylet_id);
    if (it == remote_lease_clients_.end()) {
      RAY_LOG(INFO) << "Connecting to raylet " << raylet_id;
      it = remote_lease_clients_
               .emplace(raylet_id, lease_client_factory_(raylet_address->ip_address(),
                                                         raylet_address->port()))
               .first;
    }
    lease_client = it->second;
  } else {
    lease_client = local_lease_client_;
  }

  return lease_client;
}

void CoreWorkerDirectTaskSubmitter::ReportWorkerBacklog() {
  absl::MutexLock lock(&mu_);
  ReportWorkerBacklogInternal();
}

void CoreWorkerDirectTaskSubmitter::ReportWorkerBacklogInternal() {
  absl::flat_hash_map<SchedulingClass, std::pair<TaskSpecification, int64_t>> backlogs;
  for (auto &scheduling_key_and_entry : scheduling_key_entries_) {
    const SchedulingClass scheduling_class = std::get<0>(scheduling_key_and_entry.first);
    if (backlogs.find(scheduling_class) == backlogs.end()) {
      backlogs[scheduling_class].first = scheduling_key_and_entry.second.resource_spec;
      backlogs[scheduling_class].second = 0;
    }
    // We report backlog size per scheduling class not per scheduling key
    // so we need to aggregate backlog sizes of different scheduling keys
    // with the same scheduling class
    backlogs[scheduling_class].second += scheduling_key_and_entry.second.BacklogSize();
    scheduling_key_and_entry.second.last_reported_backlog_size =
        scheduling_key_and_entry.second.BacklogSize();
  }

  std::vector<rpc::WorkerBacklogReport> backlog_reports;
  for (const auto &backlog : backlogs) {
    rpc::WorkerBacklogReport backlog_report;
    backlog_report.mutable_resource_spec()->CopyFrom(backlog.second.first.GetMessage());
    backlog_report.set_backlog_size(backlog.second.second);
    backlog_reports.emplace_back(backlog_report);
  }
  local_lease_client_->ReportWorkerBacklog(WorkerID::FromBinary(rpc_address_.worker_id()),
                                           backlog_reports);
}

void CoreWorkerDirectTaskSubmitter::ReportWorkerBacklogIfNeeded(
    const SchedulingKey &scheduling_key) {
  const auto &scheduling_key_entry = scheduling_key_entries_[scheduling_key];

  if (scheduling_key_entry.last_reported_backlog_size !=
      scheduling_key_entry.BacklogSize()) {
    ReportWorkerBacklogInternal();
  }
}

void CoreWorkerDirectTaskSubmitter::RequestNewWorkerIfNeeded(
    const SchedulingKey &scheduling_key, const rpc::Address *raylet_address) {
  auto &scheduling_key_entry = scheduling_key_entries_[scheduling_key];

  if (scheduling_key_entry.pending_lease_requests.size() ==
      max_pending_lease_requests_per_scheduling_category_) {
    RAY_LOG(DEBUG) << "Exceeding the pending request limit "
                   << max_pending_lease_requests_per_scheduling_category_;
    return;
  }
  RAY_CHECK(scheduling_key_entry.pending_lease_requests.size() <
            max_pending_lease_requests_per_scheduling_category_);

  // Check whether we really need a new worker or whether we have
  // enough room in an existing worker's pipeline to send the new tasks. If the pipelines
  // are not full, we do not request a new worker (unless work stealing is enabled, in
  // which case we can request a worker under the Eager Worker Requesting mode)
  if (!scheduling_key_entry.AllPipelinesToWorkersFull(max_tasks_in_flight_per_worker_) &&
      max_tasks_in_flight_per_worker_ == 1) {
    // The pipelines to the current workers are not full yet, so we don't need more
    // workers.
    return;
  }

  const auto &task_queue = scheduling_key_entry.task_queue;
  // Check if the task queue is empty. If that is the case, it only makes sense to
  // consider requesting a new worker if work stealing is enabled, and there is at least a
  // worker with stealable tasks. If work stealing is not enabled, or there is no tasks
  // that we can steal from existing workers, we don't need a new worker because we don't
  // have any tasks to execute on that worker.
  if (task_queue.empty()) {
    // If any worker has more than one task in flight, then that task can be stolen.
    bool stealable_tasks = scheduling_key_entry.StealableTasks();
    if (!stealable_tasks) {
      if (scheduling_key_entry.CanDelete()) {
        // We can safely remove the entry keyed by scheduling_key from the
        // scheduling_key_entries_ hashmap.
        scheduling_key_entries_.erase(scheduling_key);
      }
      return;
    }
  } else if (scheduling_key_entry.task_queue.size() <=
             scheduling_key_entry.pending_lease_requests.size()) {
    // All tasks have corresponding pending leases, no need to request more
    return;
  }

  num_leases_requested_++;
  // Create a TaskSpecification with an overwritten TaskID to make sure we don't reuse the
  // same TaskID to request a worker
  auto resource_spec_msg = scheduling_key_entry.resource_spec.GetMutableMessage();
  resource_spec_msg.set_task_id(TaskID::FromRandom(job_id_).Binary());
  const TaskSpecification resource_spec = TaskSpecification(resource_spec_msg);
  rpc::Address best_node_address;
  const bool is_spillback = (raylet_address != nullptr);
  if (raylet_address == nullptr) {
    // If no raylet address is given, find the best worker for our next lease request.
    best_node_address = lease_policy_->GetBestNodeForTask(resource_spec);
    raylet_address = &best_node_address;
  }

  auto lease_client = GetOrConnectLeaseClient(raylet_address);
  const TaskID task_id = resource_spec.TaskId();
  RAY_LOG(DEBUG) << "Requesting lease from raylet "
                 << NodeID::FromBinary(raylet_address->raylet_id()) << " for task "
                 << task_id;

  lease_client->RequestWorkerLease(
      resource_spec,
      /*grant_or_reject=*/is_spillback,
      [this, scheduling_key, task_id, is_spillback, raylet_address = *raylet_address](
          const Status &status, const rpc::RequestWorkerLeaseReply &reply) {
        absl::MutexLock lock(&mu_);

        auto &scheduling_key_entry = scheduling_key_entries_[scheduling_key];
        auto lease_client = GetOrConnectLeaseClient(&raylet_address);
        scheduling_key_entry.pending_lease_requests.erase(task_id);

        if (status.ok()) {
          if (reply.canceled()) {
            RAY_LOG(DEBUG) << "Lease canceled for task: " << task_id
                           << ", canceled type: "
                           << rpc::RequestWorkerLeaseReply::SchedulingFailureType_Name(
                                  reply.failure_type());
            if (reply.failure_type() ==
                    rpc::RequestWorkerLeaseReply::
                        SCHEDULING_CANCELLED_RUNTIME_ENV_SETUP_FAILED ||
                reply.failure_type() ==
                    rpc::RequestWorkerLeaseReply::
                        SCHEDULING_CANCELLED_PLACEMENT_GROUP_REMOVED) {
              // We need to actively fail all of the pending tasks in the queue when the
              // placement group was removed or the runtime env failed to be set up. Such
              // an operation is straightforward for the scenario of placement group
              // removal as all tasks in the queue are associated with the same placement
              // group, but in the case of runtime env setup failed, This makes an
              // implicit assumption that runtime_env failures are not transient -- we may
              // consider adding some retries in the future.
              auto &task_queue = scheduling_key_entry.task_queue;
              while (!task_queue.empty()) {
                auto &task_spec = task_queue.front();
                if (reply.failure_type() ==
                    rpc::RequestWorkerLeaseReply::
                        SCHEDULING_CANCELLED_RUNTIME_ENV_SETUP_FAILED) {
                  RAY_UNUSED(task_finisher_->FailPendingTask(
                      task_spec.TaskId(), rpc::ErrorType::RUNTIME_ENV_SETUP_FAILED));
                } else {
                  if (task_spec.IsActorCreationTask()) {
                    RAY_UNUSED(task_finisher_->FailPendingTask(
                        task_spec.TaskId(),
                        rpc::ErrorType::ACTOR_PLACEMENT_GROUP_REMOVED));
                  } else {
                    RAY_UNUSED(task_finisher_->FailPendingTask(
                        task_spec.TaskId(),
                        rpc::ErrorType::TASK_PLACEMENT_GROUP_REMOVED));
                  }
                }
                task_queue.pop_front();
              }
              if (scheduling_key_entry.CanDelete()) {
                scheduling_key_entries_.erase(scheduling_key);
              }
            } else {
              RequestNewWorkerIfNeeded(scheduling_key);
            }
          } else if (reply.rejected()) {
            RAY_LOG(DEBUG) << "Lease rejected " << task_id;
            // It might happen when the first raylet has a stale view
            // of the spillback raylet resources.
            // Retry the request at the first raylet since the resource view may be
            // refreshed.
            RAY_CHECK(is_spillback);
            RequestNewWorkerIfNeeded(scheduling_key);
          } else if (!reply.worker_address().raylet_id().empty()) {
            // We got a lease for a worker. Add the lease client state and try to
            // assign work to the worker.
            rpc::WorkerAddress addr(reply.worker_address());
            RAY_LOG(DEBUG) << "Lease granted to task " << task_id << " from raylet "
                           << addr.raylet_id;

            auto resources_copy = reply.resource_mapping();

            AddWorkerLeaseClient(addr, std::move(lease_client), resources_copy,
                                 scheduling_key);
            RAY_CHECK(scheduling_key_entry.active_workers.size() >= 1);
            OnWorkerIdle(addr, scheduling_key,
                         /*error=*/false, resources_copy);
          } else {
            // The raylet redirected us to a different raylet to retry at.
            RAY_CHECK(!is_spillback);
            RAY_LOG(DEBUG) << "Redirect lease for task " << task_id << " from raylet "
                           << NodeID::FromBinary(raylet_address.raylet_id())
                           << " to raylet "
                           << NodeID::FromBinary(
                                  reply.retry_at_raylet_address().raylet_id());

            RequestNewWorkerIfNeeded(scheduling_key, &reply.retry_at_raylet_address());
          }
        } else if (lease_client != local_lease_client_) {
          // A lease request to a remote raylet failed. Retry locally if the lease is
          // still needed.
          // TODO(swang): Fail after some number of retries?
          RAY_LOG(INFO) << "Retrying attempt to schedule task at remote node. Try again "
                           "on a local node. Error: "
                        << status.ToString();

          RequestNewWorkerIfNeeded(scheduling_key);

        } else {
          if (status.IsGrpcUnavailable()) {
            RAY_LOG(WARNING) << "The worker failed to receive a response from the local "
                             << "raylet because the raylet is unavailable (crashed). "
                             << "Error: " << status;
            if (worker_type_ == WorkerType::WORKER) {
              // Exit the worker so that caller can retry somewhere else.
              RAY_LOG(WARNING) << "Terminating the worker due to local raylet death";
              QuickExit();
            }
            RAY_CHECK(worker_type_ == WorkerType::DRIVER);
            auto &task_queue = scheduling_key_entry.task_queue;
            while (!task_queue.empty()) {
              auto &task_spec = task_queue.front();
              RAY_UNUSED(task_finisher_->FailPendingTask(
                  task_spec.TaskId(), rpc::ErrorType::LOCAL_RAYLET_DIED, &status));
              task_queue.pop_front();
            }
            if (scheduling_key_entry.CanDelete()) {
              scheduling_key_entries_.erase(scheduling_key);
            }
          } else {
            RAY_LOG(WARNING)
                << "The worker failed to receive a response from the local raylet, but "
                   "raylet is still alive. Try again on a local node. Error: "
                << status;
            // TODO(sang): Maybe we should raise FATAL error if it happens too many times.
            RequestNewWorkerIfNeeded(scheduling_key);
          }
        }
      },
      task_queue.size());
  scheduling_key_entry.pending_lease_requests.emplace(task_id, *raylet_address);
  ReportWorkerBacklogIfNeeded(scheduling_key);
}

void CoreWorkerDirectTaskSubmitter::PushNormalTask(
    const rpc::WorkerAddress &addr, rpc::CoreWorkerClientInterface &client,
    const SchedulingKey &scheduling_key, const TaskSpecification &task_spec,
    const google::protobuf::RepeatedPtrField<rpc::ResourceMapEntry> &assigned_resources) {
  RAY_LOG(DEBUG) << "Pushing task " << task_spec.TaskId() << " to worker "
                 << addr.worker_id << " of raylet " << addr.raylet_id;
  auto task_id = task_spec.TaskId();
  auto request = std::make_unique<rpc::PushTaskRequest>();
  bool is_actor = task_spec.IsActorTask();
  bool is_actor_creation = task_spec.IsActorCreationTask();

  // NOTE(swang): CopyFrom is needed because if we use Swap here and the task
  // fails, then the task data will be gone when the TaskManager attempts to
  // access the task.
  request->mutable_task_spec()->CopyFrom(task_spec.GetMessage());
  request->mutable_resource_mapping()->CopyFrom(assigned_resources);
  request->set_intended_worker_id(addr.worker_id.Binary());
  client.PushNormalTask(
      std::move(request),
      [this, task_spec, task_id, is_actor, is_actor_creation, scheduling_key, addr,
       assigned_resources](Status status, const rpc::PushTaskReply &reply) {
        {
          RAY_LOG(DEBUG) << "Task " << task_id << " finished from worker "
                         << addr.worker_id << " of raylet " << addr.raylet_id;
          absl::MutexLock lock(&mu_);
          executing_tasks_.erase(task_id);

          // Decrement the number of tasks in flight to the worker
          auto &lease_entry = worker_to_lease_entry_[addr];
          RAY_CHECK(lease_entry.tasks_in_flight > 0);
          lease_entry.tasks_in_flight--;

          // Decrement the total number of tasks in flight to any worker with the current
          // scheduling_key.
          auto &scheduling_key_entry = scheduling_key_entries_[scheduling_key];
          RAY_CHECK(scheduling_key_entry.active_workers.size() >= 1);
          RAY_CHECK(scheduling_key_entry.total_tasks_in_flight >= 1);
          scheduling_key_entry.total_tasks_in_flight--;

          if (reply.worker_exiting()) {
            RAY_LOG(DEBUG) << "Worker " << addr.worker_id
                           << " replied that it is exiting.";
            // The worker is draining and will shutdown after it is done. Don't return
            // it to the Raylet since that will kill it early.
            worker_to_lease_entry_.erase(addr);
            auto &scheduling_key_entry = scheduling_key_entries_[scheduling_key];
            scheduling_key_entry.active_workers.erase(addr);
            if (scheduling_key_entry.CanDelete()) {
              // We can safely remove the entry keyed by scheduling_key from the
              // scheduling_key_entries_ hashmap.
              scheduling_key_entries_.erase(scheduling_key);
            }
          } else if (reply.task_stolen()) {
            // If the task was stolen, we push it to the thief worker & call OnWorkerIdle
            // in the StealTasks callback within StealTasksOrReturnWorker. So we don't
            // need to do anything here.
            return;
          } else if (!status.ok() || !is_actor_creation) {
            // Successful actor creation leases the worker indefinitely from the raylet.
            OnWorkerIdle(addr, scheduling_key,
                         /*error=*/!status.ok(), assigned_resources);
          }
        }
        if (!status.ok()) {
          // TODO: It'd be nice to differentiate here between process vs node
          // failure (e.g., by contacting the raylet). If it was a process
          // failure, it may have been an application-level error and it may
          // not make sense to retry the task.
          RAY_UNUSED(task_finisher_->FailOrRetryPendingTask(
              task_id,
              is_actor ? rpc::ErrorType::ACTOR_DIED : rpc::ErrorType::WORKER_DIED,
              &status));
        } else {
          if (!task_spec.GetMessage().retry_exceptions() ||
              !reply.is_application_level_error() ||
              !task_finisher_->RetryTaskIfPossible(task_id)) {
            task_finisher_->CompletePendingTask(task_id, reply, addr.ToProto());
          }
        }
      });
}

Status CoreWorkerDirectTaskSubmitter::CancelTask(TaskSpecification task_spec,
                                                 bool force_kill, bool recursive) {
  RAY_LOG(INFO) << "Cancelling a task: " << task_spec.TaskId()
                << " force_kill: " << force_kill << " recursive: " << recursive;
  const SchedulingKey scheduling_key(
      task_spec.GetSchedulingClass(), task_spec.GetDependencyIds(),
      task_spec.IsActorCreationTask() ? task_spec.ActorCreationId() : ActorID::Nil(),
      task_spec.GetRuntimeEnvHash());
  std::shared_ptr<rpc::CoreWorkerClientInterface> client = nullptr;
  {
    absl::MutexLock lock(&mu_);
    if (cancelled_tasks_.find(task_spec.TaskId()) != cancelled_tasks_.end() ||
        !task_finisher_->MarkTaskCanceled(task_spec.TaskId())) {
      return Status::OK();
    }

    auto &scheduling_key_entry = scheduling_key_entries_[scheduling_key];
    auto &scheduled_tasks = scheduling_key_entry.task_queue;
    // This cancels tasks that have completed dependencies and are awaiting
    // a worker lease.
    if (!scheduled_tasks.empty()) {
      for (auto spec = scheduled_tasks.begin(); spec != scheduled_tasks.end(); spec++) {
        if (spec->TaskId() == task_spec.TaskId()) {
          scheduled_tasks.erase(spec);

          if (scheduled_tasks.empty()) {
            CancelWorkerLeaseIfNeeded(scheduling_key);
          }
          RAY_UNUSED(task_finisher_->FailOrRetryPendingTask(
              task_spec.TaskId(), rpc::ErrorType::TASK_CANCELLED, nullptr));
          return Status::OK();
        }
      }
    }

    // This will get removed either when the RPC call to cancel is returned
    // or when all dependencies are resolved.
    RAY_CHECK(cancelled_tasks_.emplace(task_spec.TaskId()).second);
    auto rpc_client = executing_tasks_.find(task_spec.TaskId());

    if (rpc_client == executing_tasks_.end()) {
      // This case is reached for tasks that have unresolved dependencies.
      // No executing tasks, so cancelling is a noop.
      if (scheduling_key_entry.CanDelete()) {
        // We can safely remove the entry keyed by scheduling_key from the
        // scheduling_key_entries_ hashmap.
        scheduling_key_entries_.erase(scheduling_key);
      }
      return Status::OK();
    }
    // Looks for an RPC handle for the worker executing the task.
    auto maybe_client = client_cache_->GetByID(rpc_client->second.worker_id);
    if (!maybe_client.has_value()) {
      // If we don't have a connection to that worker, we can't cancel it.
      // This case is reached for tasks that have unresolved dependencies.
      return Status::OK();
    }
    client = maybe_client.value();
  }

  RAY_CHECK(client != nullptr);

  auto request = rpc::CancelTaskRequest();
  request.set_intended_task_id(task_spec.TaskId().Binary());
  request.set_force_kill(force_kill);
  request.set_recursive(recursive);
  client->CancelTask(
      request, [this, task_spec, scheduling_key, force_kill, recursive](
                   const Status &status, const rpc::CancelTaskReply &reply) {
        absl::MutexLock lock(&mu_);
        cancelled_tasks_.erase(task_spec.TaskId());

        if (status.ok() && !reply.attempt_succeeded()) {
          if (cancel_retry_timer_.has_value()) {
            if (cancel_retry_timer_->expiry().time_since_epoch() <=
                std::chrono::high_resolution_clock::now().time_since_epoch()) {
              cancel_retry_timer_->expires_after(boost::asio::chrono::milliseconds(
                  RayConfig::instance().cancellation_retry_ms()));
            }
            cancel_retry_timer_->async_wait(
                boost::bind(&CoreWorkerDirectTaskSubmitter::CancelTask, this, task_spec,
                            force_kill, recursive));
          }
        }
        // Retry is not attempted if !status.ok() because force-kill may kill the worker
        // before the reply is sent.
      });
  return Status::OK();
}

Status CoreWorkerDirectTaskSubmitter::CancelRemoteTask(const ObjectID &object_id,
                                                       const rpc::Address &worker_addr,
                                                       bool force_kill, bool recursive) {
  auto maybe_client = client_cache_->GetByID(rpc::WorkerAddress(worker_addr).worker_id);

  if (!maybe_client.has_value()) {
    return Status::Invalid("No remote worker found");
  }
  auto client = maybe_client.value();
  auto request = rpc::RemoteCancelTaskRequest();
  request.set_force_kill(force_kill);
  request.set_recursive(recursive);
  request.set_remote_object_id(object_id.Binary());
  client->RemoteCancelTask(request, nullptr);
  return Status::OK();
}

}  // namespace core
}  // namespace ray
