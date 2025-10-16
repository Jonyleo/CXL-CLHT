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
#define SHM_TABLE_SIZE (1UL << 36) 
// 2MB for the comms (must be aligned to 2M due to devdax restructions)
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
};

void * shm_base = NULL;
struct cxl_comm * comm = NULL;
void * table_base = NULL;

static char* get_cxl_path() {
	char * cxl_path = getenv("CXL_PATH");

	if(cxl_path == NULL)
		return CXL_PATH_DEFAULT;

	return cxl_path;
}

static void* clht_mmap_cxl(char * path, uint64_t size, int * fd_out) {
	int fd;
	if ((fd = open(path, O_RDWR, 0666)) < 0) {
	    perror("open(create) @ clht_mmap_cxl");
	    return NULL;
	}

	void* res = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED_VALIDATE | MAP_SYNC, fd, 0);
	if(res == MAP_FAILED) {
	    perror("mmap @ clht_mmap_cxl");
	    return NULL;
	}

	if(fd_out == NULL)
		close(fd);
	else
		*fd_out = fd;

	return res;
}

static void * _clht_shm_init(char * path) {
	int fd = 0;
	void * res = clht_mmap_cxl(path, CXL_DAX_SIZE_ALIGNED, &fd);

	if(fd == 0) {
		return NULL;
	}

	comm = (struct cxl_comm *) mmap(((char*) res) + SHM_MAPPING_SIZE_ALIGNED, 
									SHM_COMM_SIZE, 
									PROT_READ | PROT_WRITE, 
									MAP_SHARED_VALIDATE | MAP_SYNC | MAP_FIXED, 
									fd, 
									SHM_MAPPING_SIZE_ALIGNED);


	assert((char*)comm == (((char*) res) + SHM_MAPPING_SIZE_ALIGNED));

	if(comm == MAP_FAILED) {
		perror("mmap @ _clht_shm_init comm");
		return NULL;
	}

	table_base = mmap(((char*) res) + SHM_MAPPING_SIZE_ALIGNED + SHM_COMM_SIZE, SHM_TABLE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED_VALIDATE | MAP_SYNC, fd, SHM_MAPPING_SIZE_ALIGNED + SHM_COMM_SIZE);
	if(table_base == NULL) {
		perror("mmap @ _clht_shm_init table");
		return NULL;
	}
	close(fd);

	return res;
}


void * clht_shm_init(int node, int force_init, int num_buckets) {
	char * cxl_path = get_cxl_path();

	void * shm_base = _clht_shm_init(cxl_path);
	if(shm_base == NULL) {
		return NULL;
	}

		if(force_init) {
			memset(shm_base, 0, SHM_COMM_SIZE + SHM_MAPPING_SIZE_ALIGNED);
		}

	shm_init(shm_base, cxl_path);
	
    if(CAS_U8(&comm->initialized, 0, 1) == 1) {
    	while(comm->initialized != 2);
    }

    if(comm->initialized == 1) {
    	printf("[%d] Initilizing CLHT\n", node);

		comm->table_end = 0;
    	comm->clht = clht_create(num_buckets);

    	comm->initialized = 2;
    } else {
    	printf("[%d] Obtaining CLHT\n", node);
    }

    return (void*) SHR_OFF_TO_PTR(comm->clht);
}


void clht_shm_term(int node, int force_destroy) {
	shm_deinit();
	munmap(table_base, SHM_TABLE_SIZE);
	munmap(comm, SHM_COMM_SIZE);

	if(force_destroy) {
		memset(shm_base, 0, SHM_COMM_SIZE + SHM_MAPPING_SIZE_ALIGNED);
	}
}

SHM_off clht_shm_alloc(uint64_t size) {
	return shm_malloc(size);
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

	return (SHM_off) (SHM_MAPPING_SIZE_ALIGNED + SHM_COMM_SIZE + old_table_end);
}

void clht_table_free(uint64_t num_buckets) {
	// pass
}