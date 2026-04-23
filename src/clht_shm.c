#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "clht_shm.h"

#include "shm_alloc.h"
#include "shm_constants.h"

#define CXL_PATH_DEFAULT "/dev/dax2.0"  

#define CXL_ALIGNEMNT (21)
#define CXL_ALIGN_ADDR(A) (((A >> CXL_ALIGNEMNT)+1) << CXL_ALIGNEMNT)

/*
CXL - DAX
| - SHM_MAPPING_SIZE_ALIGNED
| - 1 Huge Page - Comm struct
|	- SHM_TABLE_SIZE
*/

// 64GB for the hashtables
#define SHM_TABLE_SIZE (1UL << 34) 
// 2MB for the comms (must be aligned to 2M due to devdax restrictions)
#define SHM_COMM_SIZE (1UL << CXL_ALIGNEMNT)
// SHM_MAPPING_SIZE as defined by shm_alloc (aligned to 2M)
#define SHM_MAPPING_SIZE_ALIGNED CXL_ALIGN_ADDR(SHM_MAPPING_SIZE)

#define CXL_DAX_SIZE (SHM_MAPPING_SIZE_ALIGNED + SHM_COMM_SIZE +  SHM_TABLE_SIZE)

#define CXL_DAX_SIZE_ALIGNED CXL_ALIGN_ADDR(CXL_DAX_SIZE)

#include "atomic_ops.h"

struct cxl_comm {
	_Atomic SHM_off clht;
	_Atomic uint8_t initialized;
	_Atomic uint64_t table_end;
	_Atomic uint64_t connected_vms;
};

void * shm_base = NULL;
struct cxl_comm * comm = NULL;
void * table_base = NULL;

int read_ptr(int * ptr) { return *ptr; }

static void * allocate(char * path, size_t size) {
    int fd;

    if ((fd = open(path, O_RDWR, 0)) < 0) {
        perror("open");
        return NULL;
    }
    
    void * mmap_res = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(mmap_res == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }

    close(fd);

    return mmap_res;
}

static void * _clht_shm_init(int leader) {


	void * res = (void *) allocate("/dev/mewsi", CXL_DAX_SIZE_ALIGNED);
	if(res == MAP_FAILED) {
		return NULL;
	}

	comm = (struct cxl_comm*) (((char*)res) + SHM_MAPPING_SIZE_ALIGNED);
	table_base = ((char*)res) + SHM_MAPPING_SIZE_ALIGNED + SHM_COMM_SIZE;

	return res;
}

void * shm_base;

void * clht_shm_init(int node, int force_init, int num_buckets, int num_vms) {
	if(shm_base)
    	return (void*) SHR_OFF_TO_PTR(comm->clht);

	shm_base = _clht_shm_init(force_init);
	if(shm_base == NULL) {
		return NULL;
	}
	shm_init(force_init, shm_base);

	if(force_init) {
		comm->initialized = 0;
	}
	
    if(CAS_U8(&comm->initialized, 0, 1) == 1) {
    	while(comm->initialized != 2);
    }

    if(comm->initialized == 1) {
    	printf("[%d] Initializing CLHT\n", node);

		comm->table_end = 0;
    	comm->clht = clht_create(num_buckets);

    	comm->initialized = 2;
    	comm->connected_vms = 1;
    } else {
    	printf("[%d] Obtaining CLHT\n", node);
    	comm->connected_vms++;
    }

    while(comm->connected_vms != num_vms);

    printf("All VMs connected\n");


    return (void*) SHR_OFF_TO_PTR(comm->clht);
}


void clht_shm_term(int node) {
	// TODO - Decide if is last node in the system. if yes destroy meta ? Should this happen?
	shm_deinit();

	comm->connected_vms--;
	if(comm->connected_vms == 0) {
    	printf("All VMs disconnected\n");
	}

	munmap(shm_base, CXL_DAX_SIZE_ALIGNED);
	shm_base = NULL;
}

SHM_off clht_shm_alloc(uint64_t size) {
	SHM_off res = shm_malloc(size);
  	memset(SHR_OFF_TO_PTR(res), 0, size);
	return res;
}

void clht_shm_free(SHM_off off) {
	return shm_free(off);
}

uint64_t clht_get_shm_base_addr() {
	return (uint64_t) get_shm_user_base();
}

SHM_off clht_table_alloc(uint64_t num_buckets) {
	uint64_t new_table_end;
	uint64_t old_table_end; 
	uint64_t size = num_buckets * sizeof(bucket_t);

	do {
		old_table_end = comm->table_end;
		new_table_end = old_table_end + size;

		if(new_table_end > CXL_DAX_SIZE) {
			puts("OUT OF MEMORY FOR HASHTABLE");
			exit(-1);
		}

	} while(CAS_U64(&comm->table_end, old_table_end, new_table_end) != old_table_end);

	SHM_off off = (SHM_MAPPING_SIZE_ALIGNED + SHM_COMM_SIZE + old_table_end);

  	memset ((char *)SHR_OFF_TO_PTR(off), 0, size);

  	return off;
}

void clht_table_free(uint64_t num_buckets) {
	// pass
}