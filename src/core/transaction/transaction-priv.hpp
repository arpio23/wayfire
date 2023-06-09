#pragma once

#include "wayfire/signal-provider.hpp"
#include "wayfire/transaction/instruction.hpp"
#include <map>
#include <wayfire/transaction/transaction.hpp>
#include <wayfire/util.hpp>
#include <wayfire/option-wrapper.hpp>

namespace wf
{
namespace txn
{
class transaction_impl_t;
using transaction_iuptr_t = std::unique_ptr<transaction_impl_t>;

/**
 * Same as txn::done_signal, but on the transaction itself.
 */
struct priv_done_signal
{
    uint64_t id;
    transaction_state_t state;
};

/**
 * Emits done/cancel if an instruction does so.
 */
class transaction_impl_t : public transaction_t, public signal::provider_t
{
  public:
    transaction_impl_t();

    /**
     * Set all instructions as pending.
     * Transaction state moves from NEW to PENDING.
     */
    void set_pending();

    /**
     * Commit all instructions in the transaction.
     * Transaction state changes from PENDING to COMMITTED.
     */
    void commit();

    /**
     * Apply all instructions in the transaction.
     * Transaction state changes from COMMITTED to DONE.
     */
    void apply();

    /**
     * Move instructions from the other transaction to this,
     * thereby destroying the other transaction.
     *
     * NEW transactions can be merged into NEW or PENDING transactions.
     * PENDING transactions can be merged into PENDING transactions.
     */
    void merge(transaction_iuptr_t other);

    /**
     * Test whether instructions collide with each other (i.e have instructions
     * for the same objects).
     */
    bool does_intersect(const transaction_impl_t& other) const;

    void add_instruction(instruction_uptr_t instr) override;
    void add_instruction(instruction_uptr_t instr, bool already_pending);

    std::set<std::string> get_objects() const override;
    std::set<wayfire_view> get_views() const override;

    /**
     * Set the ID.
     */
    void set_id(uint64_t id)
    {
        this->id = id;
    }

    uint64_t get_id() const override
    {
        return this->id;
    }

    transaction_state_t get_state() const;

    /**
     * A transaction becomes dirty when new instructions are added to it,
     * until the dirty flag is cleared. Afterwards, new instructions can make
     * it dirty again.
     */
    bool is_dirty() const;

    void clear_dirty();

  private:
    uint64_t id;
    int32_t instructions_done = 0;
    bool dirty = false;

    transaction_state_t state = TXN_NEW;
    std::vector<instruction_uptr_t> instructions;

    wf::signal::connection_t<instruction_ready_signal> on_instruction_ready;
    wf::signal::connection_t<instruction_cancel_signal> on_instruction_cancel;

    wf::option_wrapper_t<int> timeout_ms{"core/transaction_timeout"};
    wf::wl_timer commit_timeout;
    void emit_done(transaction_state_t end_state);
};

// Get the transaction manager with transaction_manager_t::get(), and clean
// its state if needed.
// Used in tests.
transaction_manager_t& get_fresh_transaction_manager();
} // namespace txn
}
