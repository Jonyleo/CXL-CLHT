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
    puts("Usage: ./yscb -i [NODE_ID] -b [NUM_BUCKETS] -k [NUM_KEYS_PER_NODE] -t [NUM_THREADS] -d [DURATION]");
}

void do_clht_op(clht_t * hashtable, uint64_t rand1, uint64_t rand2) {
    uint64_t op = rand2 % 100;

    if(op < 30) {
        clht_put(hashtable, rand1, rand2);
    } else if(op < 99) {
        clht_get(hashtable->ht, rand1);
    } else {
        clht_remove(hashtable, rand1);
    }
}

int main(int argc, char **argv) {
    int id = -1;
    uint64_t num_keys = 0;
    uint64_t num_buckets = 0;
    uint64_t num_thread = 0;
    uint64_t duration = 0;
    bool setup = false;
    char c;

    while ((c = getopt (argc, argv, "i:b:k:t:d:")) != -1)
    switch (c)
      {
      case 'i':
        id = atoll(optarg);
        break;
      case 'b':
        num_buckets = atoll(optarg);
        setup = true;
        break;
      case 'k':
        num_keys = atoll(optarg);
        break;
      case 't':
        num_thread = atoll(optarg);
        break;
      case 'd':
        duration = atoll(optarg);
        break;
      default:
        printf("Invalid option %c\n", c);
        usage();
        return 1;
      }

    if(id == -1 || num_keys == 0 || num_thread == 0 || duration == 0) {
        usage();
        return 1;
    }

    printf("[%d] k:%ld b:%ld t:%ld d:%ld\n", id, num_keys, num_buckets, num_thread, duration);


    clht_t *hashtable = (clht*) clht_shm_init(id, setup, num_buckets);
    if(hashtable == NULL) {
        return 1;
    }

    barrier_init(&barrier, num_thread);

    uint64_t key_start = (id*num_keys)+1;
    uint64_t key_end = (id+1)*num_keys+1;
    printf("%ld %ld\n", key_start, key_end);

    thread_data_t *tds = (thread_data_t *) malloc(num_thread * sizeof(thread_data_t));

    std::atomic<int> next_thread_id;

    std::atomic<bool> run_workload = true;

    {
        // Workload
        auto starttime = std::chrono::system_clock::now();
        next_thread_id.store(0);
        auto func = [&]() {
            int thread_id = next_thread_id.fetch_add(1);
            tds[thread_id].id = thread_id;
            tds[thread_id].ht = hashtable;

            std::random_device dev;
            std::mt19937_64 rng(dev());
            std::uniform_int_distribution<std::mt19937_64::result_type> dist(key_start,key_end-1);

            clht_gc_thread_init(tds[thread_id].ht, tds[thread_id].id);
            barrier_cross(&barrier);

            if(setup && thread_id == 0) {
                clht_put(hashtable, -1, -1);
            }

            while(run_workload) {
                do_clht_op(tds[thread_id].ht, dist(rng), dist(rng));
            }

            if(!setup && thread_id == 0) {
                printf("%ld\n", clht_get(hashtable->ht, -1));
            }
        };

        auto timer = [&]() {
            std::this_thread::sleep_for(std::chrono::seconds(duration));
            run_workload = false;

            puts("FINISH\n\n");
        };

        std::vector<std::thread> thread_group;

        for (uint64_t i = 0; i < num_thread; i++)
            thread_group.push_back(std::thread{func});

        thread_group.push_back(std::thread{timer});

        for (uint64_t i = 0; i < num_thread+1; i++)
            thread_group[i].join();

        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now() - starttime);
        printf("Throughput: randuration, %f ,ops/us\n", (num_keys * 1.0) / duration.count());
    }

    if(id == 0) {
        clht_gc_destroy(hashtable);    
    }    

    return 0;
}