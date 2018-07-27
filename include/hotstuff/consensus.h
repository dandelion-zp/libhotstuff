#ifndef _HOTSTUFF_CONSENSUS_H
#define _HOTSTUFF_CONSENSUS_H

#include <cassert>
#include <set>
#include <unordered_map>

#include "hotstuff/promise.hpp"
#include "hotstuff/type.h"
#include "hotstuff/entity.h"
#include "hotstuff/crypto.h"

namespace hotstuff {

struct Proposal;
struct Vote;
struct Finality;

/** Abstraction for HotStuff protocol state machine (without network implementation). */
class HotStuffCore {
    block_t b0;                                  /** the genesis block */
    /* === state variables === */
    /** block containing the QC for the highest block having one */
    block_t bqc;
    block_t bexec;                            /**< last executed block */
    uint32_t vheight;          /**< height of the block last voted for */
    /* === auxilliary variables === */
    privkey_bt priv_key;            /**< private key for signing votes */
    std::set<block_t, BlockHeightCmp> tails;   /**< set of tail blocks */
    ReplicaConfig config;                   /**< replica configuration */
    /* === async event queues === */
    std::unordered_map<block_t, promise_t> qc_waiting;
    promise_t propose_waiting;

    block_t get_delivered_blk(const uint256_t &blk_hash);
    void sanity_check_delivered(const block_t &blk);
    void check_commit(const block_t &_bqc);
    bool update(const uint256_t &bqc_hash);
    void on_qc_finish(const block_t &blk);
    void on_propose_(const block_t &blk);

    protected:
    ReplicaID id;                  /**< identity of the replica itself */

    public:
    BoxObj<EntityStorage> storage;

    HotStuffCore(ReplicaID id, privkey_bt &&priv_key);
    virtual ~HotStuffCore() {
        b0->qc_ref = nullptr;
    }

    /* Inputs of the state machine triggered by external events, should called
     * by the class user, with proper invariants. */

    /** Call to initialize the protocol, should be called once before all other
     * functions. */
    void on_init(uint32_t nfaulty) { config.nmajority = 2 * nfaulty + 1; }

    /** Call to deliver a block.
     * A block is only delivered if itself is fetched, the block for the
     * contained qc is fetched and all parents are delivered. The user should
     * always ensure this invariant. The invalid blocks will be dropped by this
     * function.
     * @return true if valid */
    bool on_deliver_blk(const block_t &blk);

    /** Call upon the delivery of a proposal message.
     * The block mentioned in the message should be already delivered. */
    void on_receive_proposal(const Proposal &prop);

    /** Call upon the delivery of a vote message.
     * The block mentioned in the message should be already delivered. */
    void on_receive_vote(const Vote &vote);

    /** Call to submit new commands to be decided (executed). "Parents" must
     * contain at least one block, and the first block is the actual parent,
     * while the others are uncles/aunts */
    void on_propose(const std::vector<command_t> &cmds,
                    const std::vector<block_t> &parents);

    /* Functions required to construct concrete instances for abstract classes.
     * */

    /* Outputs of the state machine triggering external events.  The virtual
     * functions should be implemented by the user to specify the behavior upon
     * the events. */
    protected:
    /** Called by HotStuffCore upon the decision being made for cmd. */
    virtual void do_decide(Finality &&fin) = 0;
    /** Called by HotStuffCore upon broadcasting a new proposal.
     * The user should send the proposal message to all replicas except for
     * itself. */
    virtual void do_broadcast_proposal(const Proposal &prop) = 0;
    /** Called upon sending out a new vote to the next proposer.  The user
     * should send the vote message to a *good* proposer to have good liveness,
     * while safety is always guaranteed by HotStuffCore. */
    virtual void do_vote(ReplicaID last_proposer, const Vote &vote) = 0;

    /* The user plugs in the detailed instances for those
     * polymorphic data types. */
    public:
    /** Create a partial certificate that proves the vote for a block. */
    virtual part_cert_bt create_part_cert(const PrivKey &priv_key, const uint256_t &blk_hash) = 0;
    /** Create a partial certificate from its seralized form. */
    virtual part_cert_bt parse_part_cert(DataStream &s) = 0;
    /** Create a quorum certificate that proves 2f+1 votes for a block. */
    virtual quorum_cert_bt create_quorum_cert(const uint256_t &blk_hash) = 0;
    /** Create a quorum certificate from its serialized form. */
    virtual quorum_cert_bt parse_quorum_cert(DataStream &s) = 0;
    /** Create a command object from its serialized form. */
    virtual command_t parse_cmd(DataStream &s) = 0;

    public:
    /** Add a replica to the current configuration. This should only be called
     * before running HotStuffCore protocol. */
    void add_replica(ReplicaID rid, const NetAddr &addr, pubkey_bt &&pub_key);
    /** Try to prune blocks lower than last committed height - staleness. */
    void prune(uint32_t staleness);

    /* PaceMaker can use these functions to monitor the core protocol state
     * transition */
    /** Get a promise resolved when the block gets a QC. */
    promise_t async_qc_finish(const block_t &blk);
    /** Get a promise resolved when a new block is proposed. */
    promise_t async_wait_propose();

    /* Other useful functions */
    block_t get_genesis() { return b0; }
    const ReplicaConfig &get_config() { return config; }
    ReplicaID get_id() const { return id; }
    const std::set<block_t, BlockHeightCmp> get_tails() const { return tails; }
    operator std::string () const;
};

/** Abstraction for proposal messages. */
struct Proposal: public Serializable {
    ReplicaID proposer;
    /** hash for the block containing the highest QC */
    uint256_t bqc_hash;
    /** block being proposed */
    block_t blk;

    /** handle of the core object to allow polymorphism. The user should use
     * a pointer to the object of the class derived from HotStuffCore */
    HotStuffCore *hsc;

    Proposal(): blk(nullptr), hsc(nullptr) {}
    Proposal(ReplicaID proposer,
            const uint256_t &bqc_hash,
            block_t &blk,
            HotStuffCore *hsc):
        proposer(proposer),
        bqc_hash(bqc_hash),
        blk(blk), hsc(hsc) {}

    void serialize(DataStream &s) const override {
        s << proposer
          << bqc_hash
          << *blk;
    }

    void unserialize(DataStream &s) override {
        assert(hsc != nullptr);
        s >> proposer
          >> bqc_hash;
        Block _blk;
        _blk.unserialize(s, hsc);
        blk = hsc->storage->add_blk(std::move(_blk), hsc->get_config());
    }

    operator std::string () const {
        DataStream s;
        s << "<proposal "
          << "rid=" << std::to_string(proposer) << " "
          << "bqc=" << get_hex10(bqc_hash) << " "
          << "blk=" << get_hex10(blk->get_hash()) << ">";
        return std::move(s);
    }
};

/** Abstraction for vote messages. */
struct Vote: public Serializable {
    ReplicaID voter;
    /** hash for the block containing the highest QC */
    uint256_t bqc_hash;
    /** block being voted */
    uint256_t blk_hash;
    /** proof of validity for the vote (nullptr for a negative vote) */
    part_cert_bt cert;
    
    /** handle of the core object to allow polymorphism */
    HotStuffCore *hsc;

    Vote(): cert(nullptr), hsc(nullptr) {}
    Vote(ReplicaID voter,
        const uint256_t &bqc_hash,
        const uint256_t &blk_hash,
        part_cert_bt &&cert,
        HotStuffCore *hsc):
        voter(voter),
        bqc_hash(bqc_hash),
        blk_hash(blk_hash),
        cert(std::move(cert)), hsc(hsc) {}

    Vote(const Vote &other):
        voter(other.voter),
        bqc_hash(other.bqc_hash),
        blk_hash(other.blk_hash),
        cert(other.cert->clone()),
        hsc(other.hsc) {}

    Vote(Vote &&other) = default;
    
    void serialize(DataStream &s) const override {
        s << voter
          << bqc_hash
          << blk_hash;
        if (cert == nullptr)
            s << (uint8_t)0;
        else
            s << (uint8_t)1 << *cert;
    }

    void unserialize(DataStream &s) override {
        assert(hsc != nullptr);
        uint8_t has_cert;
        s >> voter
          >> bqc_hash
          >> blk_hash
          >> has_cert;
        cert = has_cert ? hsc->parse_part_cert(s) : nullptr;
    }

    bool verify() const {
        assert(hsc != nullptr);
        return cert->verify(hsc->get_config().get_pubkey(voter)) &&
                cert->get_blk_hash() == blk_hash;
    }

    operator std::string () const {
        DataStream s;
        s << "<vote "
          << "rid=" << std::to_string(voter) << " "
          << "bqc=" << get_hex10(bqc_hash) << " "
          << "blk=" << get_hex10(blk_hash) << " "
          << "cert=" << (cert ? "yes" : "no") << ">";
        return std::move(s);
    }
};

struct Finality: public Serializable {
    ReplicaID rid;
    int8_t decision;
    uint32_t cmd_idx;
    uint32_t cmd_height;
    uint256_t cmd_hash;
    uint256_t blk_hash;
    
    public:
    Finality() = default;
    Finality(ReplicaID rid,
            int8_t decision,
            uint32_t cmd_idx,
            uint32_t cmd_height,
            uint256_t cmd_hash,
            uint256_t blk_hash):
        rid(rid), decision(decision),
        cmd_idx(cmd_idx), cmd_height(cmd_height),
        cmd_hash(cmd_hash), blk_hash(blk_hash) {}

    void serialize(DataStream &s) const override {
        s << rid << decision
          << cmd_idx << cmd_height
          << cmd_hash;
        if (decision == 1) s << blk_hash;
    }

    void unserialize(DataStream &s) override {
        s >> rid >> decision
          >> cmd_idx >> cmd_height
          >> cmd_hash;
        if (decision == 1) s >> blk_hash;
    }

    operator std::string () const {
        DataStream s;
        s << "<fin "
          << "decision=" << std::to_string(decision) << " "
          << "cmd_idx=" << std::to_string(cmd_idx) << " "
          << "cmd_height=" << std::to_string(cmd_height) << " "
          << "cmd=" << get_hex10(cmd_hash) << " "
          << "blk=" << get_hex10(blk_hash) << ">";
        return std::move(s);
    }
};

}

#endif
