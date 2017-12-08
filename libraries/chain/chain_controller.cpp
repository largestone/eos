/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */

#include <eosio/chain/chain_controller.hpp>
#include <eosio/chain/contracts/staked_balance_objects.hpp>

#include <eosio/chain/block_summary_object.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/contracts/contract_table_objects.hpp>
#include <eosio/chain/action_objects.hpp>
#include <eosio/chain/generated_transaction_object.hpp>
#include <eosio/chain/transaction_object.hpp>
#include <eosio/chain/producer_object.hpp>
#include <eosio/chain/permission_link_object.hpp>
#include <eosio/chain/authority_checker.hpp>
#include <eosio/chain/contracts/chain_initializer.hpp>
#include <eosio/chain/contracts/producer_objects.hpp>
#include <eosio/chain/scope_sequence_object.hpp>
#include <eosio/chain/merkle.hpp>

#include <eosio/chain/wasm_interface.hpp>

#include <eos/utilities/rand.hpp>

#include <fc/smart_ref_impl.hpp>
#include <fc/uint128.hpp>
#include <fc/crypto/digest.hpp>

#include <boost/range/algorithm/copy.hpp>
#include <boost/range/algorithm_ext/is_sorted.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/adaptor/map.hpp>

#include <fstream>
#include <functional>
#include <iostream>
#include <chrono>

namespace eosio { namespace chain {

bool is_start_of_round( block_num_type block_num ) {
  return (block_num % config::blocks_per_round) == 0; 
}

chain_controller::chain_controller( const chain_controller::controller_config& cfg )
:_db( cfg.shared_memory_dir, 
      (cfg.read_only ? database::read_only : database::read_write), 
      cfg.shared_memory_size), 
 _block_log(cfg.block_log_dir) 
{
   _initialize_indexes();

   contracts::chain_initializer starter(cfg.genesis);
   starter.register_types(*this, _db);

   // Behave as though we are applying a block during chain initialization (it's the genesis block!)
   with_applying_block([&] {
      _initialize_chain(starter);
   });

   _spinup_db();
   _spinup_fork_db();

   if (_block_log.read_head() && head_block_num() < _block_log.read_head()->block_num())
      replay();
} /// chain_controller::chain_controller


chain_controller::~chain_controller() {
   clear_pending();
   _db.flush();
}

bool chain_controller::is_known_block(const block_id_type& id)const
{
   return _fork_db.is_known_block(id) || _block_log.read_block_by_id(id);
}
/**
 * Only return true *if* the transaction has not expired or been invalidated. If this
 * method is called with a VERY old transaction we will return false, they should
 * query things by blocks if they are that old.
 */
bool chain_controller::is_known_transaction(const transaction_id_type& id)const
{
   const auto& trx_idx = _db.get_index<transaction_multi_index, by_trx_id>();
   return trx_idx.find( id ) != trx_idx.end();
}

block_id_type chain_controller::get_block_id_for_num(uint32_t block_num)const
{ try {
   if (const auto& block = fetch_block_by_number(block_num))
      return block->id();

   FC_THROW_EXCEPTION(unknown_block_exception, "Could not find block");
} FC_CAPTURE_AND_RETHROW((block_num)) }

optional<signed_block> chain_controller::fetch_block_by_id(const block_id_type& id)const
{
   auto b = _fork_db.fetch_block(id);
   if(b) return b->data;
   return _block_log.read_block_by_id(id);
}

optional<signed_block> chain_controller::fetch_block_by_number(uint32_t num)const
{
   if (const auto& block = _block_log.read_block_by_num(num))
      return *block;

   // Not in _block_log, so it must be since the last irreversible block. Grab it from _fork_db instead
   if (num <= head_block_num()) {
      auto block = _fork_db.head();
      while (block && block->num > num)
         block = block->prev.lock();
      if (block && block->num == num)
         return block->data;
   }

   return optional<signed_block>();
}

/*
const signed_transaction& chain_controller::get_recent_transaction(const transaction_id_type& trx_id) const
{
   auto& index = _db.get_index<transaction_multi_index, by_trx_id>();
   auto itr = index.find(trx_id);
   FC_ASSERT(itr != index.end());
   return itr->trx;
}
*/

std::vector<block_id_type> chain_controller::get_block_ids_on_fork(block_id_type head_of_fork) const
{
  pair<fork_database::branch_type, fork_database::branch_type> branches = _fork_db.fetch_branch_from(head_block_id(), head_of_fork);
  if( !((branches.first.back()->previous_id() == branches.second.back()->previous_id())) )
  {
     edump( (head_of_fork)
            (head_block_id())
            (branches.first.size())
            (branches.second.size()) );
     assert(branches.first.back()->previous_id() == branches.second.back()->previous_id());
  }
  std::vector<block_id_type> result;
  for (const item_ptr& fork_block : branches.second)
    result.emplace_back(fork_block->id);
  result.emplace_back(branches.first.back()->previous_id());
  return result;
}


/**
 * Push block "may fail" in which case every partial change is unwound.  After
 * push block is successful the block is appended to the chain database on disk.
 *
 * @return true if we switched forks as a result of this push.
 */
void chain_controller::push_block(const signed_block& new_block, uint32_t skip)
{ try {
   with_skip_flags( skip, [&](){ 
      return without_pending_transactions( [&]() {
         return _db.with_write_lock( [&]() {
            return _push_block(new_block);
         } );
      });
   });
} FC_CAPTURE_AND_RETHROW((new_block)) }

bool chain_controller::_push_block(const signed_block& new_block)
{ try {
   uint32_t skip = _skip_flags;
   if (!(skip&skip_fork_db)) {
      /// TODO: if the block is greater than the head block and before the next maintenance interval
      // verify that the block signer is in the current set of active producers.

      shared_ptr<fork_item> new_head = _fork_db.push_block(new_block);
      //If the head block from the longest chain does not build off of the current head, we need to switch forks.
      if (new_head->data.previous != head_block_id()) {
         //If the newly pushed block is the same height as head, we get head back in new_head
         //Only switch forks if new_head is actually higher than head
         if (new_head->data.block_num() > head_block_num()) {
            wlog("Switching to fork: ${id}", ("id",new_head->data.id()));
            auto branches = _fork_db.fetch_branch_from(new_head->data.id(), head_block_id());

            // pop blocks until we hit the forked block
            while (head_block_id() != branches.second.back()->data.previous)
               pop_block();

            // push all blocks on the new fork
            for (auto ritr = branches.first.rbegin(); ritr != branches.first.rend(); ++ritr) {
                ilog("pushing blocks from fork ${n} ${id}", ("n",(*ritr)->data.block_num())("id",(*ritr)->data.id()));
                optional<fc::exception> except;
                try {
                   auto session = _db.start_undo_session(true);
                   _apply_block((*ritr)->data, skip);
                   session.push();
                }
                catch (const fc::exception& e) { except = e; }
                if (except) {
                   wlog("exception thrown while switching forks ${e}", ("e",except->to_detail_string()));
                   // remove the rest of branches.first from the fork_db, those blocks are invalid
                   while (ritr != branches.first.rend()) {
                      _fork_db.remove((*ritr)->data.id());
                      ++ritr;
                   }
                   _fork_db.set_head(branches.second.front());

                   // pop all blocks from the bad fork
                   while (head_block_id() != branches.second.back()->data.previous)
                      pop_block();

                   // restore all blocks from the good fork
                   for (auto ritr = branches.second.rbegin(); ritr != branches.second.rend(); ++ritr) {
                      auto session = _db.start_undo_session(true);
                      _apply_block((*ritr)->data, skip);
                      session.push();
                   }
                   throw *except;
                }
            }
            return true; //swithced fork
         }
         else return false; // didn't switch fork
      }
   }

   try {
      auto session = _db.start_undo_session(true);
      _apply_block(new_block, skip);
      session.push();
   } catch ( const fc::exception& e ) {
      elog("Failed to push new block:\n${e}", ("e", e.to_detail_string()));
      _fork_db.remove(new_block.id());
      throw;
   }

   return false;
} FC_CAPTURE_AND_RETHROW((new_block)) }

/**
 * Attempts to push the transaction into the pending queue
 *
 * When called to push a locally generated transaction, set the skip_block_size_check bit on the skip argument. This
 * will allow the transaction to be pushed even if it causes the pending block size to exceed the maximum block size.
 * Although the transaction will probably not propagate further now, as the peers are likely to have their pending
 * queues full as well, it will be kept in the queue to be propagated later when a new block flushes out the pending
 * queues.
 */
transaction_trace chain_controller::push_transaction(const signed_transaction& trx, uint32_t skip)
{ try {
   return with_skip_flags(skip, [&]() {
      return _db.with_write_lock([&]() {
         return _push_transaction(trx);
      });
   });
} FC_CAPTURE_AND_RETHROW((trx)) }

transaction_trace chain_controller::_push_transaction(const signed_transaction& trx) {
   // If this is the first transaction pushed after applying a block, start a new undo session.
   // This allows us to quickly rewind to the clean state of the head block, in case a new block arrives.
   if( !_pending_block ) {
      _start_pending_block();
   }

   auto temp_session = _db.start_undo_session(true);

   // for now apply the transaction serially but schedule it according to those invariants
   validate_referenced_accounts(trx);
   check_transaction_authorization(trx);

   auto shardnum = _pending_cycle.schedule( trx );
   auto cyclenum = _pending_block->regions.back().cycles_summary.size() - 1;
   if (shardnum == -1) {
      cyclenum += 1;
   }


   /// TODO: move _pending_cycle into db so that it can be undone if transation fails, for now we will apply
   /// the transaction first so that there is nothing to undo... this only works because things are currently
   /// single threaded
   transaction_metadata   mtrx( trx, get_chain_id(), _pending_block->regions.back().region, cyclenum, 0 );
   auto result = _apply_transaction( mtrx );


   if( shardnum == -1 ) { /// schedule conflict start new cycle
      _finalize_pending_cycle();
      _start_pending_cycle();
      shardnum = _pending_cycle.schedule( trx );
   }


   auto& bcycle = _pending_block->regions.back().cycles_summary.back();
   if( shardnum >= bcycle.size() ) {
      _start_pending_shard();
   }


   auto tid = trx.id();
   bcycle.at(shardnum).emplace_back( tid );
   _pending_cycle_trace->shard_traces.at(shardnum).append(result);

   /** for now we will just shove everything into the first shard */
   _pending_block->input_transactions.push_back(trx);

   // The transaction applied successfully. Merge its changes into the pending block session.
   temp_session.squash();

   // notify anyone listening to pending transactions
   on_pending_transaction(trx);

   return result;
}



void chain_controller::_start_pending_block()
{
   FC_ASSERT( !_pending_block );
   _pending_block         = signed_block();
   _pending_block_trace   = block_trace(*_pending_block);
   _pending_block_session = _db.start_undo_session(true);
   _pending_block->regions.resize(1);
   _pending_block_trace->region_traces.resize(1);
   _start_pending_cycle();
}

/**
 *  Wraps up all work for current shards, starts a new cycle, and
 *  executes any pending transactions
 */
void chain_controller::_start_pending_cycle() {
   _pending_block->regions.back().cycles_summary.resize( _pending_block->regions[0].cycles_summary.size() + 1 );
   _pending_cycle = pending_cycle_state();
   _pending_cycle_trace = cycle_trace();
   _start_pending_shard();

   /// TODO: check for deferred transactions and schedule them
} // _start_pending_cycle

void chain_controller::_start_pending_shard()
{
   auto& bcycle = _pending_block->regions.back().cycles_summary.back();
   bcycle.resize( bcycle.size()+1 );

   _pending_cycle_trace->shard_traces.resize(_pending_cycle_trace->shard_traces.size() + 1 );
}

void chain_controller::_finalize_pending_cycle()
{
   for( auto& shard : _pending_cycle_trace->shard_traces ) {
      shard.calculate_root();
   }

   _apply_cycle_trace(*_pending_cycle_trace);
   _pending_block_trace->region_traces.back().cycle_traces.emplace_back(std::move(*_pending_cycle_trace));
   _pending_cycle_trace.reset();
}

void chain_controller::_apply_cycle_trace( const cycle_trace& res )
{
   for (const auto&st: res.shard_traces) {
      for (const auto &tr: st.transaction_traces) {
         for (const auto &dt: tr.deferred_transactions) {
            _db.create<generated_transaction_object>([&](auto &obj) {
               obj.trx_id = dt.id();
               obj.sender = dt.sender;
               obj.sender_id = dt.sender_id;
               obj.expiration = dt.expiration;
               obj.delay_until = dt.execute_after;
               obj.packed_trx.resize(fc::raw::pack_size(dt));
               fc::datastream<char *> ds(obj.packed_trx.data(), obj.packed_trx.size());
               fc::raw::pack(ds, dt);
            });
         }

         ///TODO: hook this up as a signal handler in a de-coupled "logger" that may just silently drop them
         for (const auto &ar : tr.action_traces) {
            if (!ar.console.empty()) {
               auto prefix = fc::format_string(
                  "[(${s},${a})->${r}]",
                  fc::mutable_variant_object()
                     ("s", ar.act.scope)
                     ("a", ar.act.name)
                     ("r", ar.receiver));
               std::cerr << prefix << ": CONSOLE OUTPUT BEGIN =====================" << std::endl;
               std::cerr << ar.console;
               std::cerr << prefix << ": CONSOLE OUTPUT END   =====================" << std::endl;
            }
         }
      }
   }
}

/**
 *  After applying all transactions successfully we can update
 *  the current block time, block number, producer stats, etc
 */
void chain_controller::_finalize_block( const block_trace& trace ) { try {
   const auto& b = trace.block;
   const producer_object& signing_producer = validate_block_header(_skip_flags, b);

   update_global_properties( b );
   update_global_dynamic_data( b );
   update_signing_producer(signing_producer, b);
   update_last_irreversible_block();

   create_block_summary(b);
   clear_expired_transactions();

   applied_block( trace ); //emit
   if (_currently_replaying_blocks)
     applied_irreversible_block(b);

} FC_CAPTURE_AND_RETHROW( (trace.block) ) }

signed_block chain_controller::generate_block(
   block_timestamp_type when,
   account_name producer,
   const private_key_type& block_signing_private_key,
   uint32_t skip /* = 0 */
   )
{ try {
   return with_skip_flags( skip, [&](){
      return _db.with_write_lock( [&](){
         return _generate_block( when, producer, block_signing_private_key );
      });
   });
} FC_CAPTURE_AND_RETHROW( (when) ) }

signed_block chain_controller::_generate_block( block_timestamp_type when, 
                                              account_name producer, 
                                              const private_key_type& block_signing_key )
{ try {
   uint32_t skip     = _skip_flags;
   uint32_t slot_num = get_slot_at_time( when );
   FC_ASSERT( slot_num > 0 );
   account_name scheduled_producer = get_scheduled_producer( slot_num );
   FC_ASSERT( scheduled_producer == producer );

   const auto& producer_obj = get_producer(scheduled_producer);

   if( !_pending_block ) {
      _start_pending_block();
   }

   _finalize_pending_cycle();

   if( !(skip & skip_producer_signature) )
      FC_ASSERT( producer_obj.signing_key == block_signing_key.get_public_key() );

      _pending_block->timestamp   = when;
      _pending_block->producer    = producer_obj.owner;
      _pending_block->previous    = head_block_id();
      _pending_block->block_mroot = get_dynamic_global_properties().block_merkle_root.get_root();
      _pending_block->transaction_mroot = _pending_block->calculate_transaction_merkle_root();
      _pending_block->action_mroot = _pending_block_trace->calculate_action_merkle_root();


      if( is_start_of_round( _pending_block->block_num() ) ) {
      auto latest_producer_schedule = _calculate_producer_schedule();
      if( latest_producer_schedule != _head_producer_schedule() )
         _pending_block->new_producers = latest_producer_schedule;
   }

   if( !(skip & skip_producer_signature) )
      _pending_block->sign( block_signing_key );

   _finalize_block( *_pending_block_trace );

   _pending_block_session->push();

   auto result = move( *_pending_block );

   _pending_block_trace.reset();
   _pending_block.reset();
   _pending_block_session.reset();

   if (!(skip&skip_fork_db)) {
      _fork_db.push_block(result);
   }
   return result;

} FC_CAPTURE_AND_RETHROW( (producer) ) }

/**
 * Removes the most recent block from the database and undoes any changes it made.
 */
void chain_controller::pop_block()
{ try {
   _pending_block_session.reset();
   auto head_id = head_block_id();
   optional<signed_block> head_block = fetch_block_by_id( head_id );
   EOS_ASSERT( head_block.valid(), pop_empty_chain, "there are no blocks to pop" );

   _fork_db.pop_block();
   _db.undo();
} FC_CAPTURE_AND_RETHROW() }

void chain_controller::clear_pending()
{ try {
   _pending_block_trace.reset();
   _pending_block.reset();
   _pending_block_session.reset();
} FC_CAPTURE_AND_RETHROW() }

//////////////////// private methods ////////////////////

void chain_controller::_apply_block(const signed_block& next_block, uint32_t skip)
{
   auto block_num = next_block.block_num();
   if (_checkpoints.size() && _checkpoints.rbegin()->second != block_id_type()) {
      auto itr = _checkpoints.find(block_num);
      if (itr != _checkpoints.end())
         FC_ASSERT(next_block.id() == itr->second,
                   "Block did not match checkpoint", ("checkpoint",*itr)("block_id",next_block.id()));

      if (_checkpoints.rbegin()->first >= block_num)
         skip = ~0;// WE CAN SKIP ALMOST EVERYTHING
   }

   with_applying_block([&] {
      with_skip_flags(skip, [&] {
         __apply_block(next_block);
      });
   });
}


void chain_controller::__apply_block(const signed_block& next_block)
{ try {
   uint32_t skip = _skip_flags;

   /*
   FC_ASSERT((skip & skip_merkle_check) 
             || next_block.transaction_merkle_root == next_block.calculate_merkle_root(),
             "", ("next_block.transaction_merkle_root", next_block.transaction_merkle_root)
             ("calc",next_block.calculate_merkle_root())("next_block",next_block)("id",next_block.id()));
             */

   const producer_object& signing_producer = validate_block_header(skip, next_block);

   /// regions must be listed in order
   for( uint32_t i = 1; i < next_block.regions.size(); ++i )
      FC_ASSERT( next_block.regions[i-1].region < next_block.regions[i].region );


   /// cache the input tranasction ids so that they can be looked up when executing the
   /// summary
   map<transaction_id_type,const signed_transaction*> trx_index;
   for( const auto& t : next_block.input_transactions ) {
      trx_index[t.id()] = &t;
   }

   block_trace next_block_trace(next_block);
   next_block_trace.region_traces.reserve(next_block.regions.size());

   for( const auto& r : next_block.regions ) {
      region_trace r_trace;
      r_trace.cycle_traces.reserve(r.cycles_summary.size());

      for (uint32_t cycle_index = 0; cycle_index < r.cycles_summary.size(); cycle_index++) {
         const auto& cycle = r.cycles_summary.at(cycle_index);
         cycle_trace c_trace;
         c_trace.shard_traces.reserve(cycle.size());

         uint32_t shard_index = 0;
         for (const auto& shard: cycle) {
            shard_trace s_trace;
            for (const auto& receipt : shard) {
               if( receipt.status == transaction_receipt::executed ) {
                  auto itr = trx_index.find(receipt.id);
                  if( itr != trx_index.end() ) {
                     transaction_metadata mtrx( *itr->second, get_chain_id(), r.region, cycle_index, shard_index );
                     s_trace.append( _apply_transaction( mtrx ) );
                  }
                  else
                  {
                     FC_ASSERT( !"deferred transactions not yet supported" );
                  }
               }
               // validate_referenced_accounts(trx);
               // Check authorization, and allow irrelevant signatures.
               // If the block producer let it slide, we'll roll with it.
               // check_transaction_authorization(trx, true);
            } /// for each transaction id

            ++shard_index;
            s_trace.calculate_root();
            c_trace.shard_traces.emplace_back(move(s_trace));
         } /// for each shard

         _apply_cycle_trace(c_trace);
         r_trace.cycle_traces.emplace_back(move(c_trace));
      } /// for each cycle

      next_block_trace.region_traces.emplace_back(move(r_trace));
   } /// for each region

   FC_ASSERT(next_block.action_mroot == next_block_trace.calculate_action_merkle_root());

   _finalize_block( next_block_trace );
} FC_CAPTURE_AND_RETHROW( (next_block.block_num()) )  }

flat_set<public_key_type> chain_controller::get_required_keys(const signed_transaction& trx,
                                                              const flat_set<public_key_type>& candidate_keys)const 
{
   auto checker = make_auth_checker( [&](auto p){ return get_permission(p).auth; }, 
                                     get_global_properties().configuration.max_authority_depth,
                                     candidate_keys);

   for (const auto& act : trx.actions ) {
      for (const auto& declared_auth : act.authorization) {
         if (!checker.satisfied(declared_auth)) {
            EOS_ASSERT(checker.satisfied(declared_auth), tx_missing_sigs,
                       "transaction declares authority '${auth}', but does not have signatures for it.",
                       ("auth", declared_auth));
         }
      }
   }

   return checker.used_keys();
}

void chain_controller::check_authorization( const transaction& trx, 
                                            flat_set<public_key_type> provided_keys,
                                            bool allow_unused_signatures,
                                            flat_set<account_name>    provided_accounts  )const
{
   auto checker = make_auth_checker( [&](auto p){ return get_permission(p).auth; }, 
                                     get_global_properties().configuration.max_authority_depth,
                                     provided_keys, provided_accounts );


   for( const auto& act : trx.actions ) {
      for( const auto& declared_auth : act.authorization ) {

         const auto& min_permission = lookup_minimum_permission(declared_auth.actor, 
                                                                act.scope, act.name);

         if ((_skip_flags & skip_authority_check) == false) {
            const auto& index = _db.get_index<permission_index>().indices();
            EOS_ASSERT(get_permission(declared_auth).satisfies(min_permission, index), 
                       tx_irrelevant_auth,
                       "action declares irrelevant authority '${auth}'; minimum authority is ${min}",
                       ("auth", declared_auth)("min", min_permission.name));
         }
         if ((_skip_flags & skip_transaction_signatures) == false) {
            EOS_ASSERT(checker.satisfied(declared_auth), tx_missing_sigs,
                       "transaction declares authority '${auth}', but does not have signatures for it.",
                       ("auth", declared_auth));
         }
      }
   }

   if (!allow_unused_signatures && (_skip_flags & skip_transaction_signatures) == false)
      EOS_ASSERT(checker.all_keys_used(), tx_irrelevant_sig,
                 "transaction bears irrelevant signatures from these keys: ${keys}", 
                 ("keys", checker.unused_keys()));
}

void chain_controller::check_transaction_authorization(const signed_transaction& trx, 
                                                       bool allow_unused_signatures)const 
{
   check_authorization( trx, trx.get_signature_keys( chain_id_type{} ), allow_unused_signatures );
}

void chain_controller::validate_scope( const transaction& trx )const {
   for( uint32_t i = 1; i < trx.read_scope.size(); ++i ) {
      EOS_ASSERT( trx.read_scope[i-1] < trx.read_scope[i], transaction_exception, 
                  "Scopes must be sorted and unique" );
   }
   for( uint32_t i = 1; i < trx.write_scope.size(); ++i ) {
      EOS_ASSERT( trx.write_scope[i-1] < trx.write_scope[i], transaction_exception, 
                  "Scopes must be sorted and unique" );
   }

   /**
    * We need to verify that all authorizing accounts have write scope because write
    * access is necessary to update bandwidth usage.
    */
   ///{
   auto has_write_scope = [&]( auto s ) { return std::binary_search( trx.write_scope.begin(),
                                                                     trx.write_scope.end(),
                                                                     s ); };
   for( const auto& a : trx.actions ) {
      for( const auto& auth : a.authorization ) {
         FC_ASSERT( has_write_scope( auth.actor ), "write scope of the authorizing account is required" );
      }
   }
   ///@}

   vector<account_name> intersection;
   std::set_intersection( trx.read_scope.begin(), trx.read_scope.end(),
                          trx.write_scope.begin(), trx.write_scope.end(),
                          std::back_inserter(intersection) );
   FC_ASSERT( intersection.size() == 0, "a transaction may not redeclare scope in readscope" );
}

const permission_object& chain_controller::lookup_minimum_permission(account_name authorizer_account,
                                                                    account_name scope,
                                                                    action_name act_name) const {
   try {
      // First look up a specific link for this message act_name
      auto key = boost::make_tuple(authorizer_account, scope, act_name);
      auto link = _db.find<permission_link_object, by_action_name>(key);
      // If no specific link found, check for a contract-wide default
      if (link == nullptr) {
         get<2>(key) = "";
         link = _db.find<permission_link_object, by_action_name>(key);
      }

      // If no specific or default link found, use active permission
      auto permission_key = boost::make_tuple<account_name, permission_name>(authorizer_account, config::active_name );
      if (link != nullptr)
         get<1>(permission_key) = link->required_permission;
      return _db.get<permission_object, by_owner>(permission_key);
   } FC_CAPTURE_AND_RETHROW((authorizer_account)(scope)(act_name))
}

void chain_controller::validate_uniqueness( const signed_transaction& trx )const {
   if( !should_check_for_duplicate_transactions() ) return;

   auto transaction = _db.find<transaction_object, by_trx_id>(trx.id());
   EOS_ASSERT(transaction == nullptr, tx_duplicate, "transaction is not unique");
}

void chain_controller::record_transaction(const signed_transaction& trx) {
   //Insert transaction into unique transactions database.
    _db.create<transaction_object>([&](transaction_object& transaction) {
        transaction.trx_id = trx.id(); 
        transaction.expiration = trx.expiration;
    });
}


void chain_controller::validate_tapos(const transaction& trx)const {
   if (!should_check_tapos()) return;

   const auto& tapos_block_summary = _db.get<block_summary_object>((uint16_t)trx.ref_block_num);

   //Verify TaPoS block summary has correct ID prefix, and that this block's time is not past the expiration
   EOS_ASSERT(trx.verify_reference_block(tapos_block_summary.block_id), transaction_exception,
              "transaction's reference block did not match. Is this transaction from a different fork?",
              ("tapos_summary", tapos_block_summary));
}

void chain_controller::validate_referenced_accounts( const transaction& trx )const 
{ try { 
   for( const auto& scope : trx.read_scope )
      require_scope(scope);
   for( const auto& scope : trx.write_scope )
      require_scope(scope);

   for( const auto& act : trx.actions ) {
      require_account(act.scope);
      for (const auto& auth : act.authorization )
         require_account(auth.actor);
   }
} FC_CAPTURE_AND_RETHROW() }

void chain_controller::validate_expiration( const transaction& trx ) const
{ try {
   fc::time_point now = head_block_time();
   const auto& chain_configuration = get_global_properties().configuration;

   EOS_ASSERT( time_point(trx.expiration) <= now + fc::seconds(chain_configuration.max_transaction_lifetime),
              transaction_exception, "transaction expiration is too far in the future",
              ("trx.expiration",trx.expiration)("now",now)
              ("max_til_exp",chain_configuration.max_transaction_lifetime));
   EOS_ASSERT( now <= time_point(trx.expiration), transaction_exception, "transaction is expired",
              ("now",now)("trx.exp",trx.expiration));
} FC_CAPTURE_AND_RETHROW((trx)) }


void chain_controller::require_scope( const scope_name& scope )const {
   switch( uint64_t(scope) ) {
      case config::eosio_all_scope:
      case config::eosio_auth_scope:
         return; /// built in scopes
      default:
         require_account(scope);
   }
}

void chain_controller::require_account(const account_name& name) const {
   auto account = _db.find<account_object, by_name>(name);
   FC_ASSERT(account != nullptr, "Account not found: ${name}", ("name", name));
}

const producer_object& chain_controller::validate_block_header(uint32_t skip, const signed_block& next_block)const {
   EOS_ASSERT(head_block_id() == next_block.previous, block_validate_exception, "",
              ("head_block_id",head_block_id())("next.prev",next_block.previous));
   EOS_ASSERT(head_block_time() < (fc::time_point)next_block.timestamp, block_validate_exception, "",
              ("head_block_time",head_block_time())("next",next_block.timestamp)("blocknum",next_block.block_num()));
   if (next_block.block_num() % config::blocks_per_round != 0) {
      EOS_ASSERT(!next_block.new_producers, block_validate_exception,
                 "Producer changes may only occur at the end of a round.");
   }
   
   const producer_object& producer = get_producer(get_scheduled_producer(get_slot_at_time(next_block.timestamp)));

   if(!(skip&skip_producer_signature))
      EOS_ASSERT(next_block.validate_signee(producer.signing_key), block_validate_exception,
                 "Incorrect block producer key: expected ${e} but got ${a}",
                 ("e", producer.signing_key)("a", public_key_type(next_block.signee())));

   if(!(skip&skip_producer_schedule_check)) {
      EOS_ASSERT(next_block.producer == producer.owner, block_validate_exception,
                 "Producer produced block at wrong time",
                 ("block producer",next_block.producer)("scheduled producer",producer.owner));
   }

   
   FC_ASSERT( next_block.calculate_transaction_merkle_root() == next_block.transaction_mroot, "merkle root does not match" );

   return producer;
}

void chain_controller::create_block_summary(const signed_block& next_block) {
   auto sid = next_block.block_num() & 0xffff;
   _db.modify( _db.get<block_summary_object,by_id>(sid), [&](block_summary_object& p) {
         p.block_id = next_block.id();
   });
}

/**
 *  Takes the top config::producer_count producers by total vote excluding any producer whose
 *  block_signing_key is null.  
 */
producer_schedule_type chain_controller::_calculate_producer_schedule()const {
   const auto& producers_by_vote = _db.get_index<contracts::producer_votes_multi_index,contracts::by_votes>();
   auto itr = producers_by_vote.begin();
   producer_schedule_type schedule;
   uint32_t count = 0;
   while( itr != producers_by_vote.end() && count < schedule.producers.size() ) {
      schedule.producers[count].producer_name = itr->owner_name;
      schedule.producers[count].block_signing_key = get_producer(itr->owner_name).signing_key;
      ++itr;
      if( schedule.producers[count].block_signing_key != public_key_type() ) {
         ++count;
      }
   }
   const auto& hps = _head_producer_schedule();
   schedule.version = hps.version;
   if( hps != schedule )
      ++schedule.version;
   return schedule;
}

/**
 *  Returns the most recent and/or pending producer schedule
 */
const producer_schedule_type& chain_controller::_head_producer_schedule()const {
   const auto& gpo = get_global_properties();
   if( gpo.pending_active_producers.size() ) 
      return gpo.pending_active_producers.back().second;
   return gpo.active_producers;
}

void chain_controller::update_global_properties(const signed_block& b) { try {
   // If we're at the end of a round, update the BlockchainConfiguration, producer schedule
   // and "producers" special account authority
   if( is_start_of_round( b.block_num() ) ) {
      auto schedule = _calculate_producer_schedule();
      if( b.new_producers )
      {
          FC_ASSERT( schedule == *b.new_producers, "pending producer set different than expected" );
      }

      const auto& gpo = get_global_properties();

      if( _head_producer_schedule() != schedule ) {
         FC_ASSERT( b.new_producers, "pending producer set changed but block didn't indicate it" );
      }
      _db.modify( gpo, [&]( auto& props ) {
         if( props.pending_active_producers.size() && props.pending_active_producers.back().first == b.block_num() )
            props.pending_active_producers.back().second = schedule;
         else
            props.pending_active_producers.push_back( make_pair(b.block_num(),schedule) );
      });



      auto active_producers_authority = authority(config::producers_authority_threshold, {}, {});
      for(auto& name : gpo.active_producers.producers ) {
         active_producers_authority.accounts.push_back({{name.producer_name, config::active_name}, 1});
      }

      auto& po = _db.get<permission_object, by_owner>( boost::make_tuple(config::producers_account_name, 
                                                                         config::active_name ) );
      _db.modify(po,[active_producers_authority] (permission_object& po) {
         po.auth = active_producers_authority;
      });
   }
} FC_CAPTURE_AND_RETHROW() } 

void chain_controller::add_checkpoints( const flat_map<uint32_t,block_id_type>& checkpts ) {
   for (const auto& i : checkpts)
      _checkpoints[i.first] = i.second;
}

bool chain_controller::before_last_checkpoint()const {
   return (_checkpoints.size() > 0) && (_checkpoints.rbegin()->first >= head_block_num());
}

const global_property_object& chain_controller::get_global_properties()const {
   return _db.get<global_property_object>();
}

const dynamic_global_property_object&chain_controller::get_dynamic_global_properties() const {
   return _db.get<dynamic_global_property_object>();
}

time_point chain_controller::head_block_time()const {
   return get_dynamic_global_properties().time;
}

uint32_t chain_controller::head_block_num()const {
   return get_dynamic_global_properties().head_block_number;
}

block_id_type chain_controller::head_block_id()const {
   return get_dynamic_global_properties().head_block_id;
}

account_name chain_controller::head_block_producer() const {
   auto b = _fork_db.fetch_block(head_block_id());
   if( b ) return b->data.producer;

   if (auto head_block = fetch_block_by_id(head_block_id()))
      return head_block->producer;
   return {};
}

const producer_object& chain_controller::get_producer(const account_name& owner_name) const 
{ try {
   return _db.get<producer_object, by_owner>(owner_name);
} FC_CAPTURE_AND_RETHROW( (owner_name) ) }

const permission_object&   chain_controller::get_permission( const permission_level& level )const 
{ try {
   return _db.get<permission_object, by_owner>( boost::make_tuple(level.actor,level.permission) );
} FC_CAPTURE_AND_RETHROW( (level) ) }

uint32_t chain_controller::last_irreversible_block_num() const {
   return get_dynamic_global_properties().last_irreversible_block_num;
}

void chain_controller::_initialize_indexes() {
   _db.add_index<account_index>();
   _db.add_index<permission_index>();
   _db.add_index<permission_link_index>();
   _db.add_index<action_permission_index>();
   _db.add_index<contracts::table_id_multi_index>();
   _db.add_index<contracts::key_value_index>();
   _db.add_index<contracts::keystr_value_index>();
   _db.add_index<contracts::key128x128_value_index>();
   _db.add_index<contracts::key64x64x64_value_index>();

   _db.add_index<global_property_multi_index>();
   _db.add_index<dynamic_global_property_multi_index>();
   _db.add_index<block_summary_multi_index>();
   _db.add_index<transaction_multi_index>();
   _db.add_index<generated_transaction_multi_index>();
   _db.add_index<producer_multi_index>();
   _db.add_index<scope_sequence_multi_index>();
   _db.add_index<bandwidth_usage_index>();
   _db.add_index<compute_usage_index>();
}

void chain_controller::_initialize_chain(contracts::chain_initializer& starter)
{ try {
   if (!_db.find<global_property_object>()) {
      _db.with_write_lock([this, &starter] {
         auto initial_timestamp = starter.get_chain_start_time();
         FC_ASSERT(initial_timestamp != time_point(), "Must initialize genesis timestamp." );
         FC_ASSERT( block_timestamp_type(initial_timestamp) == initial_timestamp,
                    "Genesis timestamp must be divisible by config::block_interval_ms" );

         // Create global properties
         _db.create<global_property_object>([&starter](global_property_object& p) {
            p.configuration = starter.get_chain_start_configuration();
            p.active_producers = starter.get_chain_start_producers();
         });

         _db.create<dynamic_global_property_object>([&](dynamic_global_property_object& p) {
            p.time = initial_timestamp;
            p.recent_slots_filled = uint64_t(-1);
         });

         // Initialize block summary index
         for (int i = 0; i < 0x10000; i++)
            _db.create<block_summary_object>([&](block_summary_object&) {});

         auto acts = starter.prepare_database(*this, _db);

         transaction genesis_setup_transaction;
         genesis_setup_transaction.write_scope = { config::eosio_all_scope };
         genesis_setup_transaction.actions = move(acts);

         ilog( "applying genesis transaction" );
         with_skip_flags(skip_scope_check | skip_transaction_signatures | skip_authority_check | received_block, 
         [&](){ 
            transaction_metadata tmeta( genesis_setup_transaction );
            _apply_transaction( tmeta );
         });

      });
   }
} FC_CAPTURE_AND_RETHROW() }


void chain_controller::replay() {
   ilog("Replaying blockchain");
   auto start = fc::time_point::now();

   auto on_exit = fc::make_scoped_exit([&_currently_replaying_blocks = _currently_replaying_blocks](){
      _currently_replaying_blocks = false;
   });
   _currently_replaying_blocks = true;

   auto last_block = _block_log.read_head();
   if (!last_block) {
      elog("No blocks in block log; skipping replay");
      return;
   }

   const auto last_block_num = last_block->block_num();

   ilog("Replaying ${n} blocks...", ("n", last_block_num) );
   for (uint32_t i = 1; i <= last_block_num; ++i) {
      if (i % 5000 == 0)
         std::cerr << "   " << double(i*100)/last_block_num << "%   "<<i << " of " <<last_block_num<<"   \n";
      fc::optional<signed_block> block = _block_log.read_block_by_num(i);
      FC_ASSERT(block, "Could not find block #${n} in block_log!", ("n", i));
      _apply_block(*block, skip_producer_signature |
                          skip_transaction_signatures |
                          skip_transaction_dupe_check |
                          skip_tapos_check |
                          skip_producer_schedule_check |
                          skip_authority_check |
                          received_block);
   }
   auto end = fc::time_point::now();
   ilog("Done replaying ${n} blocks, elapsed time: ${t} sec",
        ("n", head_block_num())("t",double((end-start).count())/1000000.0));

   _db.set_revision(head_block_num());
}

void chain_controller::_spinup_db() {
   // Rewind the database to the last irreversible block
   _db.with_write_lock([&] {
      _db.undo_all();
      FC_ASSERT(_db.revision() == head_block_num(), "Chainbase revision does not match head block num",
                ("rev", _db.revision())("head_block", head_block_num()));

   });
}

void chain_controller::_spinup_fork_db()
{
   fc::optional<signed_block> last_block = _block_log.read_head();
   if(last_block.valid()) {
      _fork_db.start_block(*last_block);
      if (last_block->id() != head_block_id()) {
           FC_ASSERT(head_block_num() == 0, "last block ID does not match current chain state",
                     ("last_block->id", last_block->id())("head_block_num",head_block_num()));
      }
   }
}

/*
ProducerRound chain_controller::calculate_next_round(const signed_block& next_block) {
   auto schedule = _admin->get_next_round(_db);
   auto changes = get_global_properties().active_producers - schedule;
   EOS_ASSERT(boost::range::equal(next_block.producer_changes, changes), block_validate_exception,
              "Unexpected round changes in new block header",
              ("expected changes", changes)("block changes", next_block.producer_changes));
   
   fc::time_point tp = (fc::time_point)next_block.timestamp;
   utilities::rand::random rng(tp.sec_since_epoch());
   rng.shuffle(schedule);
   return schedule;
}*/

void chain_controller::update_global_dynamic_data(const signed_block& b) {
   const dynamic_global_property_object& _dgp = _db.get<dynamic_global_property_object>();

   const auto& bmroot = _dgp.block_merkle_root.get_root();
   FC_ASSERT( bmroot == b.block_mroot, "block merkle root does not match expected value" );

   uint32_t missed_blocks = head_block_num() == 0? 1 : get_slot_at_time((fc::time_point)b.timestamp);
   assert(missed_blocks != 0);
   missed_blocks--;

//   if (missed_blocks)
//      wlog("Blockchain continuing after gap of ${b} missed blocks", ("b", missed_blocks));

   for(uint32_t i = 0; i < missed_blocks; ++i) {
      const auto& producer_missed = get_producer(get_scheduled_producer(i+1));
      if(producer_missed.owner != b.producer) {
         /*
         const auto& producer_account = producer_missed.producer_account(*this);
         if( (fc::time_point::now() - b.timestamp) < fc::seconds(30) )
            wlog( "Producer ${name} missed block ${n} around ${t}", ("name",producer_account.name)("n",b.block_num())("t",b.timestamp) );
            */

         _db.modify( producer_missed, [&]( producer_object& w ) {
           w.total_missed++;
         });
      }
   }

   // dynamic global properties updating
   _db.modify( _dgp, [&]( dynamic_global_property_object& dgp ){
      dgp.head_block_number = b.block_num();
      dgp.head_block_id = b.id();
      dgp.time = b.timestamp;
      dgp.current_producer = b.producer;
      dgp.current_absolute_slot += missed_blocks+1;
      dgp.averge_block_size.add_usage( fc::raw::pack_size(b), b.timestamp );

      // If we've missed more blocks than the bitmap stores, skip calculations and simply reset the bitmap
      if (missed_blocks < sizeof(dgp.recent_slots_filled) * 8) {
         dgp.recent_slots_filled <<= 1;
         dgp.recent_slots_filled += 1;
         dgp.recent_slots_filled <<= missed_blocks;
      } else {
         dgp.recent_slots_filled = 0;
      }
      dgp.block_merkle_root.append( head_block_id() ); 
   });

   _fork_db.set_max_size( _dgp.head_block_number - _dgp.last_irreversible_block_num + 1 );
}

void chain_controller::update_signing_producer(const producer_object& signing_producer, const signed_block& new_block)
{
   const dynamic_global_property_object& dpo = get_dynamic_global_properties();
   uint64_t new_block_aslot = dpo.current_absolute_slot + get_slot_at_time( (fc::time_point)new_block.timestamp );

   _db.modify( signing_producer, [&]( producer_object& _wit )
   {
      _wit.last_aslot = new_block_aslot;
      _wit.last_confirmed_block_num = new_block.block_num();
   } );
}

void chain_controller::update_last_irreversible_block()
{
   const global_property_object& gpo = get_global_properties();
   const dynamic_global_property_object& dpo = get_dynamic_global_properties();

   vector<const producer_object*> producer_objs;
   producer_objs.reserve(gpo.active_producers.producers.size());

   std::transform(gpo.active_producers.producers.begin(), gpo.active_producers.producers.end(), std::back_inserter(producer_objs),
                  [this](const producer_key& pk) { return &get_producer(pk.producer_name); });

   static_assert(config::irreversible_threshold_percent > 0, "irreversible threshold must be nonzero");

   size_t offset = EOS_PERCENT(producer_objs.size(), config::percent_100- config::irreversible_threshold_percent);
   std::nth_element(producer_objs.begin(), producer_objs.begin() + offset, producer_objs.end(),
      [](const producer_object* a, const producer_object* b) {
         return a->last_confirmed_block_num < b->last_confirmed_block_num;
      });

   uint32_t new_last_irreversible_block_num = producer_objs[offset]->last_confirmed_block_num;

   if (new_last_irreversible_block_num > dpo.last_irreversible_block_num) {
      _db.modify(dpo, [&](dynamic_global_property_object& _dpo) {
         _dpo.last_irreversible_block_num = new_last_irreversible_block_num;
      });
   }

   // Write newly irreversible blocks to disk. First, get the number of the last block on disk...
   auto old_last_irreversible_block = _block_log.head();
   int last_block_on_disk = 0;
   // If this is null, there are no blocks on disk, so the zero is correct
   if (old_last_irreversible_block)
      last_block_on_disk = old_last_irreversible_block->block_num();

   if (last_block_on_disk < new_last_irreversible_block_num) {
      for (auto block_to_write = last_block_on_disk + 1;
           block_to_write <= new_last_irreversible_block_num;
           ++block_to_write) {
         auto block = fetch_block_by_number(block_to_write);
         assert(block);
         _block_log.append(*block);
         applied_irreversible_block(*block);
      }
   }

   if( new_last_irreversible_block_num > last_block_on_disk ) {
      /// TODO: use upper / lower bound to find
      optional<producer_schedule_type> new_producer_schedule;
      for( const auto& item : gpo.pending_active_producers ) {
         if( item.first < new_last_irreversible_block_num ) {
            new_producer_schedule = item.second;
         }
      }
      if( new_producer_schedule ) {
         _db.modify( gpo, [&]( auto& props ){
             /// TODO: use upper / lower bound to remove range
              while( gpo.pending_active_producers.size() ) {
                 if( gpo.pending_active_producers.front().first < new_last_irreversible_block_num ) {
                   props.pending_active_producers.erase(props.pending_active_producers.begin());
                 }
              }
              props.active_producers = *new_producer_schedule;
         });
      }
   }


   // Trim fork_database and undo histories
   _fork_db.set_max_size(head_block_num() - new_last_irreversible_block_num + 1);
   _db.commit(new_last_irreversible_block_num);
}

void chain_controller::clear_expired_transactions()
{ try {
   //Look for expired transactions in the deduplication list, and remove them.
   //transactions must have expired by at least two forking windows in order to be removed.
   /*
   auto& transaction_idx = _db.get_mutable_index<transaction_multi_index>();
   const auto& dedupe_index = transaction_idx.indices().get<by_expiration>();
   while( (!dedupe_index.empty()) && (head_block_time() > dedupe_index.rbegin()->expiration) )
      transaction_idx.remove(*dedupe_index.rbegin());

   //Look for expired transactions in the pending generated list, and remove them.
   //transactions must have expired by at least two forking windows in order to be removed.
   auto& generated_transaction_idx = _db.get_mutable_index<generated_transaction_multi_index>();
   const auto& generated_index = generated_transaction_idx.indices().get<generated_transaction_object::by_expiration>();
   while( (!generated_index.empty()) && (head_block_time() > generated_index.rbegin()->trx.expiration) )
      generated_transaction_idx.remove(*generated_index.rbegin());
      */
} FC_CAPTURE_AND_RETHROW() }

using boost::container::flat_set;

account_name chain_controller::get_scheduled_producer(uint32_t slot_num)const
{
   const dynamic_global_property_object& dpo = get_dynamic_global_properties();
   uint64_t current_aslot = dpo.current_absolute_slot + slot_num;
   const auto& gpo = _db.get<global_property_object>();
   //auto number_of_active_producers = gpo.active_producers.size();
   auto index = current_aslot % (config::blocks_per_round); //TODO configure number of repetitions by producer
   index /= config::producer_repititions;

   return gpo.active_producers.producers[index].producer_name;
}

block_timestamp_type chain_controller::get_slot_time(uint32_t slot_num)const
{
   if( slot_num == 0)
      return block_timestamp_type();

   const dynamic_global_property_object& dpo = get_dynamic_global_properties();

   if( head_block_num() == 0 )
   {
      // n.b. first block is at genesis_time plus one block interval
      auto genesis_time = block_timestamp_type(dpo.time);
      genesis_time.slot += slot_num;
      return (fc::time_point)genesis_time;
   }

   auto head_block_abs_slot = block_timestamp_type(head_block_time());
   head_block_abs_slot.slot += slot_num;
   return head_block_abs_slot;
}

uint32_t chain_controller::get_slot_at_time( block_timestamp_type when )const
{
   auto first_slot_time = get_slot_time(1);
   if( when < first_slot_time )
      return 0;
   return block_timestamp_type(when).slot - first_slot_time.slot + 1;
}

uint32_t chain_controller::producer_participation_rate()const
{
   const dynamic_global_property_object& dpo = get_dynamic_global_properties();
   return uint64_t(config::percent_100) * __builtin_popcountll(dpo.recent_slots_filled) / 64;
}

void chain_controller::_set_apply_handler( account_name contract, scope_name scope, action_name action, apply_handler v ) {
   _apply_handlers[contract][make_pair(scope,action)] = v;
}

transaction_trace chain_controller::_apply_transaction( transaction_metadata& meta ) { 
   transaction_trace result(meta.id);

   set<account_name> authorizing_accounts;


   for( const auto& act : meta.trx.actions ) {
      apply_context context( *this, _db, meta.trx, act );
      context.exec();
      fc::move_append(result.action_traces, std::move(context.results.applied_actions));
      fc::move_append(result.deferred_transactions, std::move(context.results.generated_transactions));
   }

   for( auto& at: result.action_traces ) {
      at.region_id = meta.region_id;
      at.cycle_index = meta.cycle_index;
   }


   for( const auto& act : meta.trx.actions )
      for( const auto& auth : act.authorization )
         authorizing_accounts.insert( auth.actor );

   auto trx_size = meta.bandwidth_usage + config::fixed_bandwidth_overhead_per_transaction;

   const auto& dgpo = get_dynamic_global_properties();

   auto head_time = head_block_time();
   for( const auto& authaccnt : authorizing_accounts ) {
      const auto& buo = _db.get<bandwidth_usage_object,by_owner>( authaccnt );
      _db.modify( buo, [&]( auto& bu ){
          bu.bytes.add_usage( trx_size, head_time );
      });
      const auto& sbo = _db.get<contracts::staked_balance_object, contracts::by_owner_name>(authaccnt);
#if 0 // TODO enable this after fixing divide by 0 with virtual_max_block_size and total_staked_tokens
      /// note: buo.bytes.value is in ubytes and virtual_max_block_size is in bytes, so
      //  we convert to fixed int uin128_t with 60 bits of precision, divide by rate limiting precision
      //  then divide by virtual max_block_size which gives us % of virtual max block size in fixed width
      auto used_percent  = (((uint128_t(buo.bytes.value) << 60) / config::rate_limiting_precision))  
                         / dgpo.virtual_max_block_size;

      /// percent of stake used in fixed width
      auto stake_percent = (uint128_t( sbo.staked_balance ) << 60)  
                         / dgpo.total_staked_tokens;

      FC_ASSERT( used_percent < stake_percent, "authorizing account has inusfficient stake for this transaction, try again later" );
#endif
   }

   return result;
}

const apply_handler* chain_controller::find_apply_handler( account_name receiver, account_name scope, action_name act ) const
{
   auto native_handler_scope = _apply_handlers.find( receiver );
   if( native_handler_scope != _apply_handlers.end() ) {
      auto handler = native_handler_scope->second.find( make_pair( scope, act ) );
      if( handler != native_handler_scope->second.end() ) 
         return &handler->second;
   }
   return nullptr;
}

} } /// eosio::chain
