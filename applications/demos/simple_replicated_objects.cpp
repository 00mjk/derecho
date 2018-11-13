#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "derecho/derecho.h"
#include "conf/conf.hpp"
#include "sample_objects.h"

using derecho::ExternalCaller;
using derecho::Replicated;
using std::cout;
using std::endl;

int main(int argc, char** argv) {
    // Read configurations from the command line options as well as the default config file
    derecho::Conf::initialize(argc, argv);

    //Define subgroup membership for each Replicated type
    //Each Replicated type will have one subgroup and one shard, with three members in the shard
    //The Foo and Bar subgroups will both reside on the first 3 nodes in the view,
    //while the Cache subgroup will reside on the second 3 nodes in the view.
    derecho::SubgroupInfo subgroup_info{
            {{std::type_index(typeid(Foo)), [](const derecho::View& curr_view, int& next_unassigned_rank) {
                  if(curr_view.num_members < 3) {
                      throw derecho::subgroup_provisioning_exception();
                  }
                  derecho::subgroup_shard_layout_t subgroup_vector(1);
                  std::vector<node_id_t> first_3_nodes(&curr_view.members[0], &curr_view.members[0] + 3);
                  //Put the desired SubView at subgroup_vector[0][0] since there's one subgroup with one shard
                  subgroup_vector[0].emplace_back(curr_view.make_subview(first_3_nodes));
                  next_unassigned_rank = std::max(next_unassigned_rank, 3);
                  return subgroup_vector;
              }},
             {std::type_index(typeid(Bar)), [](const derecho::View& curr_view, int& next_unassigned_rank) {
                  if(curr_view.num_members < 3) {
                      throw derecho::subgroup_provisioning_exception();
                  }
                  derecho::subgroup_shard_layout_t subgroup_vector(1);
                  std::vector<node_id_t> first_3_nodes(&curr_view.members[0], &curr_view.members[0] + 3);
                  subgroup_vector[0].emplace_back(curr_view.make_subview(first_3_nodes));
                  next_unassigned_rank = std::max(next_unassigned_rank, 3);
                  return subgroup_vector;
              }},
             {std::type_index(typeid(Cache)), [](const derecho::View& curr_view, int& next_unassigned_rank) {
                  if(curr_view.num_members < 6) {
                      throw derecho::subgroup_provisioning_exception();
                  }
                  derecho::subgroup_shard_layout_t subgroup_vector(1);
                  std::vector<node_id_t> next_3_nodes(&curr_view.members[3], &curr_view.members[3] + 3);
                  subgroup_vector[0].emplace_back(curr_view.make_subview(next_3_nodes));
                  next_unassigned_rank += 3;
                  return subgroup_vector;
              }}},
            {std::type_index(typeid(Foo)), std::type_index(typeid(Bar)), std::type_index(typeid(Cache))}};

    //Each replicated type needs a factory; this can be used to supply constructor arguments
    //for the subgroup's initial state. These must take a PersistentRegistry* argument, but
    //in this case we ignore it because the replicated objects aren't persistent.
    auto foo_factory = [](PersistentRegistry*) { return std::make_unique<Foo>(-1); };
    auto bar_factory = [](PersistentRegistry*) { return std::make_unique<Bar>(); };
    auto cache_factory = [](PersistentRegistry*) { return std::make_unique<Cache>(); };

    derecho::Group<Foo, Bar, Cache> group(derecho::CallbackSet{}, subgroup_info,
					  std::vector<derecho::view_upcall_t>{},
                                          foo_factory, bar_factory, cache_factory);

    cout << "Finished constructing/joining Group" << endl;

    //Now have each node send some updates to the Replicated objects
    //The code must be different depending on which subgroup this node is in,
    //which we can determine based on its position in the members list
    std::vector<node_id_t> member_ids = group.get_members();
    // get my_node_id from Derecho's map storing the configuration
    uint32_t my_node_id = derecho::getConfUInt32(CONF_DERECHO_LOCAL_ID);
      if(my_node_id == member_ids[0]) {
        Replicated<Foo>& foo_rpc_handle = group.get_subgroup<Foo>();
        Replicated<Bar>& bar_rpc_handle = group.get_subgroup<Bar>();
        cout << "Appending to Bar" << endl;
        bar_rpc_handle.ordered_send<RPC_NAME(append)>("Write from 0...");
        cout << "Reading Foo's state just to allow node 1's message to be delivered" << endl;
        foo_rpc_handle.ordered_query<RPC_NAME(read_state)>();
    }
    if(my_node_id == member_ids[1]) {
        Replicated<Foo>& foo_rpc_handle = group.get_subgroup<Foo>();
        Replicated<Bar>& bar_rpc_handle = group.get_subgroup<Bar>();
        int new_value = 3;
        cout << "Changing Foo's state to " << new_value << endl;
        derecho::rpc::QueryResults<bool> results = foo_rpc_handle.ordered_query<RPC_NAME(change_state)>(new_value);
        decltype(results)::ReplyMap& replies = results.get();
        cout << "Got a reply map!" << endl;
        for(auto& reply_pair : replies) {
            cout << "Reply from node " << reply_pair.first << " was " << std::boolalpha << reply_pair.second.get() << endl;
        }
        cout << "Appending to Bar" << endl;
        bar_rpc_handle.ordered_send<RPC_NAME(append)>("Write from 1...");
    }
    if(my_node_id == member_ids[2]) {
        Replicated<Foo>& foo_rpc_handle = group.get_subgroup<Foo>();
        Replicated<Bar>& bar_rpc_handle = group.get_subgroup<Bar>();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        cout << "Reading Foo's state from the group" << endl;
        derecho::rpc::QueryResults<int> foo_results = foo_rpc_handle.ordered_query<RPC_NAME(read_state)>();
        for(auto& reply_pair : foo_results.get()) {
            cout << "Node " << reply_pair.first << " says the state is: " << reply_pair.second.get() << endl;
        }
        bar_rpc_handle.ordered_send<RPC_NAME(append)>("Write from 2...");
        cout << "Printing log from Bar" << endl;
        derecho::rpc::QueryResults<std::string> bar_results = bar_rpc_handle.ordered_query<RPC_NAME(print)>();
        for(auto& reply_pair : bar_results.get()) {
            cout << "Node " << reply_pair.first << " says the log is: " << reply_pair.second.get() << endl;
        }
        cout << "Clearing Bar's log" << endl;
        bar_rpc_handle.ordered_send<RPC_NAME(clear)>();
    }

    if(my_node_id == member_ids[3]) {
        Replicated<Cache>& cache_rpc_handle = group.get_subgroup<Cache>();
        cout << "Waiting for a 'Ken' value to appear in the cache..." << endl;
        bool found = false;
        while(!found) {
            derecho::rpc::QueryResults<bool> results = cache_rpc_handle.ordered_query<RPC_NAME(contains)>("Ken");
            derecho::rpc::QueryResults<bool>::ReplyMap& replies = results.get();
            //Fold "&&" over the results to see if they're all true
            bool contains_accum = true;
            for(auto& reply_pair : replies) {
                bool contains_result = reply_pair.second.get();
                cout << std::boolalpha << "  Reply from node " << reply_pair.first << ": " << contains_result << endl;
                contains_accum = contains_accum && contains_result;
            }
            found = contains_accum;
        }
        cout << "..found!" << endl;
        derecho::rpc::QueryResults<std::string> results = cache_rpc_handle.ordered_query<RPC_NAME(get)>("Ken");
        for(auto& reply_pair : results.get()) {
            cout << "Node " << reply_pair.first << " had Ken = " << reply_pair.second.get() << endl;
        }
    }
    if(my_node_id == member_ids[4]) {
        Replicated<Cache>& cache_rpc_handle = group.get_subgroup<Cache>();
        cout << "Putting Ken = Birman in the cache" << endl;
        //Do it twice just to send more messages, so that the "contains" and "get" calls can go through
        cache_rpc_handle.ordered_send<RPC_NAME(put)>("Ken", "Birman");
        cache_rpc_handle.ordered_send<RPC_NAME(put)>("Ken", "Birman");
        node_id_t p2p_target = 2;
        cout << "Reading Foo's state from node " << p2p_target << endl;
        ExternalCaller<Foo>& p2p_foo_handle = group.get_nonmember_subgroup<Foo>();
        derecho::rpc::QueryResults<int> foo_results = p2p_foo_handle.p2p_query<RPC_NAME(read_state)>(p2p_target);
        int response = foo_results.get().get(p2p_target);
        cout << "  Response: " << response << endl;
    }
    if(my_node_id == member_ids[5]) {
        Replicated<Cache>& cache_rpc_handle = group.get_subgroup<Cache>();
        cout << "Putting Ken = Woodberry in the cache" << endl;
        cache_rpc_handle.ordered_send<RPC_NAME(put)>("Ken", "Woodberry");
        cache_rpc_handle.ordered_send<RPC_NAME(put)>("Ken", "Woodberry");
    }

    cout << "Reached end of main(), entering infinite loop so program doesn't exit" << std::endl;
    while(true) {
    }
}
