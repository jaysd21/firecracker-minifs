#pragma once
#include "remote_disk/storage_client.h"

#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace remote_disk::testing {

/* In-memory StorageClient. Completions are delivered on poll(), optionally
 * reversed (out of order) or delayed, and failures can be injected. */
class MockStorage : public StorageClient {
public:
    std::map<uint64_t, std::vector<uint8_t>> objects; /* by object_id_lo */
    std::map<uint64_t, IoStatus> fail_object;          /* injected status */
    size_t queue_limit = 1000;       /* max submitted-not-polled */
    size_t transient_failures = 0;   /* next N completions: kQueueFull */
    bool reverse_order = false;
    bool hold = false;               /* don't complete during poll */
    bool short_reads = false;
    size_t submits = 0;
    std::vector<GetRequest> log;

    bool ready() override { return true; }

    IoStatus submit_get(const GetRequest& r) override
    {
        if (queue_.size() >= queue_limit) return IoStatus::kQueueFull;
        ++submits;
        log.push_back(r);
        queue_.push_back(r);
        return IoStatus::kOk;
    }

    size_t poll(Completion* out, size_t max) override
    {
        if (hold) return 0;
        size_t n = 0;
        while (n < max && !queue_.empty()) {
            GetRequest r = reverse_order ? queue_.back() : queue_.front();
            reverse_order ? queue_.pop_back() : queue_.pop_front();
            out[n++] = finish(r);
        }
        return n;
    }

    void stop() override { queue_.clear(); }
    size_t pending() const { return queue_.size(); }

private:
    Completion finish(const GetRequest& r)
    {
        if (transient_failures > 0) {
            --transient_failures;
            return {r.user_data, IoStatus::kQueueFull, 0};
        }
        auto f = fail_object.find(r.object_id_lo);
        if (f != fail_object.end()) return {r.user_data, f->second, 0};
        auto o = objects.find(r.object_id_lo);
        if (o == objects.end()) return {r.user_data, IoStatus::kNotFound, 0};
        if (r.offset + r.length > o->second.size())
            return {r.user_data, IoStatus::kInvalid, 0};
        std::memcpy(r.dst, o->second.data() + r.offset, r.length);
        return {r.user_data, IoStatus::kOk, short_reads ? r.length - 1 : r.length};
    }
    std::deque<GetRequest> queue_;
};

} // namespace remote_disk::testing
