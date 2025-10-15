#include <iostream>
#include <chrono>
#include <random>
#include <vector>
#include <thread>
#include <atomic>
#include "tbb/tbb.h"

using namespace std;

extern "C" {
    #include "clht_shm.h"
    #include "ssmem.h"
}

typedef struct thread_data {
    uint32_t id;
    clht_t *ht;
} thread_data_t;

typedef struct barrier {
    pthread_cond_t complete;
    pthread_mutex_t mutex;
    int count;
    int crossing;
} barrier_t;

void barrier_init(barrier_t *b, int n) {
    pthread_cond_init(&b->complete, NULL);
    pthread_mutex_init(&b->mutex, NULL);
    b->count = n;
    b->crossing = 0;
}

void barrier_cross(barrier_t *b) {
    pthread_mutex_lock(&b->mutex);
    b->crossing++;
    if (b->crossing < b->count) {
        pthread_cond_wait(&b->complete, &b->mutex);
    } else {
        pthread_cond_broadcast(&b->complete);
        b->crossing = 0;
    }
    pthread_mutex_unlock(&b->mutex);
}

barrier_t barrier;

void usage() {
    puts("Usage: ./yscb -i [NODE_ID] -b [NUM_BUCKETS] -k [NUM_KEYS] -t [NUM_THREADS] -s");
}


int main(int argc, char **argv) {
    int id = -1;
    uint64_t num_keys = 0;
    uint64_t num_buckets = 0;
    uint64_t num_thread = 0;
    bool setup = false;
    char c;

    while ((c = getopt (argc, argv, "i:b:k:t:s")) != -1)
    switch (c)
      {
      case 'i':
        id = atoll(optarg);
        break;
      case 'b':
        num_buckets = atoll(optarg);
        break;
      case 'k':
        num_keys = atoll(optarg);
        break;
      case 't':
        num_thread = atoll(optarg);
        break;
      case 's':
        setup = true;
        break;
      default:
        printf("Invalid option %c\n", c);
        usage();
        return 1;
      }

    if(id == -1 || num_keys == 0 || num_buckets == 0 || num_thread == 0) {
        usage();
        return 1;
    }

    printf("[%d] - k:%ld b:%ld t:%ld\n", id, num_keys, num_buckets, num_thread);

    uint64_t *keys = new uint64_t[num_keys];

    // Generate keys
    for (uint64_t i = 0; i < num_keys; i++) {
        keys[i] = i + ((id+1) * num_keys) + 1;
    }

    clht_t *hashtable = (clht*) clht_shm_init(id, setup, num_buckets);
    if(hashtable == NULL) {
        return 1;
    }

    barrier_init(&barrier, num_thread);

    thread_data_t *tds = (thread_data_t *) malloc(num_thread * sizeof(thread_data_t));

    std::atomic<int> next_thread_id;

    {
        // Load
        auto starttime = std::chrono::system_clock::now();
        next_thread_id.store(0);
        auto func = [&]() {
            int thread_id = next_thread_id.fetch_add(1);
            tds[thread_id].id = thread_id;
            tds[thread_id].ht = hashtable;

            uint64_t start_key = num_keys / num_thread * (uint64_t)thread_id;
            uint64_t end_key = start_key + num_keys / num_thread;

            clht_gc_thread_init(tds[thread_id].ht, tds[thread_id].id);
            barrier_cross(&barrier);

            for (uint64_t i = start_key; i < end_key; i++) {
                clht_put(tds[thread_id].ht, keys[i], keys[i]);
            }
        };

        std::vector<std::thread> thread_group;

        for (uint64_t i = 0; i < num_thread; i++)
            thread_group.push_back(std::thread{func});

        for (uint64_t i = 0; i < num_thread; i++)
            thread_group[i].join();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now() - starttime);
        printf("Throughput: load, %f ,ops/us\n", (num_keys * 1.0) / duration.count());
    }

    barrier.crossing = 0;

    {
        // Run
        auto starttime = std::chrono::system_clock::now();
        next_thread_id.store(0);
        auto func = [&]() {
            int thread_id = next_thread_id.fetch_add(1);
            tds[thread_id].id = thread_id;
            tds[thread_id].ht = hashtable;

            uint64_t start_key = num_keys / num_thread * (uint64_t)thread_id;
            uint64_t end_key = start_key + num_keys / num_thread;

            clht_gc_thread_init(tds[thread_id].ht, tds[thread_id].id);
            barrier_cross(&barrier);

            for (uint64_t i = start_key; i < end_key; i++) {
                    uintptr_t val = clht_get(tds[thread_id].ht->ht, keys[i]);
                    if (val != keys[i]) {
                        std::cout << "[CLHT] wrong key read: " << val << "expected: " << keys[i] << std::endl;
                        exit(1);
                    }
            }
        };

        std::vector<std::thread> thread_group;

        for (uint64_t i = 0; i < num_thread; i++)
            thread_group.push_back(std::thread{func});

        for (uint64_t i = 0; i < num_thread; i++)
            thread_group[i].join();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now() - starttime);
        printf("Throughput: run, %f ,ops/us\n", (num_keys * 1.0) / duration.count());
    }
    clht_gc_destroy(hashtable);

    delete[] keys;

    return 0;
}