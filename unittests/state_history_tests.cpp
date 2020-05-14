#include <boost/test/unit_test.hpp>
#include <contracts.hpp>
#include <eosio/state_history/create_deltas.hpp>
#include <eosio/state_history/log.hpp>
#include <eosio/state_history/trace_converter.hpp>
#include <eosio/testing/tester.hpp>
#include <fc/io/json.hpp>

#include "test_cfd_transaction.hpp"
#include <boost/filesystem.hpp>

#undef N

#include <eosio/stream.hpp>
#include <eosio/ship_protocol.hpp>

using namespace eosio;
using namespace testing;
using namespace chain;
using prunable_data_type = eosio::chain::packed_transaction::prunable_data_type;

namespace bio = boost::iostreams;
extern const char* const state_history_plugin_abi;

prunable_data_type::prunable_data_t get_prunable_data_from_traces(std::vector<state_history::transaction_trace>& traces,
                                                                  const transaction_id_type&                     id) {
   auto cfd_trace_itr = std::find_if(traces.begin(), traces.end(), [id](const state_history::transaction_trace& v) {
      return v.get<state_history::transaction_trace_v0>().id == id;
   });

   // make sure the trace with cfd can be found
   BOOST_REQUIRE(cfd_trace_itr != traces.end());
   BOOST_REQUIRE(cfd_trace_itr->contains<state_history::transaction_trace_v0>());
   auto trace_v0 = cfd_trace_itr->get<state_history::transaction_trace_v0>();
   BOOST_REQUIRE(trace_v0.partial);
   BOOST_REQUIRE(trace_v0.partial->contains<state_history::partial_transaction_v1>());
   return trace_v0.partial->get<state_history::partial_transaction_v1>().prunable_data->prunable_data;
}

prunable_data_type::prunable_data_t get_prunable_data_from_traces_bin(const std::vector<char>&   entry,
                                                                      const transaction_id_type& id) {
   fc::datastream<const char*>                   strm(entry.data(), entry.size());
   std::vector<state_history::transaction_trace> traces;
   state_history::trace_converter::unpack(strm, traces);
   return get_prunable_data_from_traces(traces, id);
}

BOOST_AUTO_TEST_SUITE(test_state_history)

BOOST_AUTO_TEST_CASE(test_trace_converter) {

   tester chain;
   using namespace eosio::state_history;

   state_history::transaction_trace_cache cache;
   std::map<uint32_t, chain::bytes>              on_disk_log_entries;

   chain.control->applied_transaction.connect(
       [&](std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> t) {
          cache.add_transaction(std::get<0>(t), std::get<1>(t));
       });

   chain.control->accepted_block.connect([&](const block_state_ptr& bs) {
      auto                              traces = cache.prepare_traces(bs);
      fc::datastream<std::vector<char>> strm;
      state_history::trace_converter::pack(strm, chain.control->db(), true, traces,
                                           state_history::compression_type::zlib);
      on_disk_log_entries[bs->block_num] = strm.storage();
   });

   deploy_test_api(chain);
   auto cfd_trace = push_test_cfd_transaction(chain);
   chain.produce_blocks(1);

   BOOST_CHECK(on_disk_log_entries.size());

   // Now deserialize the on disk trace log and make sure that the cfd exists
   auto& cfd_entry = on_disk_log_entries.at(cfd_trace->block_num);
   BOOST_REQUIRE(!get_prunable_data_from_traces_bin(cfd_entry, cfd_trace->id).contains<prunable_data_type::none>());

   // prune the cfd for the block
   std::vector<transaction_id_type> ids{cfd_trace->id};
   fc::datastream<char*>            rw_strm(cfd_entry.data(), cfd_entry.size());
   state_history::trace_converter::prune_traces(rw_strm, cfd_entry.size(), ids);
   BOOST_CHECK(ids.size() == 0);

   // read the pruned trace and make sure it's pruned
   BOOST_CHECK(get_prunable_data_from_traces_bin(cfd_entry, cfd_trace->id).contains<prunable_data_type::none>());
}

BOOST_AUTO_TEST_CASE(test_trace_log) {
   namespace bfs = boost::filesystem;
   tester chain;

   scoped_temp_path state_history_dir;
   fc::create_directories(state_history_dir.path);
   state_history_traces_log log(state_history_dir.path);

   chain.control->applied_transaction.connect(
       [&](std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> t) {
          log.add_transaction(std::get<0>(t), std::get<1>(t));
       });

   chain.control->accepted_block.connect([&](const block_state_ptr& bs) { log.store(chain.control->db(), bs); });

   deploy_test_api(chain);
   auto cfd_trace = push_test_cfd_transaction(chain);
   chain.produce_blocks(1);

   auto traces = log.get_traces(cfd_trace->block_num);
   BOOST_REQUIRE(traces.size());

   BOOST_REQUIRE(!get_prunable_data_from_traces(traces, cfd_trace->id).contains<prunable_data_type::none>());

   std::vector<transaction_id_type> ids{cfd_trace->id};
   log.prune_transactions(cfd_trace->block_num, ids);
   BOOST_REQUIRE(ids.empty());

   // we assume the nodeos has to be stopped while running, it can only be read
   // correctly with restart
   state_history_traces_log new_log(state_history_dir.path);
   auto                     pruned_traces = new_log.get_traces(cfd_trace->block_num);
   BOOST_REQUIRE(pruned_traces.size());

   BOOST_CHECK(get_prunable_data_from_traces(pruned_traces, cfd_trace->id).contains<prunable_data_type::none>());
}

BOOST_AUTO_TEST_CASE(test_state_result_abi) {
   using namespace state_history;

   tester chain;

   transaction_trace_cache      trace_cache;
   std::map<uint32_t, chain::bytes>    history;
   fc::optional<block_position> prev_block;

   chain.control->applied_transaction.connect(
       [&](std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> t) {
          trace_cache.add_transaction(std::get<0>(t), std::get<1>(t));
       });

   chain.control->accepted_block.connect([&](const block_state_ptr& block_state) {
      auto& control = chain.control;

      fc::datastream<std::vector<char>> strm;
      trace_converter::pack(strm, control->db(), false, trace_cache.prepare_traces(block_state),
                            compression_type::none);
      strm.seekp(0);

      get_blocks_result_v1 message;
      message.head = block_position{control->head_block_num(), control->head_block_id()};
      message.last_irreversible =
          state_history::block_position{control->last_irreversible_block_num(), control->last_irreversible_block_id()};
      message.this_block = state_history::block_position{block_state->block->block_num(), block_state->id};
      message.prev_block = prev_block;
      message.block      = block_state->block;
      state_history::trace_converter::unpack(strm, message.traces);
      message.deltas = fc::raw::pack(state_history::create_deltas(control->db(), !prev_block));

      prev_block                         = message.this_block;
      history[control->head_block_num()] = fc::raw::pack(state_history::state_result{message});
   });

   deploy_test_api(chain);
   auto cfd_trace = push_test_cfd_transaction(chain);
   chain.produce_blocks(1);

   abi_serializer serializer{fc::json::from_string(state_history_plugin_abi).as<abi_def>(),
                             abi_serializer::create_yield_function(chain.abi_serializer_max_time)};

   for (auto& [key, value] : history) {
      // check the validity of the abi string
      fc::datastream<const char*> strm(value.data(), value.size());
      serializer.binary_to_variant("result", strm,
                                   abi_serializer::create_yield_function(chain.abi_serializer_max_time));
      BOOST_CHECK(value.size() == strm.tellp());

      // check the validity of abieos ship_protocol type definitions
      eosio::input_stream  bin{ value.data(), value.data() + value.size() };
      eosio::ship_protocol::result result;
      BOOST_CHECK_NO_THROW(from_bin(result, bin));
      BOOST_CHECK(bin.remaining() == 0);

      std::vector<eosio::ship_protocol::table_delta> deltas;
      auto deltas_bin =  std::get<eosio::ship_protocol::get_blocks_result_v1>(result).deltas;
      BOOST_CHECK_NO_THROW(from_bin(deltas, deltas_bin));
      BOOST_CHECK(deltas_bin.remaining() == 0);
   }
}


BOOST_AUTO_TEST_SUITE_END()