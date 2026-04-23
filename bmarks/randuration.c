//#include "tbb/tbb.h"

#include "clht_lf_res.h"
#include "clht_shm.h"
#include "ssmem.h"
#include "stdio.h"

#define KEY_LIMIT(K) (K >> 11)

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
    puts("Usage: ./yscb -i [NODE_ID] -b [NUM_BUCKETS] -t [NUM_THREADS] -d [DURATION] -v [NUM_VMS]");
}

struct op_counters {
    int valid;
    uint64_t put_count;
    uint64_t get_count;
    uint64_t rem_count;
} __attribute__ ((aligned (64)));


struct op_counters counters[128] = {0};


void do_clht_op(clht_t * hashtable, uint64_t rand1, uint64_t rand2, struct op_counters * c) {
    uint64_t op = rand2 % 100;

    if(op < 30) {
        clht_put(hashtable, rand1, rand2);
        c->put_count++;
    } else if(op < 99) {
        clht_get(hashtable->ht, rand1);
        c->get_count++;
    } else {
        clht_remove(hashtable, rand1);
        c->rem_count++;
    }
}

struct worker_struct {
    int id;
    int setup;
    clht_t * ht;
    volatile _Atomic int * run_workload; 
};

void * worker_func(void * _arg) {
    struct worker_struct * arg = _arg;

    counters[arg->id].valid = 1;

    srand(time(0));

    clht_gc_thread_init(arg->ht, arg->id);

    barrier_cross(&barrier);
    
    while(*arg->run_workload) {
        do_clht_op(arg->ht, KEY_LIMIT(rand()), rand(), &counters[arg->id]);
    }

    printf("Worker %d finished\n", arg->id);

    return NULL;
}

int main(int argc, char **argv) {
    int id = -1;
    uint64_t num_buckets = 0;
    uint64_t num_thread = 0;
    uint64_t duration = 0;
    uint64_t step = 0;
    uint64_t num_vms = 0;
    bool setup = false;
    char c;

    while ((c = getopt (argc, argv, "i:b:t:d:s:v:")) != -1)
    switch (c)
      {
      case 'i':
        id = atoll(optarg);
        break;
      case 'b':
        num_buckets = atoll(optarg);
        setup = true;
        break;
      case 't':
        num_thread = atoll(optarg);
        break;
      case 'd':
        duration = atoll(optarg);
        break;
      case 's':
        step = atoll(optarg);
        break;
      case 'v':
        num_vms = atoll(optarg);
        break;
      default:
        printf("Invalid option %c\n", c);
        usage();
        return 1;
      }

    if(id == -1 || num_thread == 0 || duration == 0 || num_vms == 0) {
        usage();
        return 1;
    }

    printf("[%d] b:%ld t:%ld d:%ld s:%ld v:%ld\n", id, num_buckets, num_thread, duration, step, num_vms);

    clht_t *hashtable = (clht_t*) clht_shm_init(id, setup, num_buckets, num_vms);

    if(hashtable == NULL) {
        perror("clht_shm_init");
        return 1;
    }

    barrier_init(&barrier, num_thread);
    
    _Atomic int run_workload = 1;

    struct worker_struct *tds = (struct worker_struct *) calloc(num_thread, sizeof(struct worker_struct));

    for (uint64_t i = 0; i < num_thread; i++) {
        tds[i].id = i;
        tds[i].ht = hashtable;
        tds[i].setup = (i == 0) && setup;
        tds[i].run_workload = &run_workload;
    }

    // Workload
    pthread_t thread_group[num_thread];

    for (uint64_t i = 0; i < num_thread; i++) {
        if(pthread_create(thread_group+i, NULL, worker_func, tds+i) < 0) {
            perror("pthread_create");
        }
    }

    puts("Threads created");

    if(step != 0) {
        int elapsed = 0;

        while(elapsed < duration) {
            int sleep_dur = step;
            if((duration - elapsed) < step)
                sleep_dur = duration - elapsed;

            sleep(sleep_dur);

            uint64_t put_c = 0, rem_c = 0, get_c = 0;

            for(int i = 0; i < 128; i++) {
                if(!counters[i].valid)
                    break;

                put_c += counters[i].put_count;
                rem_c += counters[i].rem_count;
                get_c += counters[i].get_count;
            }

            printf("Inserts:%ld - Removes:%ld - Searches:%ld\n", put_c, rem_c, get_c);

            elapsed += sleep_dur;
        }

    } else {
        sleep(duration);
    }

    puts("--------------Time elapsed------------------");
    run_workload = false;

    for (uint64_t i = 0; i < num_thread; i++) {
        pthread_join(thread_group[i], NULL);
    }


    if(id == 0) {
        clht_gc_destroy(hashtable);    
    }    

    clht_shm_term(id);

    return 0;
}