#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "derecho/derecho.h"
#include <mutils-serialization/SerializationSupport.hpp>
#include <persistent/Persistent.hpp>

using derecho::ExternalCaller;
using derecho::Replicated;
using std::cout;
using std::cerr;
using std::endl;

/**
 * Example for replicated object with Persistent<T>
 */
class PFoo : public mutils::ByteRepresentable, public derecho::PersistsFields {
    Persistent<int> pint;
#define INVALID_VALUE	(-1)

public:
    virtual ~PFoo() noexcept(true) {
    }
    int read_state(int64_t ver) {
        return *pint[ver];
    }
    int read_state_by_time(uint64_t epoch_us) {
        int val = INVALID_VALUE;
        try {
            val = *pint[HLC(epoch_us,(uint64_t)0LLU)];
        } catch (...) {
            cout << "read_state_by_time(): invalid ts=" << epoch_us << endl;
        }
        return val;
    }
    bool change_state(int new_int) {
        if(new_int == *pint) {
            return false;
        }
        *pint = new_int;
        return true;
    }
    int64_t get_latest_version() {
        return pint.getLatestVersion();
    }

    enum Functions { READ_STATE,
                     READ_STATE_BY_TIME,
                     CHANGE_STATE,
                     GET_LATEST_VERSION, };

    static auto register_functions() {
        return std::make_tuple(derecho::rpc::tag<READ_STATE>(&PFoo::read_state),
                               derecho::rpc::tag<READ_STATE_BY_TIME>(&PFoo::read_state_by_time),
                               derecho::rpc::tag<CHANGE_STATE>(&PFoo::change_state),
                               derecho::rpc::tag<GET_LATEST_VERSION>(&PFoo::get_latest_version));
    }

    // constructor for PersistentRegistry
    PFoo(PersistentRegistry* pr) : pint(nullptr, pr) {}
    PFoo(Persistent<int>& init_pint) : pint(std::move(init_pint)) {}
    DEFAULT_SERIALIZATION_SUPPORT(PFoo, pint);
};


int main(int argc, char** argv) {
    derecho::Conf::initialize(argc, argv);

    derecho::CallbackSet callback_set{
            nullptr,
            [](derecho::subgroup_id_t subgroup, persistent::version_t ver) {
                std::cout << "Subgroup " << subgroup << ", version " << ver << "is persisted." << std::endl;
            }};

    derecho::SubgroupInfo subgroup_info{{{std::type_index(typeid(PFoo)), [](const derecho::View& curr_view, int& next_unassigned_rank) {
                                              if(curr_view.num_members < 2) {
                                                  std::cout << "PFoo function throwing subgroup_provisioning_exception" << std::endl;
                                                  throw derecho::subgroup_provisioning_exception();
                                              }
                                              derecho::subgroup_shard_layout_t subgroup_vector(1);
                                              //Put the desired SubView at subgroup_vector[0][0] since there's one subgroup with one shard
                                              subgroup_vector[0].emplace_back(curr_view.make_subview({0, 1}));
                                              next_unassigned_rank = std::max(next_unassigned_rank, 2);
                                              return subgroup_vector;
                                          }}},
                                        {std::type_index(typeid(PFoo))}};

    auto pfoo_factory = [](PersistentRegistry* pr) { return std::make_unique<PFoo>(pr); };

    derecho::Group<PFoo> group(callback_set, subgroup_info,
                               std::vector<derecho::view_upcall_t>{},
                               pfoo_factory);

    cout << "Finished constructing/joining Group" << endl;

    const uint32_t node_id = derecho::getConfUInt32(CONF_DERECHO_LOCAL_ID);

    // Update the states:
    Replicated<PFoo> & pfoo_rpc_handle = group.get_subgroup<PFoo>();
    int values[] = {(int)(1000 + node_id), (int)(2000 + node_id), (int)(3000 + node_id) };
    for (int i=0;i<3;i++) {
        derecho::rpc::QueryResults<bool> resultx = pfoo_rpc_handle.ordered_send<PFoo::CHANGE_STATE>(values[i]);
        decltype(resultx)::ReplyMap& repliex = resultx.get();
        cout << "Change state to " << values[i] << endl;
        for (auto& reply_pair : repliex) {
            cout << "\tnode[" << reply_pair.first << "] replies with '" << std::boolalpha << reply_pair.second.get() << "'." << endl;
        }
    }

    if(node_id == 0) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // Query for latest version.
        int64_t lv = 0;
        derecho::rpc::QueryResults<int64_t> resultx = pfoo_rpc_handle.ordered_send<PFoo::GET_LATEST_VERSION>();
        decltype(resultx)::ReplyMap& repliex = resultx.get();
        cout << "Query the latest versions:" << endl;
        for (auto& reply_pair:repliex) {
            lv = reply_pair.second.get();
            cout << "\tnode[" << reply_pair.first << "] replies with version " << lv << "." << endl;
        }

        // Query all versions.
        for(int64_t ver = 0; ver <= lv ; ver ++) {
            derecho::rpc::QueryResults<int> resultx = pfoo_rpc_handle.ordered_send<PFoo::READ_STATE>(ver);
            cout << "Query the value of version:" << ver << endl;
            for (auto& reply_pair:resultx.get()) {
                cout <<"\tnode[" << reply_pair.first << "]: v["<<ver<<"]="<<reply_pair.second.get()<<endl;
            }
        }

        // Query state by time.
        struct timespec tv;
        if(clock_gettime(CLOCK_REALTIME,&tv)) {
            cerr << "failed to read current time" << endl;
        } else {
            uint64_t now = tv.tv_sec*1000000+tv.tv_nsec/1000;
            uint64_t too_early = now - 5000000; // 5 second before
            // wait for the temporal frontier...
            std::this_thread::sleep_for(std::chrono::seconds(1));
            derecho::rpc::QueryResults<int> resultx = pfoo_rpc_handle.ordered_send<PFoo::READ_STATE_BY_TIME>(now);
            cout << "Query for now: ts="<< now << "us" << endl;
            for (auto& reply_pair:resultx.get()) {
                cout << "\tnode[" << reply_pair.first << "] replies with value:" << reply_pair.second.get() << endl;
            }
           derecho::rpc::QueryResults<int> resulty = pfoo_rpc_handle.ordered_send<PFoo::READ_STATE_BY_TIME>(too_early);
            cout << "Query for 5 sec before: ts="<< too_early << "us" <<endl;
            for (auto& reply_pair:resulty.get()) {
                cout << "\tnode[" << reply_pair.first << "] replies with value:" << reply_pair.second.get() << endl;
            }
        }
    }

    cout << "Reached end of main(), entering infinite loop so program doesn't exit" << std::endl;
    while(true) {
    }
}
