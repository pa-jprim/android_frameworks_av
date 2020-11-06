/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "TranscodingJobScheduler"

#define VALIDATE_STATE 1

#include <inttypes.h>
#include <media/TranscodingJobScheduler.h>
#include <media/TranscodingUidPolicy.h>
#include <utils/Log.h>

#include <utility>

namespace android {

static_assert((JobIdType)-1 < 0, "JobIdType should be signed");

constexpr static uid_t OFFLINE_UID = -1;

//static
String8 TranscodingJobScheduler::jobToString(const JobKeyType& jobKey) {
    return String8::format("{client:%lld, job:%d}", (long long)jobKey.first, jobKey.second);
}

//static
const char* TranscodingJobScheduler::jobStateToString(const Job::State jobState) {
    switch (jobState) {
    case Job::State::NOT_STARTED:
        return "NOT_STARTED";
    case Job::State::RUNNING:
        return "RUNNING";
    case Job::State::PAUSED:
        return "PAUSED";
    default:
        break;
    }
    return "(unknown)";
}

TranscodingJobScheduler::TranscodingJobScheduler(
        const std::shared_ptr<TranscoderInterface>& transcoder,
        const std::shared_ptr<UidPolicyInterface>& uidPolicy,
        const std::shared_ptr<ResourcePolicyInterface>& resourcePolicy)
      : mTranscoder(transcoder),
        mUidPolicy(uidPolicy),
        mResourcePolicy(resourcePolicy),
        mCurrentJob(nullptr),
        mResourceLost(false) {
    // Only push empty offline queue initially. Realtime queues are added when requests come in.
    mUidSortedList.push_back(OFFLINE_UID);
    mOfflineUidIterator = mUidSortedList.begin();
    mJobQueues.emplace(OFFLINE_UID, JobQueueType());
}

TranscodingJobScheduler::~TranscodingJobScheduler() {}

void TranscodingJobScheduler::dumpAllJobs(int fd, const Vector<String16>& args __unused) {
    String8 result;

    const size_t SIZE = 256;
    char buffer[SIZE];
    std::scoped_lock lock{mLock};

    snprintf(buffer, SIZE, "\n========== Dumping all jobs queues =========\n");
    result.append(buffer);
    snprintf(buffer, SIZE, "  Total num of Jobs: %zu\n", mJobMap.size());
    result.append(buffer);

    std::vector<int32_t> uids(mUidSortedList.begin(), mUidSortedList.end());
    // Exclude last uid, which is for offline queue
    uids.pop_back();
    std::vector<std::string> packageNames;
    if (TranscodingUidPolicy::getNamesForUids(uids, &packageNames)) {
        uids.push_back(OFFLINE_UID);
        packageNames.push_back("(offline)");
    }

    for (int32_t i = 0; i < uids.size(); i++) {
        const uid_t uid = uids[i];

        if (mJobQueues[uid].empty()) {
            continue;
        }
        snprintf(buffer, SIZE, "    Uid: %d, pkg: %s\n", uid,
                 packageNames.empty() ? "(unknown)" : packageNames[i].c_str());
        result.append(buffer);
        snprintf(buffer, SIZE, "      Num of jobs: %zu\n", mJobQueues[uid].size());
        result.append(buffer);
        for (auto& jobKey : mJobQueues[uid]) {
            auto jobIt = mJobMap.find(jobKey);
            if (jobIt == mJobMap.end()) {
                snprintf(buffer, SIZE, "Failed to look up Job %s  \n", jobToString(jobKey).c_str());
                result.append(buffer);
                continue;
            }
            Job& job = jobIt->second;
            TranscodingRequestParcel& request = job.request;
            snprintf(buffer, SIZE, "      Job: %s, %s, %d%%\n", jobToString(jobKey).c_str(),
                     jobStateToString(job.state), job.lastProgress);
            result.append(buffer);
            snprintf(buffer, SIZE, "        Src: %s\n", request.sourceFilePath.c_str());
            result.append(buffer);
            snprintf(buffer, SIZE, "        Dst: %s\n", request.destinationFilePath.c_str());
            result.append(buffer);
        }
    }

    write(fd, result.string(), result.size());
}

TranscodingJobScheduler::Job* TranscodingJobScheduler::getTopJob_l() {
    if (mJobMap.empty()) {
        return nullptr;
    }
    uid_t topUid = *mUidSortedList.begin();
    JobKeyType topJobKey = *mJobQueues[topUid].begin();
    return &mJobMap[topJobKey];
}

void TranscodingJobScheduler::updateCurrentJob_l() {
    Job* topJob = getTopJob_l();
    Job* curJob = mCurrentJob;
    ALOGV("updateCurrentJob: topJob is %s, curJob is %s",
          topJob == nullptr ? "null" : jobToString(topJob->key).c_str(),
          curJob == nullptr ? "null" : jobToString(curJob->key).c_str());

    // If we found a topJob that should be run, and it's not already running,
    // take some actions to ensure it's running.
    if (topJob != nullptr && (topJob != curJob || topJob->state != Job::RUNNING)) {
        // If another job is currently running, pause it first.
        if (curJob != nullptr && curJob->state == Job::RUNNING) {
            mTranscoder->pause(curJob->key.first, curJob->key.second);
            curJob->state = Job::PAUSED;
        }
        // If we are not experiencing resource loss, we can start or resume
        // the topJob now.
        if (!mResourceLost) {
            if (topJob->state == Job::NOT_STARTED) {
                mTranscoder->start(topJob->key.first, topJob->key.second, topJob->request,
                                   topJob->callback.lock());
            } else if (topJob->state == Job::PAUSED) {
                mTranscoder->resume(topJob->key.first, topJob->key.second, topJob->request,
                                    topJob->callback.lock());
            }
            topJob->state = Job::RUNNING;
        }
    }
    mCurrentJob = topJob;
}

void TranscodingJobScheduler::removeJob_l(const JobKeyType& jobKey) {
    ALOGV("%s: job %s", __FUNCTION__, jobToString(jobKey).c_str());

    if (mJobMap.count(jobKey) == 0) {
        ALOGE("job %s doesn't exist", jobToString(jobKey).c_str());
        return;
    }

    // Remove job from uid's queue.
    const uid_t uid = mJobMap[jobKey].uid;
    JobQueueType& jobQueue = mJobQueues[uid];
    auto it = std::find(jobQueue.begin(), jobQueue.end(), jobKey);
    if (it == jobQueue.end()) {
        ALOGE("couldn't find job %s in queue for uid %d", jobToString(jobKey).c_str(), uid);
        return;
    }
    jobQueue.erase(it);

    // If this is the last job in a real-time queue, remove this uid's queue.
    if (uid != OFFLINE_UID && jobQueue.empty()) {
        mUidSortedList.remove(uid);
        mJobQueues.erase(uid);
        mUidPolicy->unregisterMonitorUid(uid);

        std::unordered_set<uid_t> topUids = mUidPolicy->getTopUids();
        moveUidsToTop_l(topUids, false /*preserveTopUid*/);
    }

    // Clear current job.
    if (mCurrentJob == &mJobMap[jobKey]) {
        mCurrentJob = nullptr;
    }

    // Remove job from job map.
    mJobMap.erase(jobKey);
}

/**
 * Moves the set of uids to the front of mUidSortedList (which is used to pick
 * the next job to run).
 *
 * This is called when 1) we received a onTopUidsChanged() callbcak from UidPolicy,
 * or 2) we removed the job queue for a uid because it becomes empty.
 *
 * In case of 1), if there are multiple uids in the set, and the current front
 * uid in mUidSortedList is still in the set, we try to keep that uid at front
 * so that current job run is not interrupted. (This is not a concern for case 2)
 * because the queue for a uid was just removed entirely.)
 */
void TranscodingJobScheduler::moveUidsToTop_l(const std::unordered_set<uid_t>& uids,
                                              bool preserveTopUid) {
    // If uid set is empty, nothing to do. Do not change the queue status.
    if (uids.empty()) {
        return;
    }

    // Save the current top uid.
    uid_t curTopUid = *mUidSortedList.begin();
    bool pushCurTopToFront = false;
    int32_t numUidsMoved = 0;

    // Go through the sorted uid list once, and move the ones in top set to front.
    for (auto it = mUidSortedList.begin(); it != mUidSortedList.end();) {
        uid_t uid = *it;

        if (uid != OFFLINE_UID && uids.count(uid) > 0) {
            it = mUidSortedList.erase(it);

            // If this is the top we're preserving, don't push it here, push
            // it after the for-loop.
            if (uid == curTopUid && preserveTopUid) {
                pushCurTopToFront = true;
            } else {
                mUidSortedList.push_front(uid);
            }

            // If we found all uids in the set, break out.
            if (++numUidsMoved == uids.size()) {
                break;
            }
        } else {
            ++it;
        }
    }

    if (pushCurTopToFront) {
        mUidSortedList.push_front(curTopUid);
    }
}

bool TranscodingJobScheduler::submit(ClientIdType clientId, JobIdType jobId, uid_t uid,
                                     const TranscodingRequestParcel& request,
                                     const std::weak_ptr<ITranscodingClientCallback>& callback) {
    JobKeyType jobKey = std::make_pair(clientId, jobId);

    ALOGV("%s: job %s, uid %d, prioirty %d", __FUNCTION__, jobToString(jobKey).c_str(), uid,
          (int32_t)request.priority);

    std::scoped_lock lock{mLock};

    if (mJobMap.count(jobKey) > 0) {
        ALOGE("job %s already exists", jobToString(jobKey).c_str());
        return false;
    }

    // TODO(chz): only support offline vs real-time for now. All kUnspecified jobs
    // go to offline queue.
    if (request.priority == TranscodingJobPriority::kUnspecified) {
        uid = OFFLINE_UID;
    }

    // Add job to job map.
    mJobMap[jobKey].key = jobKey;
    mJobMap[jobKey].uid = uid;
    mJobMap[jobKey].state = Job::NOT_STARTED;
    mJobMap[jobKey].lastProgress = 0;
    mJobMap[jobKey].request = request;
    mJobMap[jobKey].callback = callback;

    // If it's an offline job, the queue was already added in constructor.
    // If it's a real-time jobs, check if a queue is already present for the uid,
    // and add a new queue if needed.
    if (uid != OFFLINE_UID) {
        if (mJobQueues.count(uid) == 0) {
            mUidPolicy->registerMonitorUid(uid);
            if (mUidPolicy->isUidOnTop(uid)) {
                mUidSortedList.push_front(uid);
            } else {
                // Shouldn't be submitting real-time requests from non-top app,
                // put it in front of the offline queue.
                mUidSortedList.insert(mOfflineUidIterator, uid);
            }
        } else if (uid != *mUidSortedList.begin()) {
            if (mUidPolicy->isUidOnTop(uid)) {
                mUidSortedList.remove(uid);
                mUidSortedList.push_front(uid);
            }
        }
    }
    // Append this job to the uid's queue.
    mJobQueues[uid].push_back(jobKey);

    updateCurrentJob_l();

    validateState_l();
    return true;
}

bool TranscodingJobScheduler::cancel(ClientIdType clientId, JobIdType jobId) {
    JobKeyType jobKey = std::make_pair(clientId, jobId);

    ALOGV("%s: job %s", __FUNCTION__, jobToString(jobKey).c_str());

    std::list<JobKeyType> jobsToRemove;

    std::scoped_lock lock{mLock};

    if (jobId < 0) {
        for (auto it = mJobMap.begin(); it != mJobMap.end(); ++it) {
            if (it->first.first == clientId && it->second.uid != OFFLINE_UID) {
                jobsToRemove.push_back(it->first);
            }
        }
    } else {
        if (mJobMap.count(jobKey) == 0) {
            ALOGE("job %s doesn't exist", jobToString(jobKey).c_str());
            return false;
        }
        jobsToRemove.push_back(jobKey);
    }

    for (auto it = jobsToRemove.begin(); it != jobsToRemove.end(); ++it) {
        // If the job has ever been started, stop it now.
        // Note that stop() is needed even if the job is currently paused. This instructs
        // the transcoder to discard any states for the job, otherwise the states may
        // never be discarded.
        if (mJobMap[*it].state != Job::NOT_STARTED) {
            mTranscoder->stop(it->first, it->second);
        }

        // Remove the job.
        removeJob_l(*it);
    }

    // Start next job.
    updateCurrentJob_l();

    validateState_l();
    return true;
}

bool TranscodingJobScheduler::getJob(ClientIdType clientId, JobIdType jobId,
                                     TranscodingRequestParcel* request) {
    JobKeyType jobKey = std::make_pair(clientId, jobId);

    std::scoped_lock lock{mLock};

    if (mJobMap.count(jobKey) == 0) {
        ALOGE("job %s doesn't exist", jobToString(jobKey).c_str());
        return false;
    }

    *(TranscodingRequest*)request = mJobMap[jobKey].request;
    return true;
}

void TranscodingJobScheduler::notifyClient(ClientIdType clientId, JobIdType jobId,
                                           const char* reason,
                                           std::function<void(const JobKeyType&)> func) {
    JobKeyType jobKey = std::make_pair(clientId, jobId);

    std::scoped_lock lock{mLock};

    if (mJobMap.count(jobKey) == 0) {
        ALOGW("%s: ignoring %s for job %s that doesn't exist", __FUNCTION__, reason,
              jobToString(jobKey).c_str());
        return;
    }

    // Only ignore if job was never started. In particular, propagate the status
    // to client if the job is paused. Transcoder could have posted finish when
    // we're pausing it, and the finish arrived after we changed current job.
    if (mJobMap[jobKey].state == Job::NOT_STARTED) {
        ALOGW("%s: ignoring %s for job %s that was never started", __FUNCTION__, reason,
              jobToString(jobKey).c_str());
        return;
    }

    ALOGV("%s: job %s %s", __FUNCTION__, jobToString(jobKey).c_str(), reason);
    func(jobKey);
}

void TranscodingJobScheduler::onStarted(ClientIdType clientId, JobIdType jobId) {
    notifyClient(clientId, jobId, "started", [=](const JobKeyType& jobKey) {
        auto callback = mJobMap[jobKey].callback.lock();
        if (callback != nullptr) {
            callback->onTranscodingStarted(jobId);
        }
    });
}

void TranscodingJobScheduler::onPaused(ClientIdType clientId, JobIdType jobId) {
    notifyClient(clientId, jobId, "paused", [=](const JobKeyType& jobKey) {
        auto callback = mJobMap[jobKey].callback.lock();
        if (callback != nullptr) {
            callback->onTranscodingPaused(jobId);
        }
    });
}

void TranscodingJobScheduler::onResumed(ClientIdType clientId, JobIdType jobId) {
    notifyClient(clientId, jobId, "resumed", [=](const JobKeyType& jobKey) {
        auto callback = mJobMap[jobKey].callback.lock();
        if (callback != nullptr) {
            callback->onTranscodingResumed(jobId);
        }
    });
}

void TranscodingJobScheduler::onFinish(ClientIdType clientId, JobIdType jobId) {
    notifyClient(clientId, jobId, "finish", [=](const JobKeyType& jobKey) {
        {
            auto clientCallback = mJobMap[jobKey].callback.lock();
            if (clientCallback != nullptr) {
                clientCallback->onTranscodingFinished(
                        jobId, TranscodingResultParcel({jobId, -1 /*actualBitrateBps*/,
                                                        std::nullopt /*jobStats*/}));
            }
        }

        // Remove the job.
        removeJob_l(jobKey);

        // Start next job.
        updateCurrentJob_l();

        validateState_l();
    });
}

void TranscodingJobScheduler::onError(ClientIdType clientId, JobIdType jobId,
                                      TranscodingErrorCode err) {
    notifyClient(clientId, jobId, "error", [=](const JobKeyType& jobKey) {
        {
            auto clientCallback = mJobMap[jobKey].callback.lock();
            if (clientCallback != nullptr) {
                clientCallback->onTranscodingFailed(jobId, err);
            }
        }

        // Remove the job.
        removeJob_l(jobKey);

        // Start next job.
        updateCurrentJob_l();

        validateState_l();
    });
}

void TranscodingJobScheduler::onProgressUpdate(ClientIdType clientId, JobIdType jobId,
                                               int32_t progress) {
    notifyClient(clientId, jobId, "progress", [=](const JobKeyType& jobKey) {
        auto callback = mJobMap[jobKey].callback.lock();
        if (callback != nullptr) {
            callback->onProgressUpdate(jobId, progress);
        }
        mJobMap[jobKey].lastProgress = progress;
    });
}

void TranscodingJobScheduler::onResourceLost() {
    ALOGI("%s", __FUNCTION__);

    std::scoped_lock lock{mLock};

    if (mResourceLost) {
        return;
    }

    // If we receive a resource loss event, the TranscoderLibrary already paused
    // the transcoding, so we don't need to call onPaused to notify it to pause.
    // Only need to update the job state here.
    if (mCurrentJob != nullptr && mCurrentJob->state == Job::RUNNING) {
        mCurrentJob->state = Job::PAUSED;
        // Notify the client as a paused event.
        auto clientCallback = mCurrentJob->callback.lock();
        if (clientCallback != nullptr) {
            clientCallback->onTranscodingPaused(mCurrentJob->key.second);
        }
    }
    mResourceLost = true;

    validateState_l();
}

void TranscodingJobScheduler::onTopUidsChanged(const std::unordered_set<uid_t>& uids) {
    if (uids.empty()) {
        ALOGW("%s: ignoring empty uids", __FUNCTION__);
        return;
    }

    std::string uidStr;
    for (auto it = uids.begin(); it != uids.end(); it++) {
        if (!uidStr.empty()) {
            uidStr += ", ";
        }
        uidStr += std::to_string(*it);
    }

    ALOGD("%s: topUids: size %zu, uids: %s", __FUNCTION__, uids.size(), uidStr.c_str());

    std::scoped_lock lock{mLock};

    moveUidsToTop_l(uids, true /*preserveTopUid*/);

    updateCurrentJob_l();

    validateState_l();
}

void TranscodingJobScheduler::onResourceAvailable() {
    std::scoped_lock lock{mLock};

    if (!mResourceLost) {
        return;
    }

    ALOGI("%s", __FUNCTION__);

    mResourceLost = false;
    updateCurrentJob_l();

    validateState_l();
}

void TranscodingJobScheduler::validateState_l() {
#ifdef VALIDATE_STATE
    LOG_ALWAYS_FATAL_IF(mJobQueues.count(OFFLINE_UID) != 1,
                        "mJobQueues offline queue number is not 1");
    LOG_ALWAYS_FATAL_IF(*mOfflineUidIterator != OFFLINE_UID,
                        "mOfflineUidIterator not pointing to offline uid");
    LOG_ALWAYS_FATAL_IF(mUidSortedList.size() != mJobQueues.size(),
                        "mUidList and mJobQueues size mismatch");

    int32_t totalJobs = 0;
    for (auto uid : mUidSortedList) {
        LOG_ALWAYS_FATAL_IF(mJobQueues.count(uid) != 1, "mJobQueues count for uid %d is not 1",
                            uid);
        for (auto& jobKey : mJobQueues[uid]) {
            LOG_ALWAYS_FATAL_IF(mJobMap.count(jobKey) != 1, "mJobs count for job %s is not 1",
                                jobToString(jobKey).c_str());
        }

        totalJobs += mJobQueues[uid].size();
    }
    LOG_ALWAYS_FATAL_IF(mJobMap.size() != totalJobs,
                        "mJobs size doesn't match total jobs counted from uid queues");
#endif  // VALIDATE_STATE
}

}  // namespace android