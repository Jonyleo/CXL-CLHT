#ifndef CLHT_SHM_H
#define CLHT_SHM_H	

#include "shm_alloc.h"

typedef shm_offt SHM_off;

#include "clht_lf_res.h"

void * clht_shm_init(int node, int force_init, int num_buckets);
void clht_shm_term(int node, int force_destroy);

SHM_off clht_shm_alloc(uint64_t size);
void clht_shm_free(SHM_off off);
uint64_t clht_get_shm_base_addr();

SHM_off clht_table_alloc(uint64_t num_buckets);
void clht_table_free(uint64_t num_buckets);

#define GET_SHM_BASE_ADDR() clht_get_shm_base_addr()
#define SHR_OFF_TO_PTR(P) (P == SHM_NULL ? NULL : SHM_OFFT_TO_ADDR(P))
#define SHR_PTR_TO_OFF(P) (P == NULL ? SHM_NULL : ((SHM_off)((char*) ((uint64_t) P) - clht_get_shm_base_addr())))
//#define SHM_NULL 0 


#endif