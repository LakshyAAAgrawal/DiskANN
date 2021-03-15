// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#include <cstring>
#include <iomanip>
#include <omp.h>
#include <set>
#include <string.h>

#ifndef _WINDOWS
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

#include "aux_utils.h"
#include "index.h"
#include "memory_mapper.h"
#include "utils.h"

template<typename T>
int search_pq_memory_index(int argc, char** argv) {
  T*                query = nullptr;
  unsigned*         gt_ids = nullptr;
  float*            gt_dists = nullptr;
  size_t            query_num, query_dim, query_aligned_dim, gt_num, gt_dim;
  std::vector<_u64> Lvec;

  std::string       data_file(argv[2]);
  const std::string pq_path(argv[3]);
  std::string       memory_index_file(argv[4]);
  _u64              num_threads = std::atoi(argv[5]);
  std::string       query_bin(argv[6]);
  std::string       truthset_bin(argv[7]);
  _u64              recall_at = std::atoi(argv[8]);
  std::string       result_output_prefix(argv[9]);
  bool              use_optimized_search = std::atoi(argv[10]);

  if ((std::string(argv[1]) != std::string("float")) &&
      (use_optimized_search == true)) {
    std::cout << "Error. Optimized search currently only supported for "
                 "floating point datatypes. Using un-optimized search."
              << std::endl;
    use_optimized_search = false;
  }

  bool calc_recall_flag = false;

  for (int ctr = 10; ctr < argc; ctr++) {
    _u64 curL = std::atoi(argv[ctr]);
    if (curL >= recall_at)
      Lvec.push_back(curL);
  }

  if (Lvec.size() == 0) {
    std::cout << "No valid Lsearch found. Lsearch must be at least recall_at."
              << std::endl;
    return -1;
  }

  diskann::load_aligned_bin<T>(query_bin, query, query_num, query_dim,
                               query_aligned_dim);

  if (file_exists(truthset_bin)) {
    diskann::load_truthset(truthset_bin, gt_ids, gt_dists, gt_num, gt_dim);
    if (gt_num != query_num) {
      std::cout << "Error. Mismatch in number of queries and ground truth data"
                << std::endl;
    }
    calc_recall_flag = true;
  }

  std::cout.setf(std::ios_base::fixed, std::ios_base::floatfield);
  std::cout.precision(2);

  auto metric = diskann::L2;
  if (use_optimized_search)
    metric = diskann::FAST_L2;
  diskann::Index<T> index(metric, data_file.c_str());
  index.load(memory_index_file.c_str());  // to load NSG
  std::cout << "Index loaded" << std::endl;
  index.pq_load(pq_path.c_str());  // to load NSG
  std::cout << "PQ data loaded" << std::endl;

  if (use_optimized_search)
    index.optimize_graph();

  std::string recall_string = "Recall@" + std::to_string(recall_at);
  std::cout << std::setw(4) << "Ls" << std::setw(12) << "QPS " << std::setw(18)
            << "Mean Latency (mus)" << std::setw(15) << "99.9 Latency"
            << std::setw(12) << recall_string << std::endl;
  std::cout << "==============================================================="
               "==============="
            << std::endl;

  std::vector<std::vector<uint32_t>> query_result_ids(Lvec.size());
  std::vector<double>                latency_stats(query_num, 0);

  for (uint32_t test_id = 0; test_id < Lvec.size(); test_id++) {
    _u64 L = Lvec[test_id];
    query_result_ids[test_id].resize(recall_at * query_num);
    omp_set_num_threads(num_threads);

    auto s = std::chrono::high_resolution_clock::now();
    //#pragma omp parallel for schedule(dynamic, 1)
    for (int64_t i = 0; i < (int64_t) query_num; i++) {
      auto qs = std::chrono::high_resolution_clock::now();
      index.pq_search(query + i * query_aligned_dim, recall_at, L,
                      query_result_ids[test_id].data() + i * recall_at);
      auto qe = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> diff = qe - qs;
      latency_stats[i] = diff.count() * 1000000;
    }
    auto                          e = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = e - s;

    float qps = (query_num / diff.count());

    float recall = 0;
    if (calc_recall_flag) {
      recall = diskann::calculate_recall(query_num, gt_ids, gt_dists, gt_dim,
                                         query_result_ids[test_id].data(),
                                         recall_at, recall_at);
    }

    std::sort(latency_stats.begin(), latency_stats.end());
    double mean_latency = 0;
    for (uint64_t q = 0; q < query_num; q++) {
      mean_latency += latency_stats[q];
    }
    mean_latency /= query_num;

    std::cout << std::setw(4) << L << std::setw(12) << qps << std::setw(18)
              << (float) mean_latency << std::setw(15)
              << (float) latency_stats[(_u64)(0.999 * query_num)]
              << std::setw(12) << recall << std::endl;
  }

  std::cout << "Done searching. Now saving results " << std::endl;
  _u64 test_id = 0;
  for (auto L : Lvec) {
    std::string cur_result_path =
        result_output_prefix + "_" + std::to_string(L) + "_idx_uint32.bin";
    diskann::save_bin<_u32>(cur_result_path, query_result_ids[test_id].data(),
                            query_num, recall_at);
    test_id++;
  }

  diskann::aligned_free(query);
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 12) {
    std::cout
        << "Usage: " << argv[0]
        << "  [index_type<float/int8/uint8>]  [data_file.bin]  "
           "[pq_path_prefix]  [memory_index_path]  [num_threads]  "
           "[query_file.bin]  [truthset.bin (use \"null\" for none)] "
           " [K]  [result_output_prefix]  [use_optimized_search (for small ~1M "
           "data)] "
           " [L1]  [L2] etc. See README for more information on parameters. "
        << std::endl;
    exit(-1);
  }
  if (std::string(argv[1]) == std::string("int8"))
    search_pq_memory_index<int8_t>(argc, argv);
  else if (std::string(argv[1]) == std::string("uint8"))
    search_pq_memory_index<uint8_t>(argc, argv);
  else if (std::string(argv[1]) == std::string("float"))
    search_pq_memory_index<float>(argc, argv);
  else
    std::cout << "Unsupported type. Use float/int8/uint8" << std::endl;
}
