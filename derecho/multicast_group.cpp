#include <algorithm>
#include <cassert>
#include <chrono>
#include <limits>
#include <thread>

#include "multicast_group.h"
#include "rdmc/util.h"

namespace derecho {

/**
 * Helper function to find the index of an element in a container.
 */
template <class T, class U>
size_t index_of(T container, U elem) {
    size_t n = 0;
    for(auto it = begin(container); it != end(container); ++it) {
        if(*it == elem) return n;

        n++;
    }
    return container.size();
}

/**
 *
 * @param _members A list of node IDs of members in this group
 * @param my_node_id The rank (ID) of this node in the group
 * @param sst The SST this group will use; created by the GMS (membership
 * service) for this group.
 * @param _free_message_buffers RDMCMessage buffers to use for RDMC sending/receiving
 * (there must be one for each sender in the group)
 * @param _max_payload_size The size of the largest possible message that will
 * be sent in this group, in bytes
 * @param _callbacks A set of functions to call when messages have reached
 * various levels of stability
 * @param _block_size The block size to use for RDMC
 * @param _window_size The window size (number of outstanding messages that can
 * be in progress at once before blocking sends) to use when sending a stream
 * of messages to the group; default is 3
 * @param timeout_ms The time that this node will wait for a sender in the group
 * to send its message before concluding that the sender has failed; default is 1ms
 * @param _type The type of RDMC algorithm to use; default is BINOMIAL_SEND
 * @param filename If provided, the name of the file in which to save persistent
 * copies of all messages received. If an empty filename is given (the default),
 * the node runs in non-persistent mode and no persistence callbacks will be
 * issued.
 */

MulticastGroup::MulticastGroup(
        std::vector<node_id_t> _members, node_id_t my_node_id,
        std::shared_ptr<DerechoSST> sst,
        CallbackSet callbacks,
        uint32_t total_num_subgroups,
        const std::map<subgroup_id_t, std::pair<uint32_t, uint32_t>>& subgroup_to_shard_and_rank,
        const std::map<subgroup_id_t, std::pair<std::vector<int>, int>>& subgroup_to_senders_and_sender_rank,
        const std::map<subgroup_id_t, uint32_t>& subgroup_to_num_received_offset,
        const std::map<subgroup_id_t, std::vector<node_id_t>>& subgroup_to_membership,
        const std::map<subgroup_id_t, Mode>& subgroup_to_mode,
        const DerechoParams derecho_params,
        std::vector<char> already_failed)
        : logger(spdlog::get("debug_log")),
          members(_members),
          num_members(members.size()),
          member_index(index_of(members, my_node_id)),
          block_size(derecho_params.block_size),
          max_msg_size(compute_max_msg_size(derecho_params.max_payload_size, derecho_params.block_size)),
          type(derecho_params.type),
          window_size(derecho_params.window_size),
          callbacks(callbacks),
          total_num_subgroups(total_num_subgroups),
          subgroup_to_shard_and_rank(subgroup_to_shard_and_rank),
          subgroup_to_senders_and_sender_rank(subgroup_to_senders_and_sender_rank),
          subgroup_to_num_received_offset(subgroup_to_num_received_offset),
          received_intervals(sst->num_received.size(), {-1, -1}),
          subgroup_to_membership(subgroup_to_membership),
          subgroup_to_mode(subgroup_to_mode),
          rdmc_group_num_offset(0),
          future_message_indices(total_num_subgroups, 0),
          next_sends(total_num_subgroups),
          pending_sends(total_num_subgroups),
          current_sends(total_num_subgroups),
          next_message_to_deliver(total_num_subgroups),
          sender_timeout(derecho_params.timeout_ms),
          sst(sst),
          sst_multicast_group_ptrs(total_num_subgroups),
          last_transfer_medium(total_num_subgroups) {
    assert(window_size >= 1);

    if(!derecho_params.filename.empty()) {
        file_writer = std::make_unique<FileWriter>(make_file_written_callback(),
                                                   derecho_params.filename);
    }

    for(uint i = 0; i < num_members; ++i) {
        node_id_to_sst_index[members[i]] = i;
    }

    for(const auto p : subgroup_to_shard_and_rank) {
        auto num_shard_members = subgroup_to_membership.at(p.first).size();
        while(free_message_buffers[p.first].size() < window_size * num_shard_members) {
            free_message_buffers[p.first].emplace_back(max_msg_size);
        }
    }

    initialize_sst_row();
    bool no_member_failed = true;
    if(already_failed.size()) {
        for(uint i = 0; i < num_members; ++i) {
            if(already_failed[i]) {
                no_member_failed = false;
                break;
            }
        }
    }
    if(!already_failed.size() || no_member_failed) {
        // if groups are created successfully, rdmc_sst_groups_created will be set to true
        rdmc_sst_groups_created = create_rdmc_sst_groups();
    }
    register_predicates();
    sender_thread = std::thread(&MulticastGroup::send_loop, this);
    timeout_thread = std::thread(&MulticastGroup::check_failures_loop, this);
}

MulticastGroup::MulticastGroup(
        std::vector<node_id_t> _members, node_id_t my_node_id,
        std::shared_ptr<DerechoSST> sst,
        MulticastGroup&& old_group,
        uint32_t total_num_subgroups,
        const std::map<subgroup_id_t, std::pair<uint32_t, uint32_t>>& subgroup_to_shard_and_rank,
        const std::map<subgroup_id_t, std::pair<std::vector<int>, int>>& subgroup_to_senders_and_sender_rank,
        const std::map<subgroup_id_t, uint32_t>& subgroup_to_num_received_offset,
        const std::map<subgroup_id_t, std::vector<node_id_t>>& subgroup_to_membership,
        const std::map<subgroup_id_t, Mode>& subgroup_to_mode,
        std::vector<char> already_failed, uint32_t rpc_port)
        : logger(old_group.logger),
          members(_members),
          num_members(members.size()),
          member_index(index_of(members, my_node_id)),
          block_size(old_group.block_size),
          max_msg_size(old_group.max_msg_size),
          type(old_group.type),
          window_size(old_group.window_size),
          callbacks(old_group.callbacks),
          total_num_subgroups(total_num_subgroups),
          subgroup_to_shard_and_rank(subgroup_to_shard_and_rank),
          subgroup_to_senders_and_sender_rank(subgroup_to_senders_and_sender_rank),
          subgroup_to_num_received_offset(subgroup_to_num_received_offset),
          received_intervals(sst->num_received.size(), {-1, -1}),
          subgroup_to_membership(subgroup_to_membership),
          subgroup_to_mode(subgroup_to_mode),
          rpc_callback(old_group.rpc_callback),
          rdmc_group_num_offset(old_group.rdmc_group_num_offset + old_group.num_members),
          future_message_indices(total_num_subgroups, 0),
          next_sends(total_num_subgroups),
          pending_sends(total_num_subgroups),
          current_sends(total_num_subgroups),
          next_message_to_deliver(total_num_subgroups),
          sender_timeout(old_group.sender_timeout),
          sst(sst),
          sst_multicast_group_ptrs(total_num_subgroups),
          last_transfer_medium(total_num_subgroups) {
    // Make sure rdmc_group_num_offset didn't overflow.
    assert(old_group.rdmc_group_num_offset <= std::numeric_limits<uint16_t>::max() - old_group.num_members - num_members);

    // Just in case
    old_group.wedge();

    for(uint i = 0; i < num_members; ++i) {
        node_id_to_sst_index[members[i]] = i;
    }

    // Convience function that takes a msg from the old group and
    // produces one suitable for this group.
    auto convert_msg = [this](RDMCMessage& msg, subgroup_id_t subgroup_num) {
        msg.sender_id = members[member_index];
        msg.index = future_message_indices[subgroup_num]++;

        header* h = (header*)msg.message_buffer.buffer.get();
        future_message_indices[subgroup_num] += h->pause_sending_turns;

        return std::move(msg);
    };

    // Convience function that takes a msg from the old group and
    // produces one suitable for this group.
    auto convert_sst_msg = [this](SSTMessage& msg, subgroup_id_t subgroup_num) {
        msg.sender_id = members[member_index];
        msg.index = future_message_indices[subgroup_num]++;

        header* h = (header*)msg.buf;
        future_message_indices[subgroup_num] += h->pause_sending_turns;

        return std::move(msg);
    };

    for(const auto p : subgroup_to_shard_and_rank) {
        auto num_shard_members = subgroup_to_membership.at(p.first).size();
        while(free_message_buffers[p.first].size() < window_size * num_shard_members) {
            free_message_buffers[p.first].emplace_back(max_msg_size);
        }
    }

    // Reclaim RDMCMessageBuffers from the old group, and supplement them with
    // additional if the group has grown.
    std::lock_guard<std::mutex> lock(old_group.msg_state_mtx);
    for(const auto p : subgroup_to_shard_and_rank) {
        const auto subgroup_num = p.first;
        auto num_shard_members = subgroup_to_membership.at(p.first).size();
        // for later: don't move extra message buffers
        free_message_buffers[subgroup_num].swap(old_group.free_message_buffers[subgroup_num]);
        while(free_message_buffers[subgroup_num].size() < old_group.window_size * num_shard_members) {
            free_message_buffers[subgroup_num].emplace_back(max_msg_size);
        }
    }

    for(auto& msg : old_group.current_receives) {
        free_message_buffers[msg.first.first].push_back(std::move(msg.second.message_buffer));
    }
    old_group.current_receives.clear();

    // Assume that any locally stable messages failed. If we were the sender
    // than re-attempt, otherwise discard. TODO: Presumably the ragged edge
    // cleanup will want the chance to deliver some of these.
    for(auto& p : old_group.locally_stable_rdmc_messages) {
        if(p.second.size() == 0) {
            continue;
        }

        for(auto& q : p.second) {
            if(q.second.sender_id == members[member_index]) {
                pending_sends[p.first].push(convert_msg(q.second, p.first));
            } else {
                free_message_buffers[p.first].push_back(std::move(q.second.message_buffer));
            }
        }
    }
    old_group.locally_stable_rdmc_messages.clear();

    for(auto& p : old_group.locally_stable_sst_messages) {
        if(p.second.size() == 0) {
            continue;
        }
    }
    old_group.locally_stable_sst_messages.clear();

    // Any messages that were being sent should be re-attempted.
    for(auto p : subgroup_to_shard_and_rank) {
        auto subgroup_num = p.first;
        if(old_group.current_sends.size() > subgroup_num && old_group.current_sends[subgroup_num]) {
            pending_sends[subgroup_num].push(convert_msg(*old_group.current_sends[subgroup_num], subgroup_num));
        }

        if(old_group.pending_sends.size() > subgroup_num) {
            while(!old_group.pending_sends[subgroup_num].empty()) {
                pending_sends[subgroup_num].push(convert_msg(old_group.pending_sends[subgroup_num].front(), subgroup_num));
                old_group.pending_sends[subgroup_num].pop();
            }
        }

        if(old_group.next_sends.size() > subgroup_num && old_group.next_sends[subgroup_num]) {
            next_sends[subgroup_num] = convert_msg(*old_group.next_sends[subgroup_num], subgroup_num);
        }

        for(auto& entry : old_group.non_persistent_messages[subgroup_num]) {
            non_persistent_messages[subgroup_num].emplace(entry.first,
                                                          convert_msg(entry.second, subgroup_num));
        }
        old_group.non_persistent_messages.clear();
        for(auto& entry : old_group.non_persistent_sst_messages[subgroup_num]) {
            non_persistent_sst_messages[subgroup_num].emplace(entry.first,
                                                              convert_sst_msg(entry.second, subgroup_num));
        }
        old_group.non_persistent_sst_messages.clear();
    }

    // If the old group was using persistence, we should transfer its state to the new group
    file_writer = std::move(old_group.file_writer);
    if(file_writer) {
        file_writer->set_message_written_upcall(make_file_written_callback());
    }

    initialize_sst_row();
    bool no_member_failed = true;
    if(already_failed.size()) {
        for(uint i = 0; i < num_members; ++i) {
            if(already_failed[i]) {
                no_member_failed = false;
                break;
            }
        }
    }
    if(!already_failed.size() || no_member_failed) {
        // if groups are created successfully, rdmc_sst_groups_created will be set to true
        rdmc_sst_groups_created = create_rdmc_sst_groups();
    }
    register_predicates();
    sender_thread = std::thread(&MulticastGroup::send_loop, this);
    timeout_thread = std::thread(&MulticastGroup::check_failures_loop, this);
}

std::function<void(persistence::message)> MulticastGroup::make_file_written_callback() {
    return [this](persistence::message m) {
        callbacks.local_persistence_callback(m.subgroup_num, m.sender, m.index, m.data,
                                             m.length);
        //m.sender is an ID, not a rank
        uint sender_rank;
        for(sender_rank = 0; sender_rank < num_members; ++sender_rank) {
            if(members[sender_rank] == m.sender) break;
        }
        // m.data points to the char[] buffer in a MessageBuffer, so we need to find
        // the msg corresponding to m and put its MessageBuffer on free_message_buffers
        auto sequence_number = m.index * num_members + sender_rank;
        {
            std::lock_guard<std::mutex> lock(msg_state_mtx);
            auto find_result = non_persistent_messages[m.subgroup_num].find(sequence_number);
            assert(find_result != non_persistent_messages[m.subgroup_num].end());
            RDMCMessage& m_msg = find_result->second;
            free_message_buffers[m.subgroup_num].push_back(std::move(m_msg.message_buffer));
            non_persistent_messages[m.subgroup_num].erase(find_result);
            sst->persisted_num[member_index][m.subgroup_num] = sequence_number;
            sst->put(get_shard_sst_indices(m.subgroup_num),
                     (char*)std::addressof(sst->persisted_num[0][m.subgroup_num]) - sst->getBaseAddress(),
                     sizeof(long long int));
        }

    };
}

bool MulticastGroup::create_rdmc_sst_groups() {
    for(const auto& p : subgroup_to_membership) {
        uint32_t subgroup_num = p.first;
        const std::vector<node_id_t>& shard_members = p.second;
        std::size_t num_shard_members = shard_members.size();
        uint32_t num_shard_senders;
        std::vector<int> shard_senders;
        shard_senders = subgroup_to_senders_and_sender_rank.at(subgroup_num).first;
        num_shard_senders = get_num_senders(shard_senders);
        auto shard_sst_indices = get_shard_sst_indices(subgroup_num);
        sst_multicast_group_ptrs[subgroup_num] = std::make_unique<sst::multicast_group<DerechoSST>>(sst, shard_sst_indices, window_size, shard_senders, subgroup_to_num_received_offset.at(subgroup_num), window_size * subgroup_num);
        for(uint shard_rank = 0, sender_rank = -1; shard_rank < num_shard_members; ++shard_rank) {
            // don't create RDMC group if the shard member is never going to send
            if(!shard_senders[shard_rank]) {
                continue;
            }
            sender_rank++;
            auto node_id = shard_members[shard_rank];
            // When RDMC receives a message, it should store it in
            // locally_stable_rdmc_messages and update the received count
            rdmc::completion_callback_t rdmc_receive_handler;
            rdmc_receive_handler = [this, subgroup_num, shard_rank, sender_rank,
                                    node_id, num_shard_members, num_shard_senders,
                                    shard_sst_indices](char* data, size_t size) {
                assert(this->sst);
                uint32_t num_received_offset = subgroup_to_num_received_offset.at(subgroup_num);
                std::lock_guard<std::mutex> lock(msg_state_mtx);
                header* h = (header*)data;
                long long int index = h->index;
                auto beg_index = index;
                long long int sequence_number = index * num_shard_senders + sender_rank;

                logger->debug("Locally received message in subgroup {}, sender rank {}, index {}", subgroup_num, shard_rank, index);

                // Move message from current_receives to locally_stable_rdmc_messages.
                if(node_id == members[member_index]) {
                    assert(current_sends[subgroup_num]);
                    locally_stable_rdmc_messages[subgroup_num][sequence_number] = std::move(*current_sends[subgroup_num]);
                    current_sends[subgroup_num] = std::experimental::nullopt;
                } else {
                    auto it = current_receives.find({subgroup_num, sequence_number});
                    assert(it != current_receives.end());
                    auto& message = it->second;
                    locally_stable_rdmc_messages[subgroup_num].emplace(sequence_number, std::move(message));
                    current_receives.erase(it);
                }
                // Add empty messages to locally_stable_rdmc_messages for each turn that the sender is skipping.
                for(unsigned int j = 0; j < h->pause_sending_turns; ++j) {
                    index++;
                    sequence_number += num_shard_senders;
                    locally_stable_rdmc_messages[subgroup_num][sequence_number] = {node_id, index, 0, 0};
                }

                auto new_num_received = resolve_num_received(beg_index, index, num_received_offset + sender_rank);
                // deliver immediately if in raw mode
                if(subgroup_to_mode.at(subgroup_num) == Mode::RAW) {
                    // issue stability upcalls for the recently sequenced messages
                    for(uint i = sst->num_received[member_index][num_received_offset + sender_rank] + 1; i <= new_num_received; ++i) {
                        auto seq_num = i * num_shard_senders + sender_rank;
                        if(!locally_stable_sst_messages[subgroup_num].empty()
                           && locally_stable_sst_messages[subgroup_num].begin()->first == seq_num) {
                            auto& msg = locally_stable_sst_messages[subgroup_num].begin()->second;
                            if(msg.size > 0) {
                                char* buf = const_cast<char*>(msg.buf);
                                header* h = (header*)(buf);
                                callbacks.global_stability_callback(subgroup_num, msg.sender_id,
                                                                    msg.index, buf + h->header_size,
                                                                    msg.size - h->header_size);
                            }
                            locally_stable_sst_messages[subgroup_num].erase(locally_stable_sst_messages[subgroup_num].begin());
                        } else {
                            assert(!locally_stable_rdmc_messages[subgroup_num].empty());
                            auto it2 = locally_stable_rdmc_messages[subgroup_num].begin();
                            assert(it2->first == seq_num);
                            auto& msg = it2->second;
                            if(msg.size > 0) {
                                char* buf = msg.message_buffer.buffer.get();
                                header* h = (header*)(buf);
                                callbacks.global_stability_callback(subgroup_num, msg.sender_id,
                                                                    msg.index, buf + h->header_size,
                                                                    msg.size - h->header_size);
                                free_message_buffers[subgroup_num].push_back(std::move(msg.message_buffer));
                            }
                            locally_stable_rdmc_messages[subgroup_num].erase(it2);
                        }
                    }
                }
                if(new_num_received > sst->num_received[member_index][num_received_offset + sender_rank]) {
                    sst->num_received[member_index][num_received_offset + sender_rank] = new_num_received;
                    // std::atomic_signal_fence(std::memory_order_acq_rel);
                    auto* min_ptr = std::min_element(&sst->num_received[member_index][num_received_offset],
                                                     &sst->num_received[member_index][num_received_offset + num_shard_senders]);
                    uint min_index = std::distance(&sst->num_received[member_index][num_received_offset], min_ptr);
                    auto new_seq_num = (*min_ptr + 1) * num_shard_senders + min_index - 1;
                    if((long long int)new_seq_num > sst->seq_num[member_index][subgroup_num]) {
                        logger->debug("Updating seq_num for subgroup {} to {}", subgroup_num, new_seq_num);
                        sst->seq_num[member_index][subgroup_num] = new_seq_num;
                        // std::atomic_signal_fence(std::memory_order_acq_rel);
                        // DERECHO_LOG(node_id, index, "received_message");
                        // DERECHO_LOG(-1, -1, "stable_num_put_start");
                        sst->put(shard_sst_indices,
                                 (char*)std::addressof(sst->seq_num[0][subgroup_num]) - sst->getBaseAddress(),
                                 sizeof(long long int));
                        // DERECHO_LOG(node_id, new_seq_num, "updated_seq_num");
                        // DERECHO_LOG(-1, -1, "stable_num_put_end");
                    }
                    // DERECHO_LOG(-1, -1, "num_received_put_start");
                    sst->put(shard_sst_indices,
                             (char*)std::addressof(sst->num_received[0][num_received_offset + sender_rank]) - sst->getBaseAddress(),
                             sizeof(long long int));
                    // DERECHO_LOG(-1, -1, "num_received_put_end");
                }
            };
            // Capture rdmc_receive_handler by copy! The reference to it won't be valid after this constructor ends!
            auto receive_handler_plus_notify =
                    [this, rdmc_receive_handler](char* data, size_t size) {
                        rdmc_receive_handler(data, size);
                        // signal background writer thread
                        sender_cv.notify_all();
                    };

            // Create a "rotated" vector of members in which the currently selected shard member (shard_rank) is first
            std::vector<uint32_t> rotated_shard_members(shard_members.size());
            for(uint k = 0; k < num_shard_members; ++k) {
                rotated_shard_members[k] = shard_members[(shard_rank + k) % num_shard_members];
            }

            // don't create rdmc group if there's only one member in the shard
            if(num_shard_members <= 1) {
                continue;
            }

            if(node_id == members[member_index]) {
                //Create a group in which this node is the sender, and only self-receives happen
                if(!rdmc::create_group(
                           rdmc_group_num_offset, rotated_shard_members, block_size, type,
                           [this](size_t length) -> rdmc::receive_destination {
                               assert(false);
                               return {nullptr, 0};
                           },
                           receive_handler_plus_notify,
                           [](std::experimental::optional<uint32_t>) {})) {
                    return false;
                }
                subgroup_to_rdmc_group[subgroup_num] = rdmc_group_num_offset;
                rdmc_group_num_offset++;
            } else {
                if(!rdmc::create_group(
                           rdmc_group_num_offset, rotated_shard_members, block_size, type,
                           [this, subgroup_num, node_id, sender_rank, num_shard_senders](size_t length) {
                               std::lock_guard<std::mutex> lock(msg_state_mtx);
                               assert(!free_message_buffers[subgroup_num].empty());
                               //Create a Message struct to receive the data into.
                               RDMCMessage msg;
                               msg.sender_id = node_id;
                               msg.index = sst->num_received[member_index][subgroup_to_num_received_offset.at(subgroup_num) + sender_rank] + 1;
                               msg.size = length;
                               msg.message_buffer = std::move(free_message_buffers[subgroup_num].back());
                               free_message_buffers[subgroup_num].pop_back();

                               rdmc::receive_destination ret{msg.message_buffer.mr, 0};
                               auto sequence_number = msg.index * num_shard_senders + sender_rank;
                               current_receives[{subgroup_num, sequence_number}] = std::move(msg);

                               assert(ret.mr->buffer != nullptr);
                               return ret;
                           },
                           rdmc_receive_handler, [](std::experimental::optional<uint32_t>) {})) {
                    return false;
                }
                rdmc_group_num_offset++;
            }
        }
    }
    //    std::cout << "The members are" << std::endl;
    //    for(uint i = 0; i < num_members; ++i) {
    //        std::cout << members[i] << " ";
    //    }
    //    std::cout << std::endl;
    return true;
}

void MulticastGroup::initialize_sst_row() {
    auto num_received_size = sst->num_received.size();
    auto seq_num_size = sst->seq_num.size();
    for(uint i = 0; i < num_members; ++i) {
        for(uint j = 0; j < num_received_size; ++j) {
            sst->num_received[i][j] = -1;
        }
        for(uint j = 0; j < seq_num_size; ++j) {
            sst->seq_num[i][j] = -1;
            sst->stable_num[i][j] = -1;
            sst->delivered_num[i][j] = -1;
            sst->persisted_num[i][j] = -1;
        }
    }
    sst->put();
    sst->sync_with_members();
}

void MulticastGroup::deliver_message(RDMCMessage& msg, subgroup_id_t subgroup_num) {
    if(msg.size > 0) {
        char* buf = msg.message_buffer.buffer.get();
        header* h = (header*)(buf);
        // cooked send
        if(h->cooked_send) {
            buf += h->header_size;
            auto payload_size = msg.size - h->header_size;
            rpc_callback(subgroup_num, msg.sender_id, buf, payload_size);
        }
        // raw send
        else {
            callbacks.global_stability_callback(subgroup_num, msg.sender_id, msg.index,
                                                buf + h->header_size, msg.size - h->header_size);
        }
        if(file_writer) {
            persistence::message msg_for_filewriter{buf + h->header_size,
                                                    msg.size, (uint32_t)sst->vid[member_index],
                                                    msg.sender_id, (uint64_t)msg.index,
                                                    h->cooked_send};
            //the sequence number needs to use the sender's within-shard rank, not its ID
            auto& shard_members = subgroup_to_membership.at(subgroup_num);
            std::vector<int> shard_senders = subgroup_to_senders_and_sender_rank.at(subgroup_num).first;
            auto num_shard_senders = get_num_senders(shard_senders);
            uint shard_rank;
            uint sender_rank = 0;
            for(shard_rank = 0; shard_rank < shard_members.size(); ++shard_rank) {
                if(shard_members[shard_rank] == msg.sender_id) break;
                if(shard_senders[shard_rank])
                    sender_rank++;
            }
            auto sequence_number = msg.index * num_shard_senders + sender_rank;
            non_persistent_messages[subgroup_num].emplace(sequence_number, std::move(msg));
            file_writer->write_message(msg_for_filewriter);
        } else {
            free_message_buffers[subgroup_num].push_back(std::move(msg.message_buffer));
        }
    }
}

void MulticastGroup::deliver_message(SSTMessage& msg, subgroup_id_t subgroup_num) {
    // DERECHO_LOG(-1, -1, "deliver_message()");
    if(msg.size > 0) {
        char* buf = const_cast<char*>(msg.buf);
        header* h = (header*)(buf);
        // cooked send
        if(h->cooked_send) {
            buf += h->header_size;
            auto payload_size = msg.size - h->header_size;
            rpc_callback(subgroup_num, msg.sender_id, buf, payload_size);
        }
        // raw send
        else {
            // DERECHO_LOG(-1, -1, "start_stability_callback");
            callbacks.global_stability_callback(subgroup_num, msg.sender_id, msg.index,
                                                buf + h->header_size, msg.size - h->header_size);
            // DERECHO_LOG(-1, -1, "end_stability_callback");
        }
        if(file_writer) {
            persistence::message msg_for_filewriter{buf + h->header_size,
                                                    msg.size, (uint32_t)sst->vid[member_index],
                                                    msg.sender_id, (uint64_t)msg.index,
                                                    h->cooked_send};
            //the sequence number needs to use the sender's within-shard rank, not its ID
            auto& shard_members = subgroup_to_membership.at(subgroup_num);
            std::vector<int> shard_senders = subgroup_to_senders_and_sender_rank.at(subgroup_num).first;
            auto num_shard_senders = get_num_senders(shard_senders);
            uint shard_rank;
            uint sender_rank = 0;
            for(shard_rank = 0; shard_rank < shard_members.size(); ++shard_rank) {
                if(shard_members[shard_rank] == msg.sender_id) break;
                if(shard_senders[shard_rank])
                    sender_rank++;
            }
            auto sequence_number = msg.index * num_shard_senders + sender_rank;
            non_persistent_sst_messages[subgroup_num].emplace(sequence_number, std::move(msg));
            file_writer->write_message(msg_for_filewriter);
        }
    }
}

void MulticastGroup::deliver_messages_upto(
        const std::vector<long long int>& max_indices_for_senders,
        subgroup_id_t subgroup_num, uint32_t num_shard_senders) {
    // DERECHO_LOG(-1, -1, "deliver_messages_upto");
    assert(max_indices_for_senders.size() == (size_t)num_shard_senders);
    std::lock_guard<std::mutex> lock(msg_state_mtx);
    auto curr_seq_num = sst->delivered_num[member_index][subgroup_num];
    auto max_seq_num = curr_seq_num;
    for(uint sender = 0; sender < num_shard_senders; sender++) {
        max_seq_num = std::max(max_seq_num,
                               max_indices_for_senders[sender] * num_shard_senders + sender);
    }
    // DERECHO_LOG(-1, -1, "deliver_messages_upto_loop");
    for(auto seq_num = curr_seq_num; seq_num <= max_seq_num; seq_num++) {
        auto msg_ptr = locally_stable_rdmc_messages[subgroup_num].find(seq_num);
        if(msg_ptr != locally_stable_rdmc_messages[subgroup_num].end()) {
            deliver_message(msg_ptr->second, subgroup_num);
            // DERECHO_LOG(-1, -1, "erase_message");
            locally_stable_rdmc_messages[subgroup_num].erase(msg_ptr);
            // DERECHO_LOG(-1, -1, "erase_message_done");
        } else {
            auto sst_msg_ptr = locally_stable_sst_messages[subgroup_num].find(seq_num);
            if(sst_msg_ptr != locally_stable_sst_messages[subgroup_num].end()) {
                deliver_message(sst_msg_ptr->second, subgroup_num);
                // DERECHO_LOG(-1, -1, "erase_message");
                locally_stable_sst_messages[subgroup_num].erase(sst_msg_ptr);
                // DERECHO_LOG(-1, -1, "erase_message_done");
            }
        }
    }
}

void MulticastGroup::register_predicates() {
    for(const auto& p : subgroup_to_shard_and_rank) {
        subgroup_id_t subgroup_num = p.first;
        uint32_t shard_num, shard_index;
        std::tie(shard_num, shard_index) = p.second;
        std::vector<node_id_t> shard_members = subgroup_to_membership.at(subgroup_num);
        auto num_shard_members = shard_members.size();
        auto num_received_offset = subgroup_to_num_received_offset.at(subgroup_num);
        std::vector<int> shard_senders = subgroup_to_senders_and_sender_rank.at(subgroup_num).first;
        auto num_shard_senders = get_num_senders(shard_senders);
        std::map<uint32_t, uint32_t> shard_ranks_by_sender_rank;
        for(uint j = 0, l = 0; j < num_shard_members; ++j) {
            if(shard_senders[j]) {
                shard_ranks_by_sender_rank[l] = j;
                l++;
            }
        }

        auto receiver_pred = [this, subgroup_num, shard_members, num_shard_members,
                              shard_ranks_by_sender_rank, num_shard_senders,
                              num_received_offset](const DerechoSST& sst) {
            for(uint j = 0; j < num_shard_senders; ++j) {
                auto num_received = sst.num_received_sst[member_index][num_received_offset + j] + 1;
                uint32_t slot = num_received % window_size;
                if((long long int)sst.slots[node_id_to_sst_index.at(shard_members[shard_ranks_by_sender_rank.at(j)])]
                                           [subgroup_num * window_size + slot]
                                                   .next_seq
                   == num_received / window_size + 1) {
                    return true;
                }
            }
            return false;
        };
        auto num_times = window_size / 2;
        if(!num_times) {
            num_times = 1;
        }
        auto sst_receive_handler = [this, subgroup_num, shard_members, num_shard_members,
                                    shard_ranks_by_sender_rank, num_shard_senders,
                                    num_received_offset](uint32_t sender_rank, uint64_t index_ignored,
                                                         volatile char* data, uint32_t size) {
            header* h = (header*)data;
            long long int index = h->index;
            auto beg_index = index;
            long long int sequence_number = index * num_shard_senders + sender_rank;
            logger->debug("Locally received message in subgroup {}, sender rank {}, index {}", subgroup_num, sender_rank, index);

            auto node_id = shard_members[shard_ranks_by_sender_rank.at(sender_rank)];

            // DERECHO_LOG(node_id, index, "received_message");
            locally_stable_sst_messages[subgroup_num][sequence_number] = {node_id, index, size, data};

            // Add empty messages to locally_stable_sst_messages for each turn that the sender is skipping.
            for(unsigned int j = 0; j < h->pause_sending_turns; ++j) {
                index++;
                sequence_number += num_shard_senders;
                locally_stable_sst_messages[subgroup_num][sequence_number] = {node_id, index, 0, 0};
            }

            auto new_num_received = resolve_num_received(beg_index, index, num_received_offset + sender_rank);
            if(subgroup_to_mode.at(subgroup_num) == Mode::RAW) {
                // issue stability upcalls for the recently sequenced messages
                for(uint i = sst->num_received[member_index][num_received_offset + sender_rank] + 1; i <= new_num_received; ++i) {
                    auto seq_num = i * num_shard_senders + sender_rank;
                    if(!locally_stable_sst_messages[subgroup_num].empty()
                       && locally_stable_sst_messages[subgroup_num].begin()->first == seq_num) {
                        auto& msg = locally_stable_sst_messages[subgroup_num].begin()->second;
                        if(msg.size > 0) {
                            char* buf = const_cast<char*>(msg.buf);
                            header* h = (header*)(buf);
                            callbacks.global_stability_callback(subgroup_num, msg.sender_id,
                                                                msg.index, buf + h->header_size,
                                                                msg.size - h->header_size);
                        }
                        locally_stable_sst_messages[subgroup_num].erase(locally_stable_sst_messages[subgroup_num].begin());
                    } else {
                        assert(!locally_stable_rdmc_messages[subgroup_num].empty());
                        auto it2 = locally_stable_rdmc_messages[subgroup_num].begin();
                        assert(it2->first == seq_num);
                        auto& msg = it2->second;
                        if(msg.size > 0) {
                            char* buf = msg.message_buffer.buffer.get();
                            header* h = (header*)(buf);
                            callbacks.global_stability_callback(subgroup_num, msg.sender_id,
                                                                msg.index, buf + h->header_size,
                                                                msg.size - h->header_size);
                            free_message_buffers[subgroup_num].push_back(std::move(msg.message_buffer));
                        }
                        locally_stable_rdmc_messages[subgroup_num].erase(it2);
                    }
                }
            }
            sst->num_received[member_index][num_received_offset + sender_rank] = new_num_received;
        };
        uint64_t receiver_cnt = 0;
        auto receiver_trig = [this, num_times, sst_receive_handler, subgroup_num, shard_members,
                              num_shard_members, shard_ranks_by_sender_rank,
                              num_shard_senders, num_received_offset, receiver_cnt](DerechoSST& sst) mutable {
            receiver_cnt++;
            // DERECHO_LOG(receiver_cnt, -1, "in receiver_trig");
            std::lock_guard<std::mutex> lock(msg_state_mtx);
            for(uint i = 0; i < num_times; ++i) {
                for(uint j = 0; j < num_shard_senders; ++j) {
                    auto num_received = sst.num_received_sst[member_index][num_received_offset + j] + 1;
                    uint32_t slot = num_received % window_size;
                    long long int next_seq = (long long int)sst.slots[node_id_to_sst_index.at(shard_members[shard_ranks_by_sender_rank.at(j)])][subgroup_num * window_size + slot].next_seq;
                    if(next_seq == num_received / window_size + 1) {
                        sst_receive_handler(j, num_received,
                                            sst.slots[node_id_to_sst_index.at(shard_members[shard_ranks_by_sender_rank.at(j)])][subgroup_num * window_size + slot].buf,
                                            sst.slots[node_id_to_sst_index.at(shard_members[shard_ranks_by_sender_rank.at(j)])][subgroup_num * window_size + slot].size);
                        sst.num_received_sst[member_index][num_received_offset + j] = num_received;
                    }
                }
            }
            sst.put((char*)std::addressof(sst.num_received_sst[0][num_received_offset]) - sst.getBaseAddress(),
                    sizeof(sst.num_received_sst[0][0]) * num_shard_senders);
            // std::atomic_signal_fence(std::memory_order_acq_rel);
            auto* min_ptr = std::min_element(&sst.num_received[member_index][num_received_offset],
                                             &sst.num_received[member_index][num_received_offset + num_shard_senders]);
            int min_index = std::distance(&sst.num_received[member_index][num_received_offset], min_ptr);
            auto new_seq_num = (*min_ptr + 1) * num_shard_senders + min_index - 1;
            if(new_seq_num > sst.seq_num[member_index][subgroup_num]) {
                logger->debug("Updating seq_num for subgroup {} to {}", subgroup_num, new_seq_num);
                sst.seq_num[member_index][subgroup_num] = new_seq_num;
                sst.put((char*)std::addressof(sst.seq_num[0][subgroup_num]) - sst.getBaseAddress(),
                        sizeof(long long int));
            }
            sst.put((char*)std::addressof(sst.num_received[0][num_received_offset]) - sst.getBaseAddress(),
                    sizeof(long long int) * num_shard_senders);
        };
        receiver_pred_handles.emplace_back(sst->predicates.insert(receiver_pred, receiver_trig,
                                                                  sst::PredicateType::RECURRENT));

        if(subgroup_to_mode.at(subgroup_num) != Mode::RAW) {
            auto stability_pred = [this](
                    const DerechoSST& sst) { return true; };
            auto shard_sst_indices = get_shard_sst_indices(subgroup_num);
            auto stability_trig =
                    [this, subgroup_num, shard_members, num_shard_members, shard_sst_indices](DerechoSST& sst) mutable {
                        // DERECHO_LOG(stability_cnt, -1, "in stability_trig");
                        // compute the min of the seq_num
                        long long int min_seq_num
                                = sst.seq_num[node_id_to_sst_index.at(shard_members[0])][subgroup_num];
                        for(uint i = 0; i < num_shard_members; ++i) {
                            if(sst.seq_num[node_id_to_sst_index.at(shard_members[i])][subgroup_num] < min_seq_num) {
                                min_seq_num
                                        = sst.seq_num[node_id_to_sst_index.at(shard_members[i])][subgroup_num];
                            }
                        }
                        if(min_seq_num > sst.stable_num[member_index][subgroup_num]) {
                            logger->debug("Subgroup {}, updating stable_num to {}", subgroup_num, min_seq_num);
                            sst.stable_num[member_index][subgroup_num] = min_seq_num;
                            // DERECHO_LOG(stability_cnt, min_seq_num, "stability_trig");
                            // DERECHO_LOG(-1, -1, "stability_put_start");
                            sst.put(shard_sst_indices,
                                    (char*)std::addressof(sst.stable_num[0][subgroup_num]) - sst.getBaseAddress(),
                                    sizeof(long long int));
                            // DERECHO_LOG(-1, -1, "stability_put_end");
                            // DERECHO_LOG(stability_cnt, min_seq_num, "updated_stable_num");
                        }
                    };
            stability_pred_handles.emplace_back(sst->predicates.insert(
                    stability_pred, stability_trig, sst::PredicateType::RECURRENT));

            auto delivery_pred = [this](
                    const DerechoSST& sst) { return true; };
            auto delivery_trig = [this, subgroup_num, shard_members, num_shard_members](
                    DerechoSST& sst) mutable {
                // DERECHO_LOG(delivery_cnt, -1, "in delivery_trig");
                std::lock_guard<std::mutex> lock(msg_state_mtx);
                // compute the min of the stable_num
                long long int min_stable_num
                        = sst.stable_num[node_id_to_sst_index.at(shard_members[0])][subgroup_num];
                for(uint i = 0; i < num_shard_members; ++i) {
                    if(sst.stable_num[node_id_to_sst_index.at(shard_members[i])][subgroup_num] < min_stable_num) {
                        min_stable_num = sst.stable_num[node_id_to_sst_index.at(shard_members[i])][subgroup_num];
                    }
                }

                bool update_sst = false;
                while(true) {
                    if(locally_stable_rdmc_messages[subgroup_num].empty() && locally_stable_sst_messages[subgroup_num].empty()) {
                        break;
                    }
                    long long int least_undelivered_rdmc_seq_num, least_undelivered_sst_seq_num;
                    least_undelivered_rdmc_seq_num = least_undelivered_sst_seq_num = std::numeric_limits<long long int>::max();
                    if(!locally_stable_rdmc_messages[subgroup_num].empty()) {
                        least_undelivered_rdmc_seq_num = locally_stable_rdmc_messages[subgroup_num].begin()->first;
                    }
                    if(!locally_stable_sst_messages[subgroup_num].empty()) {
                        least_undelivered_sst_seq_num = locally_stable_sst_messages[subgroup_num].begin()->first;
                    }
                    if(least_undelivered_rdmc_seq_num < least_undelivered_sst_seq_num && least_undelivered_rdmc_seq_num <= min_stable_num) {
                        update_sst = true;
                        logger->debug("Subgroup {}, can deliver a locally stable message: min_stable_num={} and least_undelivered_seq_num={}",
                                      subgroup_num, min_stable_num, least_undelivered_rdmc_seq_num);
                        RDMCMessage& msg = locally_stable_rdmc_messages[subgroup_num].begin()->second;
                        deliver_message(msg, subgroup_num);
                        // DERECHO_LOG(-1, -1, "deliver_message() done");
                        sst.delivered_num[member_index][subgroup_num] = least_undelivered_rdmc_seq_num;
                        locally_stable_rdmc_messages[subgroup_num].erase(locally_stable_rdmc_messages[subgroup_num].begin());
                        // DERECHO_LOG(-1, -1, "message_erase_done");
                    } else if(least_undelivered_sst_seq_num < least_undelivered_rdmc_seq_num && least_undelivered_sst_seq_num <= min_stable_num) {
                        update_sst = true;
                        logger->debug("Subgroup {}, can deliver a locally stable message: min_stable_num={} and least_undelivered_seq_num={}",
                                      subgroup_num, min_stable_num, least_undelivered_sst_seq_num);
                        SSTMessage& msg = locally_stable_sst_messages[subgroup_num].begin()->second;
                        deliver_message(msg, subgroup_num);
                        // DERECHO_LOG(-1, -1, "deliver_message() done");
                        sst.delivered_num[member_index][subgroup_num] = least_undelivered_sst_seq_num;
                        locally_stable_sst_messages[subgroup_num].erase(locally_stable_sst_messages[subgroup_num].begin());
                        // DERECHO_LOG(-1, -1, "message_erase_done");
                    } else {
                        break;
                    }
                }
                if(update_sst) {
                    // DERECHO_LOG(-1, -1, "delivery_put_start");
                    sst.put(get_shard_sst_indices(subgroup_num),
                            (char*)std::addressof(sst.delivered_num[0][subgroup_num]) - sst.getBaseAddress(),
                            sizeof(long long int));
                    // DERECHO_LOG(-1, -1, "delivery_put_end");
                }
            };

            delivery_pred_handles.emplace_back(sst->predicates.insert(delivery_pred, delivery_trig, sst::PredicateType::RECURRENT));

            int shard_sender_index;
            std::tie(shard_senders, shard_sender_index) = subgroup_to_senders_and_sender_rank.at(subgroup_num);
            num_shard_senders = get_num_senders(shard_senders);
            if(shard_sender_index >= 0) {
                auto sender_pred = [this, subgroup_num, shard_members, num_shard_members, shard_sender_index, num_shard_senders](const DerechoSST& sst) {
                    long long int seq_num = next_message_to_deliver[subgroup_num] * num_shard_senders + shard_sender_index;
                    for(uint i = 0; i < num_shard_members; ++i) {
                        if(sst.delivered_num[node_id_to_sst_index.at(shard_members[i])][subgroup_num] < seq_num
                           || (file_writer && sst.persisted_num[node_id_to_sst_index.at(shard_members[i])][subgroup_num] < seq_num)) {
                            return false;
                        }
                    }
                    return true;
                };
                auto sender_trig = [this, subgroup_num](DerechoSST& sst) {
                    sender_cv.notify_all();
                    next_message_to_deliver[subgroup_num]++;
                };
                sender_pred_handles.emplace_back(sst->predicates.insert(sender_pred, sender_trig,
                                                                        sst::PredicateType::RECURRENT));
            }
        } else {
            int shard_sender_index;
            std::tie(shard_senders, shard_sender_index) = subgroup_to_senders_and_sender_rank.at(subgroup_num);
            num_shard_senders = get_num_senders(shard_senders);
            if(shard_sender_index >= 0) {
                auto sender_pred = [this, subgroup_num, shard_members, num_shard_members, shard_sender_index, num_shard_senders](const DerechoSST& sst) {
                    for(uint i = 0; i < num_shard_members; ++i) {
                        auto num_received_offset = subgroup_to_num_received_offset.at(subgroup_num);
                        if(sst.num_received[node_id_to_sst_index.at(shard_members[i])][num_received_offset + shard_sender_index]
                           < (long long int)(future_message_indices[subgroup_num] - 1 - window_size)) {
                            return false;
                        }
                    }
                    return true;
                };
                auto sender_trig = [this, subgroup_num](DerechoSST& sst) {
                    sender_cv.notify_all();
                };
                sender_pred_handles.emplace_back(sst->predicates.insert(sender_pred, sender_trig,
                                                                        sst::PredicateType::RECURRENT));
            }
        }
    }
}

MulticastGroup::~MulticastGroup() {
    wedge();
    if(timeout_thread.joinable()) {
        timeout_thread.join();
    }
}

long long unsigned int MulticastGroup::compute_max_msg_size(
        const long long unsigned int max_payload_size,
        const long long unsigned int block_size) {
    auto max_msg_size = max_payload_size + sizeof(header);
    if(max_msg_size % block_size != 0) {
        max_msg_size = (max_msg_size / block_size + 1) * block_size;
    }
    return max_msg_size;
}

void MulticastGroup::wedge() {
    bool thread_shutdown_existing = thread_shutdown.exchange(true);
    if(thread_shutdown_existing) {  // Wedge has already been called
        return;
    }

    //Consume and remove all the predicate handles
    for(auto handle_iter = sender_pred_handles.begin(); handle_iter != sender_pred_handles.end();) {
        sst->predicates.remove(*handle_iter);
        handle_iter = sender_pred_handles.erase(handle_iter);
    }
    for(auto handle_iter = receiver_pred_handles.begin(); handle_iter != receiver_pred_handles.end();) {
        sst->predicates.remove(*handle_iter);
        handle_iter = receiver_pred_handles.erase(handle_iter);
    }
    for(auto handle_iter = stability_pred_handles.begin(); handle_iter != stability_pred_handles.end();) {
        sst->predicates.remove(*handle_iter);
        handle_iter = stability_pred_handles.erase(handle_iter);
    }
    for(auto handle_iter = delivery_pred_handles.begin(); handle_iter != delivery_pred_handles.end();) {
        sst->predicates.remove(*handle_iter);
        handle_iter = delivery_pred_handles.erase(handle_iter);
    }

    for(uint i = 0; i < num_members; ++i) {
        rdmc::destroy_group(i + rdmc_group_num_offset);
    }

    sender_cv.notify_all();
    if(sender_thread.joinable()) {
        sender_thread.join();
    }
}

void MulticastGroup::send_loop() {
    pthread_setname_np(pthread_self(), "sender_thread");
    subgroup_id_t subgroup_to_send = 0;
    auto should_send_to_subgroup = [&](subgroup_id_t subgroup_num) {
        if(!rdmc_sst_groups_created) {
            return false;
        }
        if(pending_sends[subgroup_num].empty()) {
            return false;
        }
        RDMCMessage& msg = pending_sends[subgroup_num].front();
        uint32_t shard_num;
        uint32_t shard_index;

        std::tie(shard_num, shard_index) = subgroup_to_shard_and_rank.at(subgroup_num);
        uint32_t num_shard_senders;
        int shard_sender_index;
        std::vector<int> shard_senders;
        std::tie(shard_senders, shard_sender_index) = subgroup_to_senders_and_sender_rank.at(subgroup_num);
        num_shard_senders = get_num_senders(shard_senders);
        assert(shard_sender_index >= 0);

        // std::cout << "num_received offset = " << subgroup_to_num_received_offset.at(subgroup_num) + shard_sender_index <<
        //         ", num_received entry " <<  sst->num_received[member_index][subgroup_to_num_received_offset.at(subgroup_num) + shard_sender_index] <<
        //         ", message index = " << msg.index << std::endl;
        if(sst->num_received[member_index][subgroup_to_num_received_offset.at(subgroup_num) + shard_sender_index] < msg.index - 1) {
            return false;
        }

        std::vector<node_id_t> shard_members = subgroup_to_membership.at(subgroup_num);
        auto num_shard_members = shard_members.size();
        assert(num_shard_members >= 1);
        if(subgroup_to_mode.at(subgroup_num) != Mode::RAW) {
            for(uint i = 0; i < num_shard_members; ++i) {
                if(sst->delivered_num[node_id_to_sst_index.at(shard_members[i])][subgroup_num] < (long long int)((msg.index - window_size) * num_shard_senders + shard_sender_index)
                   || (file_writer && sst->persisted_num[node_id_to_sst_index.at(shard_members[i])][subgroup_num] < (long long int)((msg.index - window_size) * num_shard_senders + shard_sender_index))) {
                    return false;
                }
            }
        } else {
            for(uint i = 0; i < num_shard_members; ++i) {
                auto num_received_offset = subgroup_to_num_received_offset.at(subgroup_num);
                if(sst->num_received[node_id_to_sst_index.at(shard_members[i])][num_received_offset + shard_sender_index] < (long long int)(future_message_indices[subgroup_num] - 1 - window_size)) {
                    return false;
                }
            }
        }

        return true;
    };
    auto should_send = [&]() {
        for(uint i = 1; i <= total_num_subgroups; ++i) {
            auto subgroup_num = (subgroup_to_send + i) % total_num_subgroups;
            if(should_send_to_subgroup(subgroup_num)) {
                subgroup_to_send = subgroup_num;
                return true;
            }
        }
        return false;
    };
    auto should_wake = [&]() { return thread_shutdown || should_send(); };
    try {
        std::unique_lock<std::mutex> lock(msg_state_mtx);
        while(!thread_shutdown) {
            sender_cv.wait(lock, should_wake);
            // DERECHO_LOG(send_cnt, -1, "sender thread woke up");
            if(!thread_shutdown) {
                current_sends[subgroup_to_send] = std::move(pending_sends[subgroup_to_send].front());
                // DERECHO_LOG(-1, -1, "got_current_send");
                logger->debug("Calling send in subgroup {} on message {} from sender {}", subgroup_to_send, current_sends[subgroup_to_send]->index, current_sends[subgroup_to_send]->sender_id);
                // DERECHO_LOG(-1, -1, "did_log_event");
                if(!rdmc::send(subgroup_to_rdmc_group[subgroup_to_send],
                               current_sends[subgroup_to_send]->message_buffer.mr, 0,
                               current_sends[subgroup_to_send]->size)) {
                    throw std::runtime_error("rdmc::send returned false");
                }
                // DERECHO_LOG(-1, -1, "issued_rdmc_send");
                pending_sends[subgroup_to_send].pop();
            }
        }
        std::cout << "DerechoGroup send thread shutting down" << std::endl;
    } catch(const std::exception& e) {
        std::cout << "DerechoGroup send thread had an exception: " << e.what() << std::endl;
    }
}

void MulticastGroup::check_failures_loop() {
    pthread_setname_np(pthread_self(), "timeout_thread");
    static int cnt = 0;
    while(!thread_shutdown) {
        std::this_thread::sleep_for(std::chrono::milliseconds(sender_timeout));
        if(sst) {
            sst->put_with_completion((char*)std::addressof(sst->heartbeat[0]) - sst->getBaseAddress(), sizeof(bool));
	    cnt++;
	    // std::cout << (char*)std::addressof(sst->heartbeat[0]) - sst->getBaseAddress() << std::endl;
	    // std::cout << cnt << std::endl;
        }
    }

    std::cout << "timeout_thread shutting down" << std::endl;
}

char* MulticastGroup::get_sendbuffer_ptr(subgroup_id_t subgroup_num,
                                         long long unsigned int payload_size,
                                         bool transfer_medium, int pause_sending_turns,
                                         bool cooked_send, bool null_send) {
    // if rdmc groups were not created because of failures, return NULL
    if(!rdmc_sst_groups_created) {
        return NULL;
    }
    long long unsigned int msg_size = payload_size + sizeof(header);
    // payload_size is 0 when max_msg_size is desired, useful for ordered send/query
    if(!payload_size) {
        msg_size = max_msg_size;
    }
    if(null_send) {
        msg_size = sizeof(header);
    }
    if(msg_size > max_msg_size) {
        std::cout << "Can't send messages of size larger than the maximum message "
                     "size which is equal to "
                  << max_msg_size << std::endl;
        return nullptr;
    }

    uint32_t shard_num, shard_index;
    std::tie(shard_num, shard_index) = subgroup_to_shard_and_rank.at(subgroup_num);
    std::vector<node_id_t> shard_members = subgroup_to_membership.at(subgroup_num);
    auto num_shard_members = shard_members.size();
    // if the current node is not a sender, shard_sender_index will be -1
    uint32_t num_shard_senders;
    int shard_sender_index;
    std::vector<int> shard_senders;
    std::tie(shard_senders, shard_sender_index) = subgroup_to_senders_and_sender_rank.at(subgroup_num);
    num_shard_senders = get_num_senders(shard_senders);
    assert(shard_sender_index >= 0);

    if(subgroup_to_mode.at(subgroup_num) != Mode::RAW) {
        for(uint i = 0; i < num_shard_members; ++i) {
            if(sst->delivered_num[node_id_to_sst_index.at(shard_members[i])][subgroup_num] < (long long int)((future_message_indices[subgroup_num] - window_size) * num_shard_senders + shard_sender_index)) {
                return nullptr;
            }
        }
    } else {
        for(uint i = 0; i < num_shard_members; ++i) {
            auto num_received_offset = subgroup_to_num_received_offset.at(subgroup_num);
            if(sst->num_received[node_id_to_sst_index.at(shard_members[i])][num_received_offset + shard_sender_index] < (long long int)(future_message_indices[subgroup_num] - window_size)) {
                return nullptr;
            }
        }
    }

    if(thread_shutdown) {
        return nullptr;
    }

    if(transfer_medium) {
        std::unique_lock<std::mutex> lock(msg_state_mtx);
        if(free_message_buffers[subgroup_num].empty()) return nullptr;

        // Create new Message
        RDMCMessage msg;
        msg.sender_id = members[member_index];
        msg.index = future_message_indices[subgroup_num];
        msg.size = msg_size;
        msg.message_buffer = std::move(free_message_buffers[subgroup_num].back());
        free_message_buffers[subgroup_num].pop_back();

        // Fill header
        char* buf = msg.message_buffer.buffer.get();
        ((header*)buf)->header_size = sizeof(header);
        ((header*)buf)->pause_sending_turns = pause_sending_turns;
        ((header*)buf)->index = msg.index;
        ((header*)buf)->cooked_send = cooked_send;

        next_sends[subgroup_num] = std::move(msg);
        future_message_indices[subgroup_num] += pause_sending_turns + 1;

        last_transfer_medium[subgroup_num] = transfer_medium;
        // DERECHO_LOG(-1, -1, "provided a buffer");
        return buf + sizeof(header);
    } else {
        char* buf = (char*)sst_multicast_group_ptrs[subgroup_num]->get_buffer(msg_size);
        if(!buf) {
            return nullptr;
        }
        ((header*)buf)->header_size = sizeof(header);
        ((header*)buf)->pause_sending_turns = pause_sending_turns;
        ((header*)buf)->index = future_message_indices[subgroup_num];
        ((header*)buf)->cooked_send = cooked_send;
        future_message_indices[subgroup_num] += pause_sending_turns + 1;

        last_transfer_medium[subgroup_num] = transfer_medium;
        // DERECHO_LOG(-1, -1, "provided a buffer");
        return buf + sizeof(header);
    }
}

bool MulticastGroup::send(subgroup_id_t subgroup_num) {
    if(thread_shutdown || !rdmc_sst_groups_created) {
        return false;
    }
    if(last_transfer_medium[subgroup_num]) {
        std::lock_guard<std::mutex> lock(msg_state_mtx);
        assert(next_sends[subgroup_num]);
        pending_sends[subgroup_num].push(std::move(*next_sends[subgroup_num]));
        next_sends[subgroup_num] = std::experimental::nullopt;
        sender_cv.notify_all();
        // DERECHO_LOG(-1, -1, "user_send_finished");
        return true;
    } else {
        sst_multicast_group_ptrs[subgroup_num]->send();
        // DERECHO_LOG(-1, -1, "user_send_finished");
        return true;
    }
}

std::vector<uint32_t> MulticastGroup::get_shard_sst_indices(subgroup_id_t subgroup_num) {
    std::vector<node_id_t> shard_members = subgroup_to_membership.at(subgroup_num);

    std::vector<uint32_t> shard_sst_indices;
    for(auto m : shard_members) {
        shard_sst_indices.push_back(node_id_to_sst_index.at(m));
    }
    return shard_sst_indices;
}

void MulticastGroup::debug_print() {
    using std::cout;
    using std::endl;
    cout << "In DerechoGroup SST has " << sst->get_num_rows()
         << " rows; member_index is " << member_index << endl;
    uint num_received_offset = 0;
    cout << "Printing SST" << endl;
    for(uint subgroup_num = 0; subgroup_num < total_num_subgroups; ++subgroup_num) {
        cout << "Subgroup " << subgroup_num << endl;
        cout << "Printing seq_num, stable_num, delivered_num" << endl;
        for(uint i = 0; i < num_members; ++i) {
            cout << sst->seq_num[i][subgroup_num] << " " << sst->stable_num[i][subgroup_num] << " " << sst->delivered_num[i][subgroup_num] << endl;
        }
        cout << endl;

        uint32_t num_shard_senders;
        int shard_sender_index;
        std::vector<int> shard_senders;
        std::tie(shard_senders, shard_sender_index) = subgroup_to_senders_and_sender_rank.at(subgroup_num);
        num_shard_senders = get_num_senders(shard_senders);
        cout << "Printing last_received_messages" << endl;
        for(uint k = 0; k < num_members; ++k) {
            for(uint i = 0; i < num_shard_senders; ++i) {
                cout << sst->num_received[k][num_received_offset + i] << " ";
            }
            cout << endl;
        }
        num_received_offset += num_shard_senders;
        cout << "Printing multicastSST fields" << endl;
        sst_multicast_group_ptrs[subgroup_num]->debug_print();
        cout << endl;
    }
}

}  // namespace derecho
