/*   
 *   File: clht_lb.c
 *   Author: Vasileios Trigonakis <vasileios.trigonakis@epfl.ch>
 *   Description: lock-based cache-line hash table with no resizing
 *   clht_lb.c is part of ASCYLIB
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2014 Vasileios Trigonakis <vasileios.trigonakis@epfl.ch>
 *	      	      Distributed Programming Lab (LPD), EPFL
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <math.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#include "clht_lb.h"

#include "shm_alloc.h"

__thread ssmem_allocator_t* clht_alloc;

#ifdef DEBUG
__thread uint32_t put_num_restarts = 0;
__thread uint32_t put_num_failed_expand = 0;
__thread uint32_t put_num_failed_on_new = 0;
#endif

#include "stdlib.h"
#include "assert.h"


const char*
clht_type_desc()
{
  return "CLHT-LB-NO-RESIZE";
}

inline int
is_power_of_two (unsigned int x) 
{
return ((x != 0) && !(x & (x - 1)));
}

static inline
int is_odd (int x)
{
    return x & 1;
}

/** Jenkins' hash function for 64-bit integers. */
inline uint64_t
__ac_Jenkins_hash_64(uint64_t key)
{
    key += ~(key << 32);
    key ^= (key >> 22);
    key += ~(key << 13);
    key ^= (key >> 8);
    key += (key << 3);
    key ^= (key >> 15);
    key += ~(key << 27);
    key ^= (key >> 31);
    return key;
}

/* Create a new bucket. */
bucket_t*
clht_bucket_create() 
{
  shm_offt bucket_off = SHM_NULL;
  bucket_off = shm_malloc(sizeof(bucket_t));
  if (bucket_off == SHM_NULL)
    {
      printf("** shm_malloc @ clht_bucket_create\n");
      return NULL;
    }

  bucket_t* bucket = SHM_OFFT_TO_ADDR(bucket_off);

  bucket->lock = 0;

  uint32_t j;
  for (j = 0; j < ENTRIES_PER_BUCKET; j++)
    {
      bucket->key[j] = 0;
    }
  bucket->next = SHM_NULL;
    
  return bucket;
}

clht_t* 
clht_create(uint64_t num_buckets)
{
  shm_offt w_off = SHM_NULL;
  w_off = shm_malloc(sizeof(clht_t));
  if (w_off == SHM_NULL)
    {
      printf("** shm_malloc @ clht_create\n");
      return NULL;
    }

  clht_t* w = (clht_t*) SHM_OFFT_TO_ADDR(w_off);
  
  w->ht = clht_hashtable_create(num_buckets);
  if (w->ht == SHM_NULL)
    {
      shm_free(w_off);
      return NULL;
    }

  return w;
}

shm_offt
clht_hashtable_create(uint64_t num_buckets) 
{
  clht_hashtable_t* hashtable = NULL;
    
  if (num_buckets == 0)
    {
      return SHM_NULL;
    }
    
  /* Allocate the table itself. */
  shm_offt hashtable_off = SHM_NULL;
  hashtable_off = shm_malloc(sizeof(clht_hashtable_t));
  if (hashtable_off == SHM_NULL) 
    {
      printf("** shm_malloc @ clht_hashtable_create hashtable\n");
      return SHM_NULL;
    }

  hashtable = (clht_hashtable_t*) SHM_OFFT_TO_ADDR(hashtable_off);


  hashtable->table =  shm_malloc(num_buckets * (sizeof(bucket_t)));
  if (hashtable->table == SHM_NULL) 
    {
      printf("** shm_malloc: clht_hashtable_create table\n"); 
      shm_free(hashtable->table);
      return SHM_NULL;
    }

  bucket_t * table = SHM_OFFT_TO_ADDR(hashtable->table);
  
  memset(table, 0, num_buckets * (sizeof(bucket_t)));
    
  uint64_t i;
  for (i = 0; i < num_buckets; i++)
    {
      table[i].lock = 0;
      uint32_t j;
      for (j = 0; j < ENTRIES_PER_BUCKET; j++)
	{
	  table[i].key[j] = 0;
	}
    }

  hashtable->num_buckets = num_buckets;
    
  return hashtable_off;
}

/* Hash a key for a particular hash table. */
uint64_t
clht_hash(clht_hashtable_t* hashtable, clht_addr_t key) 
{
	/* uint64_t hashval; */
	/* hashval = __ac_Jenkins_hash_64(key); */
	/* return hashval % hashtable->num_buckets; */
  /* return key % hashtable->num_buckets; */
  return key & (hashtable->num_buckets - 1);
}


  /* Retrieve a key-value entry from a hash table. */
clht_val_t
clht_get(clht_hashtable_t* hashtable, clht_addr_t key)
{
  size_t bin = clht_hash(hashtable, key);
  volatile bucket_t* bucket = SHM_OFFT_TO_ADDR(hashtable->table) + bin;
    
  uint32_t j;
  do 
    {
      for (j = 0; j < ENTRIES_PER_BUCKET; j++) 
	{
	  clht_val_t val = bucket->val[j];
#ifdef __tile__
	  _mm_lfence();
#endif
	  if (bucket->key[j] == key) 
	    {
	      if (bucket->val[j] == val)
		{
		  return val;
		}
	      else
		{
		  return 0;
		}
	    }
	}

      bucket = SHM_OFFT_TO_ADDR(bucket->next);
    } 
  while (bucket != NULL);
  return 0;
}

inline clht_addr_t
bucket_exists(bucket_t* bucket, clht_addr_t key)
{
  uint32_t j;
  do 
    {
      for (j = 0; j < ENTRIES_PER_BUCKET; j++) 
	{
	  if (bucket->key[j] == key) 
	    {
	      return true;
	    }
	}

      bucket = SHM_OFFT_TO_ADDR(bucket->next);
    } while (bucket != NULL);
  return false;
}


/* Insert a key-value entry into a hash table. */
int
clht_put(clht_t* h, clht_addr_t key, clht_val_t val) 
{
  clht_hashtable_t* hashtable = SHM_OFFT_TO_ADDR(h->ht);
  size_t bin = clht_hash(hashtable, key);
  bucket_t* bucket = ((bucket_t*) SHM_OFFT_TO_ADDR(hashtable->table)) + bin;

#if defined(READ_ONLY_FAIL)
  if (bucket_exists(bucket, key))
    {
      return false;
    }
#endif
  clht_lock_t* lock = &bucket->lock;

  clht_addr_t* empty = NULL;
  clht_val_t* empty_v = NULL;

  uint32_t j;

  LOCK_ACQ(lock);
  do 
    {
      for (j = 0; j < ENTRIES_PER_BUCKET; j++) 
	{
	  if (bucket->key[j] == key) 
	    {
	      LOCK_RLS(lock);
	      return false;
	    }
	  else if (empty == NULL && bucket->key[j] == 0)
	    {
	      empty = &bucket->key[j];
	      empty_v = &bucket->val[j];
	    }
	}
        
      if (bucket->next == SHM_NULL)
	{
	  if (empty == NULL)
	    {
	      DPP(put_num_failed_expand);
        bucket_t* next_ptr = clht_bucket_create();
	      bucket->next = SHM_ADDR_TO_OFFT(next_ptr);
	      next_ptr->key[0] = key;
#ifdef __tile__
	      _mm_sfence();
#endif
	      next_ptr->val[0] = val;
	    }
	  else 
	    {
	      *empty_v = val;
#ifdef __tile__
	      _mm_sfence();
#endif
	      *empty = key;
	    }

	  LOCK_RLS(lock);
	  return true;
	}

      bucket = SHM_OFFT_TO_ADDR(bucket->next);
    } while (true);
}



/* Remove a key-value entry from a hash table. */
clht_val_t
clht_remove(clht_t* h, clht_addr_t key)
{
  clht_hashtable_t* hashtable = SHM_OFFT_TO_ADDR(h->ht);
  size_t bin = clht_hash(hashtable, key);
  bucket_t* bucket = ((bucket_t *) SHM_OFFT_TO_ADDR(hashtable->table)) + bin;

#if defined(READ_ONLY_FAIL)
  if (!bucket_exists(bucket, key))
    {
      return false;
    }
#endif  /* READ_ONLY_FAIL */

  clht_lock_t* lock = &bucket->lock;
  uint32_t j;

  LOCK_ACQ(lock);
  do 
    {
      for (j = 0; j < ENTRIES_PER_BUCKET; j++) 
	{
	  if (bucket->key[j] == key) 
	    {
	      clht_val_t val = bucket->val[j];
	      bucket->key[j] = 0;
	      LOCK_RLS(lock);
	      return val;
	    }
	}
      bucket = SHM_OFFT_TO_ADDR(bucket->next);
    } while (bucket != NULL);
  LOCK_RLS(lock);
  return false;
}

static uint32_t
clht_put_seq(clht_hashtable_t* hashtable, clht_addr_t key, clht_val_t val, uint64_t bin) 
{
  bucket_t* bucket = ((bucket_t*) SHM_OFFT_TO_ADDR(hashtable->table)) + bin;
  clht_addr_t* empty = NULL;
  clht_val_t* empty_v = NULL;
  uint32_t j;

  do 
    {
      for (j = 0; j < ENTRIES_PER_BUCKET; j++) 
	{
	  if (bucket->key[j] == key) 
	    {
	      return false;
	    }
	  else if (empty == NULL && bucket->key[j] == 0)
	    {
	      empty = &bucket->key[j];
	      empty_v = &bucket->val[j];
	    }
	}
        
      if (bucket->next == SHM_NULL)
	{
	  if (empty == NULL)
	    {
	      DPP(put_num_failed_expand);
        bucket_t* next_ptr = clht_bucket_create();
	      bucket->next = SHM_ADDR_TO_OFFT(next_ptr);
	      next_ptr->key[0] = key;
	      next_ptr->val[0] = val;
	    }
	  else 
	    {
	      *empty_v = val;
	      *empty = key;
	    }
	  return true;
	}

      bucket = SHM_OFFT_TO_ADDR(bucket->next);
    } while (true);
}


static inline void
bucket_cpy(bucket_t* bucket, clht_hashtable_t* ht_new)
{
  LOCK_ACQ(&bucket->lock);
  uint32_t j;
  do 
    {
      for (j = 0; j < ENTRIES_PER_BUCKET; j++) 
	{
	  clht_addr_t key = bucket->key[j];
	  if (key != 0) 
	    {
	      uint64_t bin = clht_hash(ht_new, key);
	      clht_val_t val = bucket->key[j];
	      clht_put_seq(ht_new, key, val, bin);
	    }
	}
      bucket = SHM_OFFT_TO_ADDR(bucket->next);
    } while (bucket != NULL);

}

void
clht_destroy(clht_hashtable_t* hashtable)
{
  shm_free(hashtable->table);
  shm_free(SHM_ADDR_TO_OFFT(hashtable));
}



size_t
clht_size(clht_hashtable_t* hashtable)
{
  uint64_t num_buckets = hashtable->num_buckets;
  bucket_t* bucket = NULL;
  size_t size = 0;

  uint64_t bin;
  for (bin = 0; bin < num_buckets; bin++)
    {
      bucket = ((bucket_t*) SHM_OFFT_TO_ADDR(hashtable->table)) + bin;
       
      uint32_t j;
      do
	{
	  for (j = 0; j < ENTRIES_PER_BUCKET; j++)
	    {
	      if (bucket->key[j] > 0)
		{
		  size++;
		}
	    }

	  bucket = SHM_OFFT_TO_ADDR(bucket->next);
	}
      while (bucket != NULL);
    }
  return size;
}

void
clht_print(clht_hashtable_t* hashtable)
{
  uint64_t num_buckets = hashtable->num_buckets;
  bucket_t* bucket;

  printf("Number of buckets: %u\n", num_buckets);

  uint64_t bin;
  for (bin = 0; bin < num_buckets; bin++)
    {
      bucket = ((bucket_t*) SHM_OFFT_TO_ADDR(hashtable->table)) + bin;
      
      printf("[[%05d]] ", bin);

      uint32_t j;
      do
	{
	  for (j = 0; j < ENTRIES_PER_BUCKET; j++)
	    {
	      if (bucket->key[j])
	      	{
		  printf("(%-5llu)-> ", (long long unsigned int) bucket->key[j]);
		}
	    }

	  bucket = SHM_OFFT_TO_ADDR(bucket->next);
	  printf(" ** -> ");
	}
      while (bucket != NULL);
      printf("\n");
    }
  fflush(stdout);
}
