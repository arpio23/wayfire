#pragma once

#include "wayfire/signal-provider.hpp"
#include <wayfire/transaction/instruction.hpp>
#include <wayfire/view.hpp>
#include <memory>
#include <set>

namespace wf
{
namespace txn
{
/**
 * Describes the state of a transaction at the end of its lifetime.
 */
enum transaction_state_t
{
    /**
     * A newly created transaction which has not been submitted and does not
     * have an ID yet.
     */
    TXN_NEW,
    /**
     * A transaction with a pending commit.
     */
    TXN_PENDING,
    /**
     * A committed transaction, waiting for readiness to be applied.
     */
    TXN_COMMITTED,
    /**
     * A transaction which is ready (and about) to be applied.
     */
    TXN_READY,
    /**
     * Transaction was cancelled because one of the participating objects
     * was destroyed.
     */
    TXN_CANCELLED,
    /**
     * Transaction has timed out because instruction took too long to commit.
     *
     * In this case, the transaction is ended and all instructions which can be
     * applied are applied.
     */
    TXN_TIMED_OUT,
    /**
     * Transaction was successfully applied.
     */
    TXN_APPLIED,
};

class transaction_t;
using transaction_uptr_t = std::unique_ptr<transaction_t>;

/**
 * on: transaction-manager, view(transaction-)
 * when: Pending is emitted when there are new pending instructions in a
 *   transaction. This happens when the transaction moves from NEW to PENDING.
 *   When this signal is emitted, plugins can add additional instructions. This
 *   will cause the pending signal to be emitted again for the same
 *   transaction. Plugins should be EXTREMELY careful with adding new
 *   instructions, because that may cause infinite loops if they keep adding
 *   new instructions on every pending signal.
 */
struct transaction_pending_signal
{
    /**
     * The transaction which this signal is about.
     */
    nonstd::observer_ptr<transaction_t> tx;
};

/**
 * on: transaction-manager, view(transaction-)
 * when: Whenever a transaction is about to be applied. This can happen when
 *   the transaction is READY or TIMED_OUT.
 */
struct transaction_ready_signal
{
    /**
     * The transaction which this signal is about.
     */
    nonstd::observer_ptr<transaction_t> tx;
};

/**
 * on: transaction-manager, view(transaction-)
 * when: Whenever a transaction has been applied or cancelled.
 */
struct transaction_done_signal
{
    /**
     * The transaction which this signal is about.
     */
    nonstd::observer_ptr<transaction_t> tx;
};

/**
 * transaction_t represents a collection of changes to views' states which
 * are applied atomically. The individual changes are called instructions.
 *
 * The transaction lifetime is as follows:
 *
 * - NEW: transaction has been created, but not submitted to core. In this
 *   state, transactions can live indefinitely and have no effect whatsoever.
 *   Generally, a transaction should be submitted as soon as possible, to avoid
 *   the case where it contains a dangling view (this case causes a crash).
 *
 * - PENDING: transaction has been submitted to core via
 *   wf::compositor_core_t::submit().  Instructions are not yet sent to clients.
 *
 * - COMMITTED: instructions have been sent to client, and Wayfire is waiting
 *   for a response.
 *
 * - READY: all instructions are ready to be applied.
 *
 * Transactions are moved from PENDING to COMMITTED automatically.
 * This is possible as soon as there are no COMMITTED transactions which affect
 * the same views.  Otherwise, all PENDING transactions are merged together into
 * a single PENDING transaction.
 *
 * Destruction of transactions:
 *
 * - A pending or committed transaction may fail at any time, for ex. if a view
 *   in it is closed by the client (state CANCELLED).
 * - A committed transaction may time out because clients are too slow to
 *   respond (state TIMED_OUT).
 * - A committed transaction may succeed if all clients update their surfaces on
 *   time (state APPLIED).
 *
 * In all of these three cases, 'done' signal is emitted from the transaction
 * manager.
 */
class transaction_t
{
  public:
    /**
     * Create a NEW empty transaction.
     *
     * Note: this is the only valid way to create transactions which can be
     * submitted to Wayfire's core for processing.
     */
    static transaction_uptr_t create();

    /**
     * Add new instructions for a given object.
     *
     * The object identifier is used for determining whether two transactions
     * can be committed in parallel (only if they touch separate objects).
     *
     * By default, views and outputs have unique IDs which are stringified
     * and used as their object identifier.
     *
     * @param object The object identifier.
     * @param instr The instruction for the object.
     */
    virtual void add_instruction(instruction_uptr_t instr) = 0;

    /**
     * Get a list of all objects which are influenced by this transaction.
     */
    virtual std::set<std::string> get_objects() const = 0;

    /**
     * Get a list of all views influenced by this transaction.
     */
    virtual std::set<wayfire_view> get_views() const = 0;

    /**
     * Get the ID of the transaction.
     * The ID is valid only after submitting the transaction to the
     * transaction_manager.
     */
    virtual uint64_t get_id() const = 0;

    transaction_t(const transaction_t& other) = delete;
    transaction_t(transaction_t&& other) = delete;
    transaction_t& operator =(const transaction_t& other) = delete;
    transaction_t& operator =(transaction_t&& other) = delete;

    virtual ~transaction_t() = default;

  protected:
    transaction_t() = default;
};

/**
 * A class which holds all active (pending/committed) transactions.
 * It is responsible for merging pending transactions, committing and finalizing
 * transactions.
 */
class transaction_manager_t : public signal::provider_t
{
  public:
    /**
     * Get the single global instance of the transaction manager.
     */
    static transaction_manager_t& get();

    /**
     * Submit a new transaction.
     *
     * In case the transaction manipulates only objects for which there are no
     * already pending or committed instructions, the transaction is committed
     * as soon as control returns to the main loop.
     *
     * If that is not true, all such transactions are merged together in a
     * single large transaction and committed as soon as the committed
     * instructions which block it are all done.
     *
     * Note that submitting an empty transaction is not allowed.
     *
     * @param tx The transaction to submit.
     * @return The assigned ID of the transaction. Note that this may be the ID
     *   of the large transaction that the current one was merged in.
     */
    uint64_t submit(transaction_uptr_t tx);

    // Implementation details
    class impl;
    std::unique_ptr<impl> priv;

  private:
    transaction_manager_t();
    ~transaction_manager_t();

    transaction_manager_t(const transaction_manager_t& other) = delete;
    transaction_manager_t(transaction_manager_t&& other) = delete;
    transaction_manager_t& operator =(const transaction_manager_t& other) = delete;
    transaction_manager_t& operator =(transaction_manager_t&& other) = delete;
};
}
}
