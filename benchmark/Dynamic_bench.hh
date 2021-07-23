#pragma once

#include <iostream>
#include <iomanip>
#include <random>
#include <set>
#include <string>
#include <thread>

#include "barrier.hh" // pthread_barrier_t

#include "compiler.hh"
#include "clp.h"
#include "SystemProfiler.hh"
#include "DB_structs.hh"
#include "Dynamic_structs.hh"
//#include "Dynamic_commutators.hh"

#if TABLE_FINE_GRAINED
#include "Dynamic_selectors.hh"
#endif

#include "DB_index.hh"
#include "DB_params.hh"
#include "DB_profiler.hh"
#include "PlatformFeatures.hh"

#if TABLE_FINE_GRAINED
//#include "dynamic_split_params_ts.hh"
#else
//#include "dynamic_split_params_default.hh"
#endif

// @section: clp parser definitions
enum {
    opt_dbid = 1, opt_thld, opt_nthrs, opt_time, opt_perf, opt_pfcnt, opt_gc,
    opt_gr, opt_node, opt_comm, opt_verb, opt_mix, opt_ordr, opt_ada, opt_samp
};

extern const char* workload_mix_names[];
extern const Clp_Option options[];
extern const size_t noptions;
extern void print_usage(const char *);
// @endsection: clp parser definitions

namespace dynamic {

using namespace db_params;
using bench::db_profiler;

class dynamic_input_generator {
public:
    static const char * last_names[];

    dynamic_input_generator(int id) : gen(id) {}
    explicit dynamic_input_generator() : gen(0) {}

    uint64_t nurand(uint64_t a, uint64_t c, uint64_t x, uint64_t y) {
        uint64_t r1 = (random(0, a) | random(x, y)) + c;
        return (r1 % (y - x + 1)) + x;
    }
    uint64_t random(uint64_t x, uint64_t y) {
        std::uniform_int_distribution<uint64_t> dist(x, y);
        return dist(gen);
    }
    template <typename S, typename T>
    std::vector<uint64_t> nrandom(S x, T y, size_t count) {
        std::set<uint64_t> s;
        while (s.size() < count) {
            s.insert(random(static_cast<uint64_t>(x), static_cast<uint64_t>(y)));
        }
        return {s.begin(), s.end()};
    }

    uint64_t gen_index(uint64_t length) {
        return random(0, length);
    }
    uint64_t gen_value() {
        return random(1, 6174961351ULL);
    }
    std::mt19937& random_generator() {
        return gen;
    }

private:
    std::mt19937 gen;
};

template <typename DBParams>
class dynamic_access;

template <typename DBParams>
class dynamic_db {
public:
    template <typename K, typename V>
    using OIndex = typename std::conditional<DBParams::MVCC,
          mvcc_ordered_index<K, V, DBParams>,
          ordered_index<K, V, DBParams>>::type;

    template <typename K, typename V>
    using UIndex = typename std::conditional<DBParams::MVCC,
          mvcc_unordered_index<K, V, DBParams>,
          unordered_index<K, V, DBParams>>::type;

    typedef OIndex<ordered_key, ordered_value>           o_table_type;
    typedef UIndex<unordered_key, unordered_value>       u_table_type;

    static const size_t TABLE_SIZE = 1000000;

    explicit inline dynamic_db(uint64_t keymin, uint64_t keymax);
    explicit inline dynamic_db(const std::string& db_file_name) = delete;
    inline ~dynamic_db();
    void thread_init_all();

    uint64_t keymin() const {
        return keymin_;
    }
    uint64_t keymax() const {
        return keymax_;
    }

    o_table_type& tbl_ordered() {
        return tbl_o_;
    }
    u_table_type& tbl_unordered() {
        return tbl_u_;
    }

private:
    o_table_type tbl_o_;
    u_table_type tbl_u_;
    uint64_t keymin_;
    uint64_t keymax_;

    friend class dynamic_access<DBParams>;
};

template <typename DBParams>
class dynamic_runner {
public:
    static constexpr bool Commute = DBParams::Commute;
    enum class txn_type : int {
        read = 1,
        write,
        read_medium,
        write_medium,
        read_heavy,
        write_heavy,
        per_record,
        per_record_read,
        per_record_write
    };

    struct Params {
        int mix;
        int threshold;
        bool ordered;
        bool profile;
        bool sample;
    };

    using o_table_access = typename dynamic_db<DBParams>::o_table_type::ColumnAccess;
    using u_table_access = typename dynamic_db<DBParams>::u_table_type::ColumnAccess;

    dynamic_runner(int id, dynamic_db<DBParams>& database, Params& params)
        : ig(id), count(0), db(database), mix(params.mix), params(params),
          threshold(params.threshold),
          tsc_threshold(params.threshold * constants::processor_tsc_frequency * constants::million),
          runner_id(id), prev_workload(0), switches(0) {}

    inline txn_type next_transaction(uint64_t tsc_elapsed) {
        ++count;
        uint64_t x = ig.random(0, 99);
        if (mix == 0) {
            int workload_variant = (tsc_elapsed / tsc_threshold) % 4;
            //int workload_variant = (count / 10000) % 2;
            /*
            bool change_workload = ig.random(0, tsc_threshold - 1) < (tsc_elapsed % tsc_threshold);
            int workload_variant = change_workload? 1 - prev_workload : prev_workload;
            */
            txn_type txn;
            int next_workload;
            switch (workload_variant) {
                case 0:
                    next_workload = 0;
                    txn = x < 90 ? txn_type::read_heavy : txn_type::write;
                    break;
                case 1:
                case 3:
                    next_workload = 1;
                    txn = x < 50 ? txn_type::read_medium : txn_type::write_medium;
                    break;
                case 2:
                    next_workload = 2;
                    txn = x < 10 ? txn_type::read : txn_type::write_heavy;
                    break;
                default:
                    std::cerr << "Invalid workload variant: " << workload_variant << std::endl;
                    assert(false);
            }
            switches += next_workload != prev_workload;
            if (next_workload != prev_workload) {
                //std::cout << "Workload switched from " << prev_workload << " to " << workload << std::endl;
            }
            prev_workload = next_workload;
            return txn;
        } else if (mix == 1) {
            return txn_type::per_record;
        } else if (mix == 2) {
            return ig.random(0, 1) ? txn_type::per_record_read : txn_type::per_record_write;
        }
        assert(false);
        return txn_type::read;
    }

    inline void adapters_commit() {
        if (!ig.random(0, 3)) {
            if (params.ordered) {
                ADAPTER_OF(ordered_value)::Commit();
            } else {
                ADAPTER_OF(unordered_value)::Commit();
            }
            if (!params.profile && !ig.random(0, 99)) {
                if (params.ordered) {
                    if (ADAPTER_OF(ordered_value)::RecomputeSplit()) {
                        std::cout << "Split changed to " << (int)ADAPTER_OF(ordered_value)::CurrentSplit() << std::endl;
                    }
                } else {
                    if (ADAPTER_OF(unordered_value)::RecomputeSplit()) {
                        std::cout << "Split changed to " << (int)ADAPTER_OF(unordered_value)::CurrentSplit() << std::endl;
                    }
                }
            }
        }
    }

    inline void adapters_treset() {
        if (params.ordered) {
            ADAPTER_OF(ordered_value)::ResetThread();
        } else {
            ADAPTER_OF(unordered_value)::ResetThread();
        }
    }

    inline void run_txn_ordered_per_record(uint64_t, bool=true, bool=true);
    inline void run_txn_ordered_read(uint64_t);
    inline void run_txn_ordered_write(uint64_t);
    inline void run_txn_unordered_per_record(uint64_t, bool=true, bool=true);
    inline void run_txn_unordered_read(uint64_t);
    inline void run_txn_unordered_write(uint64_t);

private:
    dynamic_input_generator ig;
    int64_t count;
    dynamic_db<DBParams>& db;
    int mix;
    const Params params;
    int threshold;
    uint64_t tsc_threshold;
    int runner_id;
    int prev_workload;
    int switches;

    const uint64_t tsc_sec = constants::processor_tsc_frequency * constants::million;

    friend class dynamic_access<DBParams>;
};

template <typename DBParams>
class dynamic_prepopulator {
public:
    static pthread_barrier_t sync_barrier;

    dynamic_prepopulator(int id, dynamic_db<DBParams>& database, uint64_t keymin, uint64_t keymax)
        : ig(id), db(database), keymin(keymin), keymax(keymax), worker_id(id) {}

    inline void prepopulate_ordered();
    inline void prepopulate_unordered();

    inline void run();

private:
    dynamic_input_generator ig;
    dynamic_db<DBParams>& db;
    uint64_t keymin;
    uint64_t keymax;
    int worker_id;
};

template <typename DBParams>
pthread_barrier_t dynamic_prepopulator<DBParams>::sync_barrier;

template <typename DBParams>
dynamic_db<DBParams>::dynamic_db(uint64_t keymin, uint64_t keymax)
        : tbl_o_(TABLE_SIZE), tbl_u_(TABLE_SIZE), keymin_(keymin), keymax_(keymax) {
    assert(keymin_ < keymax_);
    assert(keymax_ < TABLE_SIZE);
}

template <typename DBParams>
dynamic_db<DBParams>::~dynamic_db() {}

template <typename DBParams>
void dynamic_db<DBParams>::thread_init_all() {
    tbl_o_.thread_init();
    tbl_u_.thread_init();
}

// @section: db prepopulation functions
template<typename DBParams>
void dynamic_prepopulator<DBParams>::prepopulate_ordered() {
    for (uint64_t key = keymin; key <= keymax; ++key) {
        ordered_key ok(key);
        ordered_value ov;

        for (int index = 0; ordered_value::NamedColumn::ro + index < ordered_value::NamedColumn::rw; ++index) {
            ov.ro(index) = ig.random(1900, 1999);
        }
        for (int index = 0; ordered_value::NamedColumn::rw + index < ordered_value::NamedColumn::wo; ++index) {
            ov.rw(index) = ig.random(1900, 1999);
        }
        for (int index = 0; ordered_value::NamedColumn::wo + index < ordered_value::NamedColumn::COLCOUNT; ++index) {
            ov.wo(index) = ig.random(1900, 1999);
        }

        db.tbl_ordered().nontrans_put(ok, ov);
    }
}

template<typename DBParams>
void dynamic_prepopulator<DBParams>::prepopulate_unordered() {
    for (uint64_t key = keymin; key <= keymax; ++key) {
        unordered_key uk(key);
        unordered_value uv;

        for (int index = 0; unordered_value::NamedColumn::ro + index < unordered_value::NamedColumn::rw; ++index) {
            uv.ro(index) = ig.random(1900, 1999);
        }
        for (int index = 0; unordered_value::NamedColumn::rw + index < unordered_value::NamedColumn::wo; ++index) {
            uv.rw(index) = ig.random(1900, 1999);
        }
        for (int index = 0; unordered_value::NamedColumn::wo + index < unordered_value::NamedColumn::COLCOUNT; ++index) {
            uv.wo(index) = ig.random(1900, 1999);
        }

        db.tbl_unordered().nontrans_put(uk, uv);
    }
}
// @endsection: db prepopulation functions

template<typename DBParams>
void dynamic_prepopulator<DBParams>::run() {
    int r;

    always_assert(worker_id >= 1, "prepopulator worker id range error");
    // set affinity so that the warehouse is filled at the corresponding numa node
    set_affinity(worker_id - 1);

    prepopulate_ordered();
    prepopulate_unordered();

    // barrier
    r = pthread_barrier_wait(&sync_barrier);
    always_assert(r == PTHREAD_BARRIER_SERIAL_THREAD || r == 0, "pthread_barrier_wait");
}

template <typename DBParams>
class dynamic_access {
public:
    static void prepopulation_worker(dynamic_db<DBParams> &db, int worker_id, uint64_t keymin, uint64_t keymax) {
        dynamic_prepopulator<DBParams> pop(worker_id, db, keymin, keymax);
        db.thread_init_all();
        pop.run();
    }

    static void prepopulate_db(dynamic_db<DBParams> &db, int nworkers) {
        int r;
        r = pthread_barrier_init(&dynamic_prepopulator<DBParams>::sync_barrier, nullptr, nworkers);
        always_assert(r == 0, "pthread_barrier_init failed");

        auto range = db.keymax() - db.keymin();
        auto step = (range - 1) / nworkers + 1;
        std::vector<std::thread> prepop_thrs;
        for (int i = 1; i <= nworkers; ++i) {
            auto kmin = db.keymin() + (i - 1) * step;
            auto kmax = db.keymin() + i * step;
            prepop_thrs.emplace_back(prepopulation_worker, std::ref(db), i, kmin, kmax);
        }
        for (auto &t : prepop_thrs)
            t.join();

        r = pthread_barrier_destroy(&dynamic_prepopulator<DBParams>::sync_barrier);
        always_assert(r == 0, "pthread_barrier_destroy failed");
    }

    static void dynamic_runner_thread(
            dynamic_db<DBParams>& db, db_profiler& prof, int runner_id, double time_limit,
            typename dynamic_runner<DBParams>::Params& params, uint64_t& txn_cnt) {
        dynamic_runner<DBParams> runner(runner_id, db, params);
        typedef typename dynamic_runner<DBParams>::txn_type txn_type;

        uint64_t local_cnt = 0;

        ::TThread::set_id(runner_id);
        set_affinity(runner_id);
        db.thread_init_all();

        uint64_t tsc_diff = (uint64_t)(time_limit * constants::processor_tsc_frequency * constants::billion);
        auto start_t = prof.start_timestamp();

        while (true) {
            auto curr_t = read_tsc();
            if ((curr_t - start_t) >= tsc_diff)
                break;

            txn_type t = runner.next_transaction(curr_t - start_t);
            switch (t) {
                case txn_type::read:
                    if (params.ordered) {
                        runner.run_txn_ordered_read(0);
                    } else {
                        runner.run_txn_unordered_read(0);
                    }
                    break;
                case txn_type::write:
                    if (params.ordered) {
                        runner.run_txn_ordered_write(0);
                    } else {
                        runner.run_txn_unordered_write(0);
                    }
                    break;
                case txn_type::read_medium:
                    if (params.ordered) {
                        runner.run_txn_ordered_read(1);
                    } else {
                        runner.run_txn_unordered_read(1);
                    }
                    break;
                case txn_type::write_medium:
                    if (params.ordered) {
                        runner.run_txn_ordered_write(1);
                    } else {
                        runner.run_txn_unordered_write(1);
                    }
                    break;
                case txn_type::read_heavy:
                    if (params.ordered) {
                        runner.run_txn_ordered_read(2);
                    } else {
                        runner.run_txn_unordered_read(2);
                    }
                    break;
                case txn_type::write_heavy:
                    if (params.ordered) {
                        runner.run_txn_ordered_write(2);
                    } else {
                        runner.run_txn_unordered_write(2);
                    }
                    break;
                case txn_type::per_record:
                    if (params.ordered) {
                        runner.run_txn_ordered_per_record(100);
                    } else {
                        runner.run_txn_unordered_per_record(100);
                    }
                    break;
                case txn_type::per_record_read:
                    if (params.ordered) {
                        runner.run_txn_ordered_per_record(100, true, false);
                    } else {
                        runner.run_txn_unordered_per_record(100, true, false);
                    }
                    break;
                case txn_type::per_record_write:
                    if (params.ordered) {
                        runner.run_txn_ordered_per_record(100, false, true);
                    } else {
                        runner.run_txn_unordered_per_record(100, false, true);
                    }
                    break;
                default:
                    fprintf(stderr, "r:%d unknown txn type\n", runner_id);
                    assert(false);
                    break;
            };

            ++local_cnt;
        }

        if (runner.switches) {
            printf("Switches: %d\n", runner.switches);
        }

        txn_cnt = local_cnt;
    }

    static uint64_t run_benchmark(dynamic_db<DBParams>& db, db_profiler& prof, int num_runners,
                                  double time_limit,
                                  typename dynamic_runner<DBParams>::Params& params,
                                  const bool verbose) {
        (void) verbose;

        std::vector<std::thread> runner_thrs;
        std::vector<uint64_t> txn_cnts(size_t(num_runners), 0);

        for (int i = 0; i < num_runners; ++i) {
            runner_thrs.emplace_back(
                    dynamic_runner_thread, std::ref(db), std::ref(prof),
                    i, time_limit, std::ref(params), std::ref(txn_cnts[i]));
        }

        for (auto &t : runner_thrs)
            t.join();

        uint64_t total_txn_cnt = 0;
        for (auto& cnt : txn_cnts)
            total_txn_cnt += cnt;
        return total_txn_cnt;
    }

    static int execute(int argc, const char *const *argv) {
        std::cout << "*** DBParams::Id = " << DBParams::Id << std::endl;
        std::cout << "*** DBParams::Commute = " << std::boolalpha << DBParams::Commute << std::endl;
        int ret = 0;

        bool spawn_perf = false;
        bool counter_mode = false;
        int num_threads = 1;
        double time_limit = 10.0;
        bool enable_gc = false;
        unsigned gc_rate = Transaction::get_epoch_cycle();
        bool verbose = false;

        db_params_adapter adapter_type = db_params_adapter::None;

        typename dynamic_runner<DBParams>::Params params {
            0 /*mix*/, 1000 /*threshold*/, false /*ordered*/, false /*profile*/,
            false /*sample*/
        };

        Clp_Parser *clp = Clp_NewParser(argc, argv, noptions, options);

        int opt;
        bool clp_stop = false;
        while (!clp_stop && ((opt = Clp_Next(clp)) != Clp_Done)) {
            switch (opt) {
                case opt_dbid:
                    break;
                case opt_thld:
                    params.threshold = clp->val.i;
                    break;
                case opt_nthrs:
                    num_threads = clp->val.i;
                    break;
                case opt_time:
                    time_limit = clp->val.d;
                    break;
                case opt_perf:
                    spawn_perf = !clp->negated;
                    break;
                case opt_pfcnt:
                    counter_mode = !clp->negated;
                    break;
                case opt_gc:
                    enable_gc = !clp->negated;
                    break;
                case opt_gr:
                    gc_rate = clp->val.i;
                    break;
                case opt_node:
                    break;
                case opt_comm:
                    break;
                case opt_ada:
                    adapter_type = parse_adapter(clp->val.s);
                    break;
                case opt_ordr:
                    params.ordered = !clp->negated;
                    break;
                case opt_samp:
                    params.sample = !clp->negated;
                    break;
                case opt_verb:
                    verbose = !clp->negated;
                    break;
                case opt_mix:
                    params.mix = clp->val.i;
                    if (params.mix > 2 || params.mix < 0) {
                        params.mix = 0;
                    }
                    break;
                default:
                    ::print_usage(argv[0]);
                    ret = 1;
                    clp_stop = true;
                    break;
            }
        }

        Clp_DeleteParser(clp);
        if (ret != 0)
            return ret;

        std::cout << "Selected workload mix: " << std::string(workload_mix_names[params.mix]) << std::endl;

        switch (adapter_type) {
            case db_params_adapter::Profile:
                params.profile = true;
                break;
            default:
                break;
        }


        auto profiler_mode = counter_mode ?
                             Profiler::perf_mode::counters : Profiler::perf_mode::record;

        if (counter_mode && !spawn_perf) {
            // turns on profiling automatically if perf counters are requested
            spawn_perf = true;
        }

        if (spawn_perf) {
            std::cout << "Info: Spawning perf profiler in "
                      << (counter_mode ? "counter" : "record") << " mode" << std::endl;
        }

        ADAPTER_OF(ordered_value)::ResetGlobal();
        ADAPTER_OF(unordered_value)::ResetGlobal();

        db_profiler prof(spawn_perf);
        dynamic_db<DBParams> db(16, 1ULL << 16);

        std::cout << "Prepopulating database..." << std::endl;
        prepopulate_db(db, num_threads);
        std::cout << "Prepopulation complete." << std::endl;

        std::thread advancer;
        std::cout << "Garbage collection: ";
        if (enable_gc) {
            std::cout << "enabled, running every " << gc_rate / 1000.0 << " ms";
            Transaction::set_epoch_cycle(gc_rate);
            advancer = std::thread(&Transaction::epoch_advancer, nullptr);
        } else {
            std::cout << "disabled";
        }
        std::cout << std::endl << std::flush;

        prof.start(profiler_mode);
        auto num_trans = run_benchmark(db, prof, num_threads, time_limit, params, verbose);
        prof.finish(num_trans);

        Transaction::rcu_release_all(advancer, num_threads);

        if (adapter_type == db_params_adapter::Global ||
                adapter_type == db_params_adapter::Profile) {
            if (params.ordered) {
                ADAPTER_OF(ordered_value)::PrintStats();
            } else {
                ADAPTER_OF(unordered_value)::PrintStats();
            }
        }

        if (params.mix == 1 || params.mix == 2) {
            size_t suboptimal = 0;
            for (auto key = db.keymin(); key <= db.keymax(); ++key) {
                if (params.ordered) {
                    auto value = db.tbl_ordered().nontrans_get(key);
                    auto range_boundary = static_cast<ordered_value::NamedColumn>(
                            key % static_cast<uint64_t>(ordered_value::NamedColumn::COLCOUNT));
                    if (value->splitindex_ != range_boundary) {
                        ++suboptimal;
                        /*
                        printf("Suboptimal split (%ld) with optimum %ld\n",
                               value->splitindex_, range_boundary);
                        */
                    }
                }

                if (key == db.keymax()) {
                    break;
                }
            }
            size_t ncolumns = params.ordered ?
                static_cast<size_t>(ordered_value::NamedColumn::COLCOUNT) :
                static_cast<size_t>(unordered_value::NamedColumn::COLCOUNT);
            printf("Suboptimal splits: %zu/%zu (%.6f%%, ~%zu/%zu)\n",
                   suboptimal, db.keymax() - db.keymin() + 1,
                   suboptimal * 100.0 / (db.keymax() - db.keymin() + 1),
                   static_cast<size_t>(
                       std::round(suboptimal * ncolumns * 1.0 / (db.keymax() - db.keymin() + 1))),
                   ncolumns);
        }

        return 0;
    }
}; // class dynamic_access

};
