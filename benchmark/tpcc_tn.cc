#include "TPCC_bench.hh"
#include "TPCC_txns.hh"

using namespace tpcc;

int tpcc_tn(int argc, char const* const* argv, db_split_type split) {
    switch (split) {
    case db_split_type::None:
        return tpcc_access<db_tictoc_node_params>::execute(argc, argv);
    case db_split_type::Static:
        return tpcc_access<db_tictoc_sts_node_params>::execute(argc, argv);
    case db_split_type::Adaptive:
        return tpcc_access<db_tictoc_ats_node_params>::execute(argc, argv);
    default:
        break;
    }
    assert(false);
    return 1;
}
