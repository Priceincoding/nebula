/* Copyright (c) 2018 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#include "base/Base.h"
#include "kvstore/raftex/Host.h"
#include "kvstore/raftex/RaftPart.h"
#include "kvstore/wal/FileBasedWal.h"
#include "network/NetworkUtils.h"
#include <folly/io/async/EventBase.h>
#include <folly/executors/IOThreadPoolExecutor.h>

DEFINE_uint32(max_appendlog_batch_size, 128,
              "The max number of logs in each appendLog request batch");
DEFINE_uint32(max_outstanding_requests, 1024,
              "The max number of outstanding appendLog requests");
DEFINE_int32(raft_rpc_timeout_ms, 500, "rpc timeout for raft client");


namespace nebula {
namespace raftex {

using nebula::network::NetworkUtils;

Host::Host(const HostAddr& addr, std::shared_ptr<RaftPart> part, bool isLearner)
        : part_(std::move(part))
        , addr_(addr)
        , isLearner_(isLearner)
        , idStr_(folly::stringPrintf(
            "%s[Host: %s:%d] ",
            part_->idStr_.c_str(),
            NetworkUtils::intToIPv4(addr_.first).c_str(),
            addr_.second))
        , cachingPromise_(folly::SharedPromise<cpp2::AppendLogResponse>()) {
}


void Host::waitForStop() {
    std::unique_lock<std::mutex> g(lock_);

    CHECK(stopped_);
    noMoreRequestCV_.wait(g, [this] {
        return !requestOnGoing_;
    });
    LOG(INFO) << idStr_ << "The host has been stopped!";
}


cpp2::ErrorCode Host::checkStatus() const {
    CHECK(!lock_.try_lock());
    if (stopped_) {
        VLOG(2) << idStr_ << "The host is stopped, just return";
        return cpp2::ErrorCode::E_HOST_STOPPED;
    }

    if (paused_) {
        VLOG(2) << idStr_
                << "The host is paused, due to losing leadership";
        return cpp2::ErrorCode::E_NOT_A_LEADER;
    }

    return cpp2::ErrorCode::SUCCEEDED;
}


folly::Future<cpp2::AskForVoteResponse> Host::askForVote(
        const cpp2::AskForVoteRequest& req) {
    {
        std::lock_guard<std::mutex> g(lock_);
        auto res = checkStatus();
        if (res != cpp2::ErrorCode::SUCCEEDED) {
            VLOG(2) << idStr_
                    << "The Host is not in a proper status, do not send";
            cpp2::AskForVoteResponse resp;
            resp.set_error_code(res);
            return resp;
        }
    }
    auto client = tcManager().client(addr_);
    return client->future_askForVote(req);
}


folly::Future<cpp2::AppendLogResponse> Host::appendLogs(
        folly::EventBase* eb,
        TermID term,
        LogID logId,
        LogID committedLogId,
        TermID prevLogTerm,
        LogID prevLogId) {
    VLOG(3) << idStr_ << "Entering Host::appendLogs()";

    VLOG(2) << idStr_
            << "Append logs to the host [term = " << term
            << ", logId = " << logId
            << ", committedLogId = " << committedLogId
            << ", lastLogTermSent = " << prevLogTerm
            << ", lastLogIdSent = " << prevLogId
            << "]";

    auto ret = folly::Future<cpp2::AppendLogResponse>::makeEmpty();
    std::shared_ptr<cpp2::AppendLogRequest> req;
    {
        std::lock_guard<std::mutex> g(lock_);

        auto res = checkStatus();

        if (logId == logIdToSend_) {
            // This is a re-send or a heartbeat. If there is an
            // ongoing request, we will just return SUCCEEDED
            if (requestOnGoing_) {
                LOG(INFO) << idStr_ << "Another request is onging,"
                                       "ignore the re-send request";
                cpp2::AppendLogResponse r;
                r.set_error_code(cpp2::ErrorCode::SUCCEEDED);
                return r;
            }
        } else {
            // Otherwise, logId has to be greater
            if (logId < logIdToSend_) {
                LOG(INFO) << idStr_ << "The log has been sended";
                cpp2::AppendLogResponse r;
                r.set_error_code(cpp2::ErrorCode::SUCCEEDED);
                return r;
            }
        }

        if (requestOnGoing_ && res == cpp2::ErrorCode::SUCCEEDED) {
            if (cachingPromise_.size() <= FLAGS_max_outstanding_requests) {
                pendingReq_ = std::make_tuple(term,
                                              logId,
                                              committedLogId,
                                              prevLogTerm,
                                              prevLogId);
                return cachingPromise_.getFuture();
            } else {
                LOG(INFO) << idStr_
                          << "Too many requests are waiting, return error";
                cpp2::AppendLogResponse r;
                r.set_error_code(cpp2::ErrorCode::E_TOO_MANY_REQUESTS);
                return r;
            }
        }

        if (res != cpp2::ErrorCode::SUCCEEDED) {
            VLOG(2) << idStr_
                    << "The host is not in a proper status, just return";
            cpp2::AppendLogResponse r;
            r.set_error_code(res);
            return r;
        }

        VLOG(2) << idStr_ << "About to send the AppendLog request";

        // No request is ongoing, let's send a new request
        CHECK_GE(prevLogTerm, lastLogTermSent_);
        CHECK_GE(prevLogId, lastLogIdSent_);
        logTermToSend_ = term;
        logIdToSend_ = logId;
        lastLogTermSent_ = prevLogTerm;
        lastLogIdSent_ = prevLogId;
        committedLogId_ = committedLogId;
        pendingReq_ = std::make_tuple(0, 0, 0, 0, 0);
        promise_ = std::move(cachingPromise_);
        cachingPromise_ = folly::SharedPromise<cpp2::AppendLogResponse>();
        ret = promise_.getFuture();

        requestOnGoing_ = true;

        req = prepareAppendLogRequest();
    }

    // Get a new promise
    appendLogsInternal(eb, std::move(req));

    return ret;
}

void Host::setResponse(const cpp2::AppendLogResponse& r) {
    CHECK(!lock_.try_lock());
    promise_.setValue(r);
    cachingPromise_.setValue(r);
    cachingPromise_ = folly::SharedPromise<cpp2::AppendLogResponse>();
    pendingReq_ = std::make_tuple(0, 0, 0, 0, 0);
    requestOnGoing_ = false;
}

void Host::appendLogsInternal(folly::EventBase* eb,
                              std::shared_ptr<cpp2::AppendLogRequest> req) {
    sendAppendLogRequest(eb, std::move(req)).via(eb).then(
            [eb, self = shared_from_this()] (folly::Try<cpp2::AppendLogResponse>&& t) {
        VLOG(3) << self->idStr_ << "appendLogs() call got response";
        if (t.hasException()) {
            LOG(ERROR) << self->idStr_ << t.exception().what();
            cpp2::AppendLogResponse r;
            r.set_error_code(cpp2::ErrorCode::E_EXCEPTION);
            {
                std::lock_guard<std::mutex> g(self->lock_);
                self->setResponse(r);
            }
            self->noMoreRequestCV_.notify_all();
            return r;
        }

        cpp2::AppendLogResponse resp = std::move(t).value();
        VLOG(3) << self->idStr_ << "AppendLogResponse "
                << "code " << static_cast<int32_t>(resp.get_error_code())
                << ", currTerm " << resp.get_current_term()
                << ", lastLogId " << resp.get_last_log_id()
                << ", lastLogTerm " << resp.get_last_log_term()
                << ", commitLogId " << resp.get_committed_log_id();
        switch (resp.get_error_code()) {
            case cpp2::ErrorCode::SUCCEEDED: {
                VLOG(2) << self->idStr_
                        << "AppendLog request sent successfully";

                std::shared_ptr<cpp2::AppendLogRequest> newReq;
                cpp2::AppendLogResponse r;
                {
                    std::lock_guard<std::mutex> g(self->lock_);
                    auto res = self->checkStatus();
                    if (res != cpp2::ErrorCode::SUCCEEDED) {
                        VLOG(2) << self->idStr_
                                << "The host is not in a proper status,"
                                   " just return";
                        r.set_error_code(res);
                        self->setResponse(r);
                    } else {
                        self->lastLogIdSent_ = resp.get_last_log_id();
                        self->lastLogTermSent_ = resp.get_last_log_term();
                        if (self->lastLogIdSent_ < self->logIdToSend_) {
                            // More to send
                            VLOG(2) << self->idStr_
                                    << "There are more logs to send";
                            newReq = self->prepareAppendLogRequest();
                        } else {
                            VLOG(2) << self->idStr_
                                    << "Fulfill the promise, size = " << self->promise_.size();
                            // Fulfill the promise
                            self->promise_.setValue(resp);

                            if (self->noRequest()) {
                                VLOG(2) << self->idStr_ << "No request any more!";
                                self->requestOnGoing_ = false;
                            } else {
                                auto& tup = self->pendingReq_;
                                self->logTermToSend_ = std::get<0>(tup);
                                self->logIdToSend_ = std::get<1>(tup);
                                self->committedLogId_ = std::get<2>(tup);
                                VLOG(2) << self->idStr_
                                        << "Sending the pending request in the queue"
                                        << ", from " << self->lastLogIdSent_ + 1
                                        << " to " << self->logIdToSend_;
                                newReq = self->prepareAppendLogRequest();
                                self->promise_ = std::move(self->cachingPromise_);
                                self->cachingPromise_
                                    = folly::SharedPromise<cpp2::AppendLogResponse>();
                                self->pendingReq_ = std::make_tuple(0, 0, 0, 0, 0);
                            }
                        }

                        r = std::move(resp);
                    }
                }

                if (newReq) {
                    self->appendLogsInternal(eb, newReq);
                } else {
                    self->noMoreRequestCV_.notify_all();
                }
                return r;
            }
            case cpp2::ErrorCode::E_LOG_GAP: {
                VLOG(2) << self->idStr_
                        << "The host's log is behind, need to catch up";
                std::shared_ptr<cpp2::AppendLogRequest> newReq;
                cpp2::AppendLogResponse r;
                {
                    std::lock_guard<std::mutex> g(self->lock_);
                    auto res = self->checkStatus();
                    if (res != cpp2::ErrorCode::SUCCEEDED) {
                        VLOG(2) << self->idStr_
                                << "The host is not in a proper status,"
                                   " skip catching up the gap";
                        r.set_error_code(res);
                        self->setResponse(r);
                    } else {
                        self->lastLogIdSent_ = resp.get_last_log_id();
                        self->lastLogTermSent_ = resp.get_last_log_term();
                        newReq = self->prepareAppendLogRequest();
                        r = std::move(resp);
                    }
                }
                if (newReq) {
                    self->appendLogsInternal(eb, newReq);
                } else {
                    self->noMoreRequestCV_.notify_all();
                }
                return r;
            }
            default: {
                PLOG_EVERY_N(ERROR, 100)
                           << self->idStr_
                           << "Failed to append logs to the host (Err: "
                           << static_cast<int32_t>(resp.get_error_code())
                           << ")";
                {
                    std::lock_guard<std::mutex> g(self->lock_);
                    self->setResponse(resp);
                }
                self->noMoreRequestCV_.notify_all();
                return resp;
            }
        }
    });
}


std::shared_ptr<cpp2::AppendLogRequest>
Host::prepareAppendLogRequest() const {
    CHECK(!lock_.try_lock());
    auto req = std::make_shared<cpp2::AppendLogRequest>();
    req->set_space(part_->spaceId());
    req->set_part(part_->partitionId());
    req->set_current_term(logTermToSend_);
    req->set_last_log_id(logIdToSend_);
    req->set_leader_ip(part_->address().first);
    req->set_leader_port(part_->address().second);
    req->set_committed_log_id(committedLogId_);
    req->set_last_log_term_sent(lastLogTermSent_);
    req->set_last_log_id_sent(lastLogIdSent_);

    VLOG(2) << idStr_ << "Prepare AppendLogs request from Log "
                      << lastLogIdSent_ + 1 << " to " << logIdToSend_;
    auto it = part_->wal()->iterator(lastLogIdSent_ + 1, logIdToSend_);
    if (it->valid()) {
        VLOG(2) << idStr_ << "Prepare the list of log entries to send";

        auto term = it->logTerm();
        req->set_log_term(term);

        std::vector<cpp2::LogEntry> logs;
        for (size_t cnt = 0;
             it->valid()
                && it->logTerm() == term
                && cnt < FLAGS_max_appendlog_batch_size;
             ++(*it), ++cnt) {
            cpp2::LogEntry le;
            le.set_cluster(it->logSource());
            le.set_log_str(it->logMsg().toString());
            logs.emplace_back(std::move(le));
        }
        req->set_log_str_list(std::move(logs));
    } else {
        LOG(FATAL) << idStr_ << "We have not support snapshot yet";
    }

    return req;
}


folly::Future<cpp2::AppendLogResponse> Host::sendAppendLogRequest(
        folly::EventBase* eb,
        std::shared_ptr<cpp2::AppendLogRequest> req) {
    VLOG(2) << idStr_ << "Entering Host::sendAppendLogRequest()";

    {
        std::lock_guard<std::mutex> g(lock_);
        auto res = checkStatus();
        if (res != cpp2::ErrorCode::SUCCEEDED) {
            LOG(WARNING) << idStr_
                         << "The Host is not in a proper status, do not send";
            cpp2::AppendLogResponse resp;
            resp.set_error_code(res);
            return resp;
        }
    }

    VLOG(1) << idStr_ << "Sending request space " << req->get_space()
              << ", part " << req->get_part()
              << ", current term " << req->get_current_term()
              << ", last_log_id" << req->get_last_log_id()
              << ", committed_id " << req->get_committed_log_id()
              << ", last_log_term_sent" << req->get_last_log_term_sent()
              << ", last_log_id_sent " << req->get_last_log_id_sent();
    // Get client connection
    auto client = tcManager().client(addr_, eb, false, FLAGS_raft_rpc_timeout_ms);
    return client->future_appendLog(*req);
}

bool Host::noRequest() const {
    CHECK(!lock_.try_lock());
    static auto emptyTup = std::make_tuple(0, 0, 0, 0, 0);
    return pendingReq_ == emptyTup;
}

}  // namespace raftex
}  // namespace nebula

