#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <ios>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <span>
#include <sstream>
#include <thread>
#include <vector>

#include "cppcoro/sync_wait.hpp"
#include "cppcoro/task.hpp"
#include "cppcoro/when_all_ready.hpp"
#include "storage/file.h"
#include "storage/io_uring.h"
#include "storage/schema.h"
#include "storage/swip.h"
#include "storage/types.h"

namespace {
using namespace storage;

bool do_work = true;
uint64_t num_tuples_per_morsel = 1'000;

class Cache {
 public:
  Cache(std::span<Swip> swips, const File &data_file)
      : swips_(swips), data_file_(data_file) {
    frames_.reserve(swips.size());
  }

  void Populate(std::span<const uint64_t> swip_indexes) {
    constexpr uint64_t kNumConcurrentTasks = 64ull;
    IOUring ring(kNumConcurrentTasks);
    Countdown countdown(kNumConcurrentTasks);

    std::vector<cppcoro::task<void>> tasks;
    tasks.reserve(kNumConcurrentTasks + 1);

    uint64_t partition_size =
        (swip_indexes.size() + kNumConcurrentTasks - 1) / kNumConcurrentTasks;

    for (uint64_t i = 0; i != kNumConcurrentTasks; ++i) {
      uint64_t begin = std::min(i * partition_size, swip_indexes.size());
      auto end = std::min(begin + partition_size, swip_indexes.size());
      tasks.emplace_back(
          AsyncLoadPages(ring, begin, end, countdown, swip_indexes));
    }
    tasks.emplace_back(DrainRing(ring, countdown));
    cppcoro::sync_wait(cppcoro::when_all_ready(std::move(tasks)));
  }

 private:
  cppcoro::task<void> AsyncLoadPages(IOUring &ring, uint64_t begin,
                                     uint64_t end, Countdown &countdown,
                                     std::span<const uint64_t> swip_indexes) {
    for (uint64_t i = begin; i != end; ++i) {
      frames_.emplace_back();
      LineitemPageQ1 &page = frames_.back();
      co_await data_file_.AsyncReadPage(ring,
                                        swips_[swip_indexes[i]].GetPageIndex(),
                                        reinterpret_cast<std::byte *>(&page));
      swips_[swip_indexes[i]].SetPointer(&page);
    }
    countdown.Decrement();
  }

  std::span<Swip> swips_;
  const File &data_file_;
  std::vector<LineitemPageQ1> frames_;
};

struct HashTableEntry {
  Numeric<12, 2> sum_qty;
  Numeric<12, 2> sum_base_price;
  Numeric<12, 2> sum_disc;
  Numeric<12, 4> sum_disc_price;
  Numeric<12, 4> sum_charge;
  uint32_t count;
  Char l_returnflag;
  Char l_linestatus;
};

using HashTable = std::vector<std::unique_ptr<HashTableEntry>>;
using ValidHashTableIndexes = std::vector<uint32_t>;

// implementation idea for query 1 stolen from the MonetDB/X100 paper
class QueryRunner {
 public:
  QueryRunner(uint32_t num_threads, std::span<const Swip> swips,
              const File &data_file, uint32_t num_ring_entries = 0)
      : thread_local_hash_tables_(num_threads),
        thread_local_valid_hash_table_indexes_(num_threads),
        high_date_(Date::FromString("1998-09-02|", '|').value),
        num_threads_(num_threads),
        swips_(swips),
        data_file_(data_file),
        num_ring_entries_(num_ring_entries) {
    for (auto &hash_table : thread_local_hash_tables_) {
      hash_table.resize(1ull << 16);
    }

    if (num_ring_entries > 0) {
      thread_local_rings_.reserve(num_threads);
      for (uint32_t i = 0; i != num_threads; ++i) {
        thread_local_rings_.emplace_back(num_ring_entries);
      }
    }
  }

  static void ProcessTuples(const LineitemPageQ1 &page, HashTable &hash_table,
                            ValidHashTableIndexes &valid_hash_table_indexes,
                            Date high_date) {
    Numeric<12, 2> one{int64_t{100}};  // assigns a raw value
    for (uint32_t i = 0; i != page.num_tuples; ++i) {
      if (page.l_shipdate[i] <= high_date) {
        uint32_t hash_table_index = page.l_returnflag[i];
        hash_table_index = (hash_table_index << 8) + page.l_linestatus[i];
        auto &entry = hash_table[hash_table_index];
        if (!entry) {
          entry = std::make_unique<HashTableEntry>();
          entry->l_returnflag = page.l_returnflag[i];
          entry->l_linestatus = page.l_linestatus[i];
          entry->count = 0;
          valid_hash_table_indexes.push_back(hash_table_index);
        }

        ++entry->count;
        entry->sum_qty += page.l_quantity[i];
        entry->sum_base_price += page.l_extendedprice[i];
        entry->sum_disc += page.l_discount[i];
        Numeric<12, 4> common_term =
            page.l_extendedprice[i] * (one - page.l_discount[i]);
        entry->sum_disc_price += common_term;
        entry->sum_charge += common_term.CastM2() * (one + page.l_tax[i]);
      }
    }
  }

  static void ProcessPages(LineitemPageQ1 &page, std::span<const Swip> swips,
                           HashTable &hash_table,
                           ValidHashTableIndexes &valid_hash_table_indexes,
                           Date high_date, const File &data_file) {
    for (auto swip : swips) {
      LineitemPageQ1 *data;

      if (swip.IsPageIndex()) {
        data_file.ReadPage(swip.GetPageIndex(),
                           reinterpret_cast<std::byte *>(&page));
        data = &page;
      } else {
        data = swip.GetPointer<LineitemPageQ1>();
      }
      if (do_work) {
        ProcessTuples(*data, hash_table, valid_hash_table_indexes, high_date);
      }
    }
  }

  static cppcoro::task<void> AsyncProcessPages(
      LineitemPageQ1 &page, std::vector<Swip> swips, HashTable &hash_table,
      ValidHashTableIndexes &valid_hash_table_indexes, Date high_date,
      const File &data_file, IOUring &ring, Countdown &countdown) {
    std::partition(swips.begin(), swips.end(),
                   [](Swip swip) { return swip.IsPageIndex(); });
    for (auto swip : swips) {
      LineitemPageQ1 *data;

      if (swip.IsPageIndex()) {
        co_await data_file.AsyncReadPage(ring, swip.GetPageIndex(),
                                         reinterpret_cast<std::byte *>(&page));
        data = &page;
      } else {
        data = swip.GetPointer<LineitemPageQ1>();
      }
      if (do_work) {
        ProcessTuples(*data, hash_table, valid_hash_table_indexes, high_date);
      }
    }
    countdown.Decrement();
  }

  bool IsSynchronous() const noexcept { return num_ring_entries_ == 0; }

  void StartProcessing() {
    std::atomic<uint64_t> current_swip{0ull};
    std::vector<std::thread> threads;
    threads.reserve(num_threads_);

    for (uint32_t thread_index = 0; thread_index != num_threads_;
         ++thread_index) {
      threads.emplace_back(
          [&hash_table = thread_local_hash_tables_[thread_index],
           &valid_hash_table_indexes =
               thread_local_valid_hash_table_indexes_[thread_index],
           high_date = high_date_, &current_swip, num_swips = swips_.size(),
           &swips = swips_, &data_file = data_file_,
           is_synchronous = IsSynchronous(),
           &ring = thread_local_rings_[thread_index],
           num_ring_entries = num_ring_entries_] {
            if (!is_synchronous) {
#ifdef USE_ALLOCATOR
              cppcoro::detail::allocator = new Allocator();
              cppcoro::detail::sync_allocator = new Allocator();
#endif
            }
            std::allocator<LineitemPageQ1> alloc;
            auto pages = alloc.allocate(is_synchronous ? 1 : num_ring_entries);

            // process ceil(num_tuples_per_morsel / kMaxNumTuples) pages per
            // morsel
            // => each morsel contains circa num_tuples_per_morsel tuples
            const uint64_t kSyncFetchIncrement =
                (num_tuples_per_morsel + LineitemPageQ1::kMaxNumTuples - 1) /
                LineitemPageQ1::kMaxNumTuples;

            uint64_t fetch_increment;

            if (is_synchronous) {
              fetch_increment = kSyncFetchIncrement;
            } else {
              // process num_ring_entries morsels together
              fetch_increment = kSyncFetchIncrement * num_ring_entries;
            }

            while (true) {
              auto begin = current_swip.fetch_add(fetch_increment);
              if (begin >= num_swips) {
                return;
              }
              auto end = std::min(num_swips, begin + fetch_increment);
              auto size = end - begin;

              if (is_synchronous) {
                ProcessPages(pages[0], swips.subspan(begin, size), hash_table,
                             valid_hash_table_indexes, high_date, data_file);
              } else {
                Countdown countdown(num_ring_entries);
                std::vector<cppcoro::task<void>> tasks;
                tasks.reserve(num_ring_entries + 1);

                auto num_pages_per_task =
                    (size + num_ring_entries - 1) / num_ring_entries;

                for (uint32_t i = 0; i != num_ring_entries; ++i) {
                  auto local_begin =
                      std::min(begin + i * num_pages_per_task, end);
                  auto local_end =
                      std::min(local_begin + num_pages_per_task, end);
                  tasks.emplace_back(AsyncProcessPages(
                      pages[i], {&swips[local_begin], &swips[local_end]},
                      hash_table, valid_hash_table_indexes, high_date,
                      data_file, ring, countdown));
                }
                tasks.emplace_back(DrainRing(ring, countdown));
                cppcoro::sync_wait(cppcoro::when_all_ready(std::move(tasks)));
              }
            }

            alloc.deallocate(pages, is_synchronous ? 1 : num_ring_entries);
            if (!is_synchronous) {
              delete cppcoro::detail::allocator;
              cppcoro::detail::allocator = nullptr;
              delete cppcoro::detail::sync_allocator;
              cppcoro::detail::sync_allocator = nullptr;
            }
          });
    }

    for (auto &t : threads) {
      t.join();
    }
  }

  void DoPostProcessing(bool should_print_result) {
    if (do_work) {
      // post-processing happens in a single thread. That's okay, because there
      // are only four groups
      auto &result_hash_table = thread_local_hash_tables_.front();
      auto &result_valid_hash_table_indexes =
          thread_local_valid_hash_table_indexes_.front();

      for (uint32_t i = 1; i != num_threads_; ++i) {
        auto &local_hash_table = thread_local_hash_tables_[i];
        for (auto valid_hash_table_index :
             thread_local_valid_hash_table_indexes_[i]) {
          auto &local_entry = local_hash_table[valid_hash_table_index];
          auto &result_entry = result_hash_table[valid_hash_table_index];
          if (result_entry) {
            result_entry->sum_qty += local_entry->sum_qty;
            result_entry->sum_base_price += local_entry->sum_base_price;
            result_entry->sum_disc += local_entry->sum_disc;
            result_entry->sum_disc_price += local_entry->sum_disc_price;
            result_entry->sum_charge += local_entry->sum_charge;
            result_entry->count += local_entry->count;
          } else {
            result_entry = std::move(local_entry);
            result_valid_hash_table_indexes.push_back(valid_hash_table_index);
          }
        }
      }

      std::vector<HashTableEntry *> result_entries;
      for (auto valid_hash_table_index : result_valid_hash_table_indexes) {
        result_entries.push_back(
            result_hash_table[valid_hash_table_index].get());
      }
      std::sort(result_entries.begin(), result_entries.end(),
                [](HashTableEntry *lhs, HashTableEntry *rhs) {
                  return std::pair(lhs->l_returnflag, lhs->l_linestatus) <
                         std::pair(rhs->l_returnflag, rhs->l_linestatus);
                });

      if (should_print_result) {
        std::cerr
            << "l_returnflag|l_linestatus|sum_qty|sum_base_price|sum_disc_"
               "price|sum_charge|avg_qty|avg_price|avg_disc|count_order\n";
        for (auto *entry : result_entries) {
          std::cerr << entry->l_returnflag << "|" << entry->l_linestatus << "|"
                    << entry->sum_qty << "|" << entry->sum_base_price << "|"
                    << entry->sum_disc_price << "|" << entry->sum_charge << "|"
                    << entry->sum_qty / entry->count << "|"
                    << entry->sum_base_price / entry->count << "|"
                    << entry->sum_disc / entry->count << "|" << entry->count
                    << "\n";
        }
      }
    }
  }

 private:
  std::vector<HashTable> thread_local_hash_tables_;
  std::vector<ValidHashTableIndexes> thread_local_valid_hash_table_indexes_;
  std::vector<IOUring> thread_local_rings_;
  const Date high_date_;
  const uint32_t num_threads_;
  const std::span<const Swip> swips_;
  const File &data_file_;
  const uint32_t num_ring_entries_;
};

std::vector<Swip> GetSwips(uint64_t size_of_data_file) {
  auto num_pages = size_of_data_file / kPageSize;
  std::vector<Swip> swips;
  swips.reserve(num_pages);
  for (PageIndex i = 0; i != num_pages; ++i) {
    swips.emplace_back(Swip::MakePageIndex(i));
  }
  return swips;
}

}  // namespace

int main(int argc, char *argv[]) {
  if (argc != 9) {
    std::cerr << "Usage: " << argv[0]
              << " lineitem.dat num_threads num_entries_per_ring "
                 "num_tuples_per_morsel do_work "
                 "do_random_io print_result print_header\n";
    return 1;
  }

  std::string path_to_lineitem{argv[1]};
  unsigned num_threads = std::atoi(argv[2]);
  unsigned num_entries_per_ring = std::atoi(argv[3]);
  num_tuples_per_morsel = std::atoi(argv[4]);
  std::istringstream(argv[5]) >> std::boolalpha >> do_work;
  bool do_random_io;
  std::istringstream(argv[6]) >> std::boolalpha >> do_random_io;
  bool print_result;
  std::istringstream(argv[7]) >> std::boolalpha >> print_result;
  bool print_header;
  std::istringstream(argv[8]) >> std::boolalpha >> print_header;

  const File file{path_to_lineitem.c_str(), File::kRead, true};
  auto file_size = file.ReadSize();
  auto swips = GetSwips(file_size);

  std::vector<uint64_t> swip_indexes(swips.size());
  {
    std::random_device rd;
    std::mt19937 g(rd());
    g.seed(42);

    if (do_random_io) {
      std::shuffle(swips.begin(), swips.end(), g);
    }

    std::iota(swip_indexes.begin(), swip_indexes.end(), 0ull);
    std::shuffle(swip_indexes.begin(), swip_indexes.end(), g);
  }

  Cache cache{swips, file};

  auto partition_size =
      (swip_indexes.size() + 9) / 10;  // divide in 10 partitions

  if (print_header) {
    std::cout << "kind_of_io,page_size_power,num_threads,num_cached_pages,num_"
                 "total_pages,num_entries_per_ring,num_tuples_per_morsel,do_"
                 "work,do_random_io,time,file_size,throughput\n";
  }

  // Start with 0% cached, then 10%, then 20%, ...
  for (int i = 0; i != 11; ++i) {
    if (i > 0) {
      auto offset = std::min((i - 1) * partition_size, swip_indexes.size());
      auto size = std::min(partition_size, swip_indexes.size() - offset);
      cache.Populate(
          std::span<const uint64_t>{swip_indexes}.subspan(offset, size));
    }

    {
      QueryRunner synchronousRunner{num_threads, swips, file};
      auto start = std::chrono::steady_clock::now();
      synchronousRunner.StartProcessing();
      synchronousRunner.DoPostProcessing(print_result);
      auto end = std::chrono::steady_clock::now();
      auto milliseconds =
          std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
              .count();
      std::cout << "synchronous," << kPageSizePower << "," << num_threads << ","
                << std::min(i * partition_size, swip_indexes.size()) << ","
                << swip_indexes.size() << ",0," << num_tuples_per_morsel << ","
                << std::boolalpha << do_work << "," << do_random_io << ","
                << milliseconds << "," << file_size << ","
                << (file_size / 1000000000.0) / (milliseconds / 1000.0) << "\n";
    }

    {
      QueryRunner asynchronousRunner{num_threads, swips, file,
                                     num_entries_per_ring};
      auto start = std::chrono::steady_clock::now();
      asynchronousRunner.StartProcessing();
      asynchronousRunner.DoPostProcessing(print_result);
      auto end = std::chrono::steady_clock::now();
      auto milliseconds =
          std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
              .count();
      std::cout << "asynchronous," << kPageSizePower << "," << num_threads
                << "," << std::min(i * partition_size, swip_indexes.size())
                << "," << swip_indexes.size() << "," << num_entries_per_ring
                << "," << num_tuples_per_morsel << "," << std::boolalpha
                << do_work << "," << do_random_io << "," << milliseconds << ","
                << file_size << ","
                << (file_size / 1000000000.0) / (milliseconds / 1000.0) << "\n";
    }
  }
}