#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>

#include "view.h"

namespace derecho {

using std::shared_ptr;
using std::string;

SubView::SubView(int32_t num_members)
        : mode(Mode::ORDERED),
          members(num_members),
          is_sender(num_members, 1),
          member_ips_and_ports(num_members),
          joined(0),
          departed(0),
          my_rank(-1) {}

SubView::SubView(Mode mode,
                 const std::vector<node_id_t>& members,
                 std::vector<int> is_sender,
                 const std::vector<std::tuple<ip_addr_t, uint16_t, uint16_t, uint16_t,
                                              uint16_t>>& member_ips_and_ports)
        : mode(mode),
          members(members),
          is_sender(members.size(), 1),
          member_ips_and_ports(member_ips_and_ports),
          my_rank(-1) {
    // if the sender information is not provided, assume that all members are
    // senders
    if(is_sender.size()) {
        this->is_sender = is_sender;
    }
}

int SubView::rank_of(const node_id_t& who) const {
    for(std::size_t rank = 0; rank < members.size(); ++rank) {
        if(members[rank] == who) {
            return rank;
        }
    }
    return -1;
}

int SubView::sender_rank_of(uint32_t rank) const {
    if(!is_sender[rank]) {
        return -1;
    }
    int num = 0;
    for(uint i = 0; i < rank; ++i) {
        if(is_sender[i]) {
            num++;
        }
    }
    return num;
}

uint32_t SubView::num_senders() const {
    uint32_t num = 0;
    for(const auto i : is_sender) {
        if(i) {
            num++;
        }
    }
    return num;
}

View::View(const int32_t vid, const std::vector<node_id_t>& members,
           const std::vector<std::tuple<ip_addr_t, uint16_t, uint16_t, uint16_t, uint16_t>>& member_ips_and_ports,
           const std::vector<char>& failed, const int32_t num_failed,
           const std::vector<node_id_t>& joined,
           const std::vector<node_id_t>& departed,
           const int32_t num_members,
           const std::map<subgroup_type_id_t, std::vector<subgroup_id_t>>& subgroup_ids_by_type_id,
           const std::vector<std::vector<SubView>>& subgroup_shard_views,
           const std::map<subgroup_id_t, uint32_t>& my_subgroups)
        : vid(vid),
          members(members),
          member_ips_and_ports(member_ips_and_ports),
          failed(failed),
          num_failed(num_failed),
          joined(joined),
          departed(departed),
          num_members(num_members),
          my_rank(0),              // This will always get overwritten by the receiver after deserializing
          next_unassigned_rank(0), /* next_unassigned_rank should never be serialized, since each
                                    * node must re-run the allocation functions independently */
          subgroup_ids_by_type_id(subgroup_ids_by_type_id),
          subgroup_shard_views(subgroup_shard_views),
          my_subgroups(my_subgroups) {
    for(int rank = 0; rank < num_members; ++rank) {
        node_id_to_rank[members[rank]] = rank;
    }
}

int View::rank_of_leader() const {
    for(int r = 0; r < num_members; ++r) {
        if(!failed[r]) {
            return r;
        }
    }
    return -1;
}

View::View(const int32_t vid, const std::vector<node_id_t>& members,
           const std::vector<std::tuple<ip_addr_t, uint16_t, uint16_t, uint16_t, uint16_t>>& member_ips_and_ports,
           const std::vector<char>& failed, const std::vector<node_id_t>& joined,
           const std::vector<node_id_t>& departed,
           const int32_t my_rank,
           const int32_t next_unassigned_rank,
           const std::vector<std::type_index>& subgroup_type_order)
        : vid(vid),
          members(members),
          member_ips_and_ports(member_ips_and_ports),
          failed(failed),
          num_failed(0),
          joined(joined),
          departed(departed),
          num_members(members.size()),
          my_rank(my_rank),
          next_unassigned_rank(next_unassigned_rank),
          subgroup_type_order(subgroup_type_order) {
    for(int rank = 0; rank < num_members; ++rank) {
        node_id_to_rank[members[rank]] = rank;
    }
    for(auto c : failed) {
        if(c) {
            num_failed++;
        }
    }
}

int View::rank_of(const std::tuple<ip_addr_t, uint16_t, uint16_t, uint16_t, uint16_t>& who) const {
    for(int rank = 0; rank < num_members; ++rank) {
        if(member_ips_and_ports[rank] == who) {
            return rank;
        }
    }
    return -1;
}

int View::rank_of(const node_id_t& who) const {
    auto it = node_id_to_rank.find(who);
    if(it != node_id_to_rank.end()) {
        return it->second;
    }
    return -1;
}

SubView View::make_subview(const std::vector<node_id_t>& with_members,
                           const Mode mode,
                           const std::vector<int>& is_sender) const {
    std::vector<std::tuple<ip_addr_t, uint16_t, uint16_t, uint16_t, uint16_t>> subview_member_ips_and_ports(with_members.size());
    for(std::size_t subview_rank = 0; subview_rank < with_members.size();
        ++subview_rank) {
        std::size_t member_pos = std::distance(members.begin(), std::find(members.begin(), members.end(),
                                                                          with_members[subview_rank]));
        if(member_pos == members.size()) {
            // The ID wasn't found in members[]
            throw subgroup_provisioning_exception();
        }
        subview_member_ips_and_ports[subview_rank] = member_ips_and_ports[member_pos];
    }
    // Note that joined and departed do not need to get initialized here; they wiill be initialized by ViewManager
    return SubView(mode, with_members, is_sender, subview_member_ips_and_ports);
}

int View::subview_rank_of_shard_leader(subgroup_id_t subgroup_id,
                                       uint32_t shard_index) const {
    if(shard_index >= subgroup_shard_views.at(subgroup_id).size()) {
        return -1;
    }
    const SubView& shard_view = subgroup_shard_views.at(subgroup_id).at(shard_index);
    for(std::size_t rank = 0; rank < shard_view.members.size(); ++rank) {
        // Inefficient to call rank_of every time, but no guarantee the subgroup
        // members will have ascending ranks
        if(!failed[rank_of(shard_view.members[rank])]) {
            return rank;
        }
    }
    return -1;
}

bool View::i_am_leader() const {
    return (rank_of_leader() == my_rank);  // True if I know myself to be the leader
}

bool View::i_am_new_leader() {
    if(i_know_i_am_leader) {
        return false;  // I am the OLD leader
    }

    for(int n = 0; n < my_rank; n++) {
        for(int row = 0; row < my_rank; row++) {
            if(!failed[n] && !gmsSST->suspected[row][n]) {
                return false;  // I'm not the new leader, or some failure suspicion hasn't fully propagated
            }
        }
    }
    i_know_i_am_leader = true;
    return true;
}

void View::merge_changes() {
    int myRank = my_rank;
    // Merge the change lists
    for(int n = 0; n < num_members; n++) {
        if(gmsSST->num_changes[myRank] < gmsSST->num_changes[n]) {
            gmssst::set(gmsSST->changes[myRank], gmsSST->changes[n],
                        gmsSST->changes.size());
            gmssst::set(gmsSST->num_changes[myRank], gmsSST->num_changes[n]);
        }

        if(gmsSST->num_committed[myRank] < gmsSST->num_committed[n])  // How many I know to have been committed
        {
            gmssst::set(gmsSST->num_committed[myRank], gmsSST->num_committed[n]);
        }
    }
    bool found = false;
    for(int n = 0; n < num_members; n++) {
        if(failed[n]) {
            // Make sure that the failed process is listed in the changes vector as a
            // proposed change
            for(int c = gmsSST->num_committed[myRank];
                c < gmsSST->num_changes[myRank] && !found; c++) {
                if(gmsSST->changes[myRank][c % gmsSST->changes.size()] == members[n]) {
                    // Already listed
                    found = true;
                }
            }
        } else {
            // Not failed
            found = true;
        }

        if(!found) {
            gmssst::set(gmsSST->changes[myRank][gmsSST->num_changes[myRank] % gmsSST->changes.size()],
                        members[n]);
            gmssst::increment(gmsSST->num_changes[myRank]);
        }
    }
    // gmsSST->put(gmsSST->changes.get_base() - gmsSST->getBaseAddress(),
    //             gmsSST->num_acked.get_base() - gmsSST->changes.get_base());
    /* breaking the above put statement into individual put calls, to be sure that
     * if we were relying on any ordering guarantees, we won't run into issue when
     * guarantees do not hold*/
    gmsSST->put(gmsSST->changes.get_base() - gmsSST->getBaseAddress(),
                gmsSST->joiner_ips.get_base() - gmsSST->changes.get_base());
    gmsSST->put(gmsSST->joiner_ips.get_base() - gmsSST->getBaseAddress(),
                gmsSST->num_changes.get_base() - gmsSST->joiner_ips.get_base());
    gmsSST->put(gmsSST->num_changes.get_base() - gmsSST->getBaseAddress(),
                gmsSST->num_committed.get_base() - gmsSST->num_changes.get_base());
    gmsSST->put(gmsSST->num_committed.get_base() - gmsSST->getBaseAddress(),
                gmsSST->num_acked.get_base() - gmsSST->num_committed.get_base());
}

void View::wedge() {
    multicast_group->wedge();  // RDMC finishes sending, stops new sends or receives in Vc
    gmssst::set(gmsSST->wedged[my_rank], true);
    gmsSST->put(gmsSST->wedged.get_base() - gmsSST->getBaseAddress(),
                sizeof(gmsSST->wedged[0]));
}

std::string View::debug_string() const {
    // need to add member ips and ports and other fields
    std::stringstream s;
    s << "View " << vid << ": MyRank=" << my_rank << ". ";
    s << "Members={ ";
    for(int m = 0; m < num_members; m++) {
        s << members[m] << "  ";
    }
    s << "}, ";
    string fs = (" ");
    for(int m = 0; m < num_members; m++) {
        fs += failed[m] ? string(" T ") : string(" F ");
    }

    s << "Failed={" << fs << " }, num_failed=" << num_failed;
    s << ", Departed: { ";
    for(const node_id_t& departed_node : departed) {
        s << departed_node << " ";
    }
    s << "} , Joined: { ";
    for(const node_id_t& joined_node : joined) {
        s << joined_node << " ";
    }
    s << "}" << std::endl;
    s << "SubViews: ";
    for(subgroup_id_t subgroup = 0; subgroup < subgroup_shard_views.size(); ++subgroup) {
        for(uint32_t shard = 0; shard < subgroup_shard_views[subgroup].size(); ++shard) {
            s << "Shard (" << subgroup << ", " << shard << "): Members={";
            for(const node_id_t& member : subgroup_shard_views[subgroup][shard].members) {
                s << member << " ";
            }
            s << "}, is_sender={";
            for(uint i = 0; i < subgroup_shard_views[subgroup][shard].members.size(); ++i) {
                if(subgroup_shard_views[subgroup][shard].is_sender[i]) {
                    s << "T ";
                } else {
                    s << "F ";
                }
            }
            s << "}.  ";
        }
    }
    return s.str();
}

}  // namespace derecho
