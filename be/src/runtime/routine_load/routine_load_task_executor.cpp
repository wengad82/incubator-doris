// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "runtime/routine_load/routine_load_task_executor.h"

#include "common/status.h"
#include "runtime/exec_env.h"
#include "runtime/routine_load/data_consumer.h"
#include "runtime/routine_load/kafka_consumer_pipe.h"
#include "runtime/stream_load/stream_load_context.h"
#include "runtime/stream_load/stream_load_executor.h"
#include "util/uid_util.h"

#include "gen_cpp/FrontendService_types.h"
#include "gen_cpp/BackendService_types.h"
#include "gen_cpp/Types_types.h"

namespace doris {

Status RoutineLoadTaskExecutor::submit_task(const TRoutineLoadTask& task) {
    std::unique_lock<std::mutex> l(_lock); 
    if (_task_map.find(task.id) != _task_map.end()) {
        // already submitted
        LOG(INFO) << "routine load task " << task.id << " has already been submitted";
        return Status::OK;
    }

    // create the context
    StreamLoadContext* ctx = new StreamLoadContext(_exec_env);
    ctx->load_type = TLoadType::ROUTINE_LOAD;
    ctx->load_src_type = task.type;
    ctx->job_id = task.job_id;
    ctx->id = UniqueId(task.id);
    ctx->txn_id = task.txn_id;
    ctx->db = task.db;
    ctx->table = task.tbl;
    ctx->label = task.label;
    ctx->auth.auth_code = task.auth_code;

    // the routine load task'txn has alreay began in FE.
    // so it need to rollback if encounter error.
    ctx->need_rollback = true;

    // set source related params
    switch (task.type) {
        case TLoadSourceType::KAFKA:
            ctx->kafka_info = new KafkaLoadInfo(task.kafka_load_info);
            break;
        default:
            LOG(WARNING) << "unknown load source type: " << task.type;
            delete ctx;
            return Status("unknown load source type");
    }

    VLOG(1) << "receive a new routine load task: " << ctx->brief();
    // register the task
    ctx->ref();
    _task_map[ctx->id] = ctx;
    
    // offer the task to thread pool
    if (!_thread_pool->offer(
            boost::bind<void>(&RoutineLoadTaskExecutor::exec_task, this, ctx,
            [this] (StreamLoadContext* ctx) {
                std::unique_lock<std::mutex> l(_lock);
                _task_map.erase(ctx->id);
                LOG(INFO) << "finished routine load task " << ctx->brief();
                if (ctx->unref()) {
                    delete ctx;
                }
            }))) {

        // failed to submit task, clear and return
        LOG(WARNING) << "failed to submit routine load task: " << ctx->brief();
        _task_map.erase(ctx->id);
        if (ctx->unref()) {
            delete ctx;
        }
        return Status("failed to submit routine load task");

    } else {
        LOG(INFO) << "submit a new routine load task: " << ctx->brief()
                  << ", current tasks num: " << _task_map.size();
        return Status::OK;
    }
}

void RoutineLoadTaskExecutor::exec_task(
        StreamLoadContext* ctx, ExecFinishCallback cb) {

    // create pipe and consumer
    std::shared_ptr<StreamLoadPipe> pipe;
    std::shared_ptr<DataConsumer> consumer;
    switch (ctx->load_src_type) {
        case TLoadSourceType::KAFKA:
            pipe = std::make_shared<KafkaConsumerPipe>();
            consumer = std::make_shared<KafkaDataConsumer>(
                    ctx, std::static_pointer_cast<KafkaConsumerPipe>(pipe));
            ctx->body_sink = pipe;
            break;
        default:
            std::stringstream ss;
            ss << "unknown routine load task type: " << ctx->load_type;
            err_handler(ctx, Status::CANCELLED, ss.str());
            cb(ctx);
            return;
    }

#define HANDLE_ERROR(stmt, err_msg) \
    do { \
        Status _status_ = (stmt); \
        if (UNLIKELY(!_status_.ok())) { \
            err_handler(ctx, _status_, err_msg); \
            cb(ctx); \
            return; \
        } \
    } while (false);

    HANDLE_ERROR(consumer->init(), "failed to init consumer");

    // must put pipe before executing plan fragment
    HANDLE_ERROR(_exec_env->load_stream_mgr()->put(ctx->id, pipe), "failed to add pipe");

    // execute plan fragment, async
    HANDLE_ERROR(_exec_env->stream_load_executor()->execute_plan_fragment(ctx),
            "failed to execute plan fragment");

    // start to consume, this may block a while
    HANDLE_ERROR(consumer->start(), "consuming failed");

    // wait for consumer finished
    HANDLE_ERROR(ctx->future.get(), "consume failed");

    ctx->load_cost_nanos = MonotonicNanos() - ctx->start_nanos;
    
    // commit txn
    HANDLE_ERROR(_exec_env->stream_load_executor()->commit_txn(ctx), "commit failed");
    
    cb(ctx);
}

void RoutineLoadTaskExecutor::err_handler(
        StreamLoadContext* ctx,
        const Status& st,
        const std::string& err_msg) {

    LOG(WARNING) << err_msg;
    ctx->status = st;
    if (ctx->need_rollback) {
        _exec_env->stream_load_executor()->rollback_txn(ctx);
        ctx->need_rollback = false;
    }
    if (ctx->body_sink.get() != nullptr) {
        ctx->body_sink->cancel();
    }

    return;
}

} // end namespace
