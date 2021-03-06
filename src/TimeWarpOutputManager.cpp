#include <limits> // for std::numeric_limits<unsigned int>::max()

#include "utility/memory.hpp"
#include "TimeWarpOutputManager.hpp"

namespace warped {

void TimeWarpOutputManager::initialize(unsigned int num_local_objects) {
    output_queue_ = make_unique<std::vector<std::shared_ptr<Event>> []>(num_local_objects);
    output_queue_lock_ = make_unique<std::mutex []>(num_local_objects);
    num_local_objects_ = num_local_objects;
}

void TimeWarpOutputManager::insertEvent(std::shared_ptr<Event> event,
    unsigned int local_object_id) {
    output_queue_lock_[local_object_id].lock();
    output_queue_[local_object_id].push_back(event);
    output_queue_lock_[local_object_id].unlock();
}

unsigned int TimeWarpOutputManager::fossilCollect(unsigned int gvt, unsigned int local_object_id) {

    unsigned int retval = std::numeric_limits<unsigned int>::max();

    output_queue_lock_[local_object_id].lock();
    if (output_queue_[local_object_id].empty()) {
        output_queue_lock_[local_object_id].unlock();
        return retval;
    }

    auto min = output_queue_[local_object_id].begin();
    while ((min != output_queue_[local_object_id].end()) && (min->get()->timestamp() < gvt)) {
        min = output_queue_[local_object_id].erase(min);
    }

    if (min != output_queue_[local_object_id].end()) {
        retval = min->get()->timestamp();
    }

    output_queue_lock_[local_object_id].unlock();

    return retval;
}

std::unique_ptr<std::vector<std::shared_ptr<Event>>>
TimeWarpOutputManager::removeEventsSentAfter(std::shared_ptr<Event> straggler_event,
    unsigned int local_object_id) {

    auto events_to_cancel = make_unique<std::vector<std::shared_ptr<Event>>>();

    output_queue_lock_[local_object_id].lock();
    if (output_queue_[local_object_id].empty()) {
        output_queue_lock_[local_object_id].unlock();
        return std::move(events_to_cancel);
    }

    auto max = std::prev(output_queue_[local_object_id].end());
    while ((max != output_queue_[local_object_id].begin()) && (*straggler_event < **max)) {
        events_to_cancel->push_back(*max);
        output_queue_[local_object_id].erase(max);
        max = std::prev(output_queue_[local_object_id].end());
    }

    if ((max != output_queue_[local_object_id].begin()) && (*straggler_event < **max)) {
        events_to_cancel->push_back(*max);
        output_queue_[local_object_id].erase(max);
    }

    output_queue_lock_[local_object_id].unlock();

    return std::move(events_to_cancel);
}

std::size_t TimeWarpOutputManager::size(unsigned int local_object_id) {
    return output_queue_[local_object_id].size();
}

} // namespace warped

