#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>

#include "sst/multicast_msg.h"
#include "sst/sst.h"
#include "derecho_internal.h"

namespace derecho {

using ip_addr = std::string;
using node_id_t = uint32_t;

using sst::SSTField;
using sst::SSTFieldVector;
/**
 * The GMS and derecho_group will share the same SST for efficiency. This class
 * defines all the fields in this SST.
 */
class DerechoSST : public sst::SST<DerechoSST> {
public:
    // MulticastGroup members, related only to tracking message delivery
    /**
     * Sequence numbers are interpreted like a row-major pair:
     * (sender, index) becomes sender + num_members * index.
     * Since the global order is round-robin, the correct global order of
     * messages becomes a consecutive sequence of these numbers: with 4
     * senders, we expect to receive (0,0), (1,0), (2,0), (3,0), (0,1),
     * (1,1), ... which is 0, 1, 2, 3, 4, 5, ....
     *
     * This variable is the highest sequence number that has been received
     * in-order by this node; if a node updates seq_num, it has received all
     * messages up to seq_num in the global round-robin order. */
    SSTFieldVector<message_id_t> seq_num;
    /** This represents the highest sequence number that has been received
     * by every node, as observed by this node. If a node updates stable_num,
     * then it believes that all messages up to stable_num in the global
     * round-robin order have been received by every node. */
    SSTFieldVector<message_id_t> stable_num;
    /** This represents the highest sequence number that has been delivered
     * at this node. Messages are only delievered once stable, so it must be
     * at least stable_num. */
    SSTFieldVector<message_id_t> delivered_num;
    /** This represents the highest persistent version number that has been
     * persisted to disk at this node, if persistence is enabled. This is
     * updated by the PersistenceManager. */
    SSTFieldVector<ns_persistent::version_t> persisted_num;

    // Group management service members, related only to handling view changes
    /** View ID associated with this SST. VIDs monotonically increase as views change. */
    SSTField<int32_t> vid;
    /** Array of same length as View::members, where each bool represents
     * whether the corresponding member is suspected to have failed */
    SSTFieldVector<bool> suspected;
    /** An array of the same length as View::members, containing a list of
     * proposed changes to the view that have not yet been installed. The number
     * of valid elements is num_changes - num_installed, which should never exceed
     * View::num_members/2.
     * If request i is a Join, changes[i] is not in current View's members.
     * If request i is a Departure, changes[i] is in current View's members. */
    SSTFieldVector<node_id_t> changes;
    /** If changes[i] is a Join, joiner_ips[i] is the IP address of the joining
     *  node, packed into an unsigned int in network byte order. This
     *  representation is necessary because SST doesn't support variable-length
     *  strings. */
    SSTFieldVector<uint32_t> joiner_ips;
    /** How many changes to the view have been proposed. Monotonically increases.
     * num_changes - num_committed is the number of pending changes, which should never
     * exceed the number of members in the current view. If num_changes == num_committed
     * == num_installed, no changes are pending. */
    SSTField<int> num_changes;
    /** How many proposed view changes have reached the commit point. */
    SSTField<int> num_committed;
    /** How many proposed changes have been seen. Incremented by a member
     * to acknowledge that it has seen a proposed change.*/
    SSTField<int> num_acked;
    /** How many previously proposed view changes have been installed in the
     * current view. Monotonically increases, lower bound on num_committed. */
    SSTField<int> num_installed;
    /** Local count of number of received messages by sender.  For each
     * sender k, nReceived[k] is the number received (a.k.a. "locally stable").
     */
    SSTFieldVector<int32_t> num_received;
    /** Set after calling rdmc::wedged(), reports that this member is wedged.
     * Must be after num_received!*/
    SSTField<bool> wedged;
    /** Array of how many messages to accept from each sender in the current view change */
    SSTFieldVector<int> global_min;
    /** Array indicating whether each shard leader (indexed by subgroup number)
     * has published a global_min for the current view change*/
    SSTFieldVector<bool> global_min_ready;
    /** for SST multicast */
    SSTFieldVector<sst::Message> slots;
    SSTFieldVector<int32_t> num_received_sst;

    /** to check for failures - used by the thread running check_failures_loop in derecho_group **/
    SSTFieldVector<uint64_t> local_stability_frontier;
    /**
     * Constructs an SST, and initializes the GMS fields to "safe" initial values
     * (0, false, etc.). Initializing the MulticastGroup fields is left to MulticastGroup.
     * @param parameters The SST parameters, which will be forwarded to the
     * standard SST constructor.
     */
    DerechoSST(const sst::SSTParams& parameters, const uint32_t num_subgroups, const uint32_t num_received_size, uint32_t window_size)
            : sst::SST<DerechoSST>(this, parameters),
              seq_num(num_subgroups),
              stable_num(num_subgroups),
              delivered_num(num_subgroups),
              persisted_num(num_subgroups),
              suspected(parameters.members.size()),
              changes(100 + parameters.members.size()),
              joiner_ips(100 + parameters.members.size()),
              num_received(num_received_size),
              global_min(num_received_size),
              global_min_ready(num_subgroups),
              slots(window_size * num_subgroups),
              num_received_sst(num_received_size),
              local_stability_frontier(num_subgroups) {
        SSTInit(seq_num, stable_num, delivered_num,
                persisted_num, vid, suspected, changes, joiner_ips,
                num_changes, num_committed, num_acked, num_installed,
                num_received, wedged, global_min, global_min_ready,
                slots, num_received_sst, local_stability_frontier);
        //Once superclass constructor has finished, table entries can be initialized
        for(unsigned int row = 0; row < get_num_rows(); ++row) {
            vid[row] = 0;
            for(size_t i = 0; i < suspected.size(); ++i) {
                suspected[row][i] = false;
            }
            for(size_t i = 0; i < changes.size(); ++i) {
                changes[row][i] = false;
            }
            for(size_t i = 0; i < global_min_ready.size(); ++i) {
                global_min_ready[row][i] = false;
            }
            for(size_t i = 0; i < global_min.size(); ++i) {
                global_min[row][i] = 0;
            }
            memset(const_cast<uint32_t*>(joiner_ips[row]), 0, joiner_ips.size());
            num_changes[row] = 0;
            num_committed[row] = 0;
            num_installed[row] = 0;
            num_acked[row] = 0;
            wedged[row] = false;
            // start off local_stability_frontier with the current time
            struct timespec start_time;
            clock_gettime(CLOCK_REALTIME, &start_time);
            auto current_time = start_time.tv_sec * 1e9 + start_time.tv_nsec;
            for(size_t i = 0; i < local_stability_frontier.size(); ++i) {
                local_stability_frontier[row][i] = current_time;
            }
        }
    }

    /**
     * Initializes the local row of this SST based on the specified row of the
     * previous View's SST. Copies num_changes, num_committed, and num_acked,
     * adds num_changes_installed to the previous value of num_installed, copies
     * (num_changes - num_changes_installed) elements of changes, and initializes
     * the other SST fields to 0/false.
     * @param old_sst The SST instance to copy data from
     * @param row The target row in that SST instance (from which data will be copied)
     * @param num_changes_installed The number of changes that were applied
     * when changing from the previous view to this one
     */
    void init_local_row_from_previous(const DerechoSST& old_sst, const int row, const int num_changes_installed);

    /**
     * Copies currently proposed changes and the various counter values associated
     * with them to the local row from some other row (i.e. the group leader's row).
     * @param other_row The row to copy values from.
     */
    void init_local_change_proposals(const int other_row);

    /**
     * Creates a string representation of the local row (not the whole table).
     * This should be converted to an ostream operator<< to follow standards.
     */
    std::string to_string() const;
};

namespace gmssst {

/**
 * Thread-safe setter for DerechoSST members; ensures there is a
 * std::atomic_signal_fence after writing the value.
 * @param e A reference to a member of GMSTableRow.
 * @param value The value to set that reference to.
 */
template <typename Elem>
void set(volatile Elem& e, const Elem& value) {
    e = value;
    std::atomic_signal_fence(std::memory_order_acq_rel);
}

/**
 * Thread-safe setter for DerechoSST members; ensures there is a
 * std::atomic_signal_fence after writing the value.
 * @param e A reference to a member of GMSTableRow.
 * @param value The value to set that reference to.
 */
template <typename Elem>
void set(volatile Elem& e, volatile const Elem& value) {
    e = value;
    std::atomic_signal_fence(std::memory_order_acq_rel);
}

/**
 * Thread-safe setter for DerechoSST members that are arrays; takes a lock
 * before running memcpy, and then ensures there is an atomic_signal_fence.
 * The first {@code length} members of {@code value} are copied to {@code array}.
 * @param array A pointer to the first element of an array that should be set
 * to {@code value}, obtained by calling SSTFieldVector::operator[]
 * @param value A pointer to the first element of an array to read values from
 * @param length The number of array elements to copy
 */
template <typename Elem>
void set(volatile Elem* array, volatile Elem* value, const size_t length) {
    static thread_local std::mutex set_mutex;
    {
        std::lock_guard<std::mutex> lock(set_mutex);
        memcpy(const_cast<Elem*>(array), const_cast<Elem*>(value),
               length * sizeof(Elem));
    }
    std::atomic_signal_fence(std::memory_order_acq_rel);
}
/**
 * Thread-safe setter for DerechoSST members that are arrays; takes a lock
 * before running memcpy, and then ensures there is an atomic_signal_fence.
 * This version copies the entire array, and assumes both arrays are the same
 * length.
 *
 * @param e A reference to an array-type member of GMSTableRow
 * @param value The array whose contents should be copied to this member
 */
template <typename Arr, size_t Len>
void set(volatile Arr (&e)[Len], const volatile Arr (&value)[Len]) {
    static thread_local std::mutex set_mutex;
    {
        std::lock_guard<std::mutex> lock(set_mutex);
        memcpy(const_cast<Arr(&)[Len]>(e), const_cast<const Arr(&)[Len]>(value),
               Len * sizeof(Arr));
        // copy_n just plain doesn't work, claiming that its argument types are
        // "not assignable"
        //        std::copy_n(const_cast<const Arr (&)[Len]>(value), Len,
        //        const_cast<Arr (&)[Len]>(e));
    }
    std::atomic_signal_fence(std::memory_order_acq_rel);
}

/**
 * Thread-safe setter for DerechoSST members that are arrays; takes a lock
 * before running memcpy, and then ensures there is an atomic_signal_fence.
 * This version only copies the first num elements of the source array.
 * @param dst
 * @param src
 * @param num
 */
template <size_t L1, size_t L2, typename Arr>
void set(volatile Arr (&dst)[L1], const volatile Arr (&src)[L2], const size_t& num) {
    static thread_local std::mutex set_mutex;
    {
        std::lock_guard<std::mutex> lock(set_mutex);
        memcpy(const_cast<Arr(&)[L2]>(dst), const_cast<const Arr(&)[L1]>(src),
               num * sizeof(Arr));
    }
    std::atomic_signal_fence(std::memory_order_acq_rel);
}

void set(volatile char* string_array, const std::string& value);

void increment(volatile int& member);

bool equals(const volatile char& string_array, const std::string& value);

}  // namespace gmssst

}  // namespace derecho
