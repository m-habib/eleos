#include "Face_Verification_Enclave_t.h"
#include "sgx_trts.h"

#include "Queue.h"
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <sgx_trts.h>
#include <stdio.h>
#include <sys/types.h>
#include "Aptr.h"
#include "mem.h"
#include <sgx_tcrypto.h>

#define DUP_NUM 2
#define MAX_REQUESTS 99999 

#define HISTOGRAM_LEN 58
#define IMG_WIDTH 512
#define BLOCK_WIDTH 16
#define NBLOCKS_DIM (IMG_WIDTH / BLOCK_WIDTH)
#define NBLOCKS (NBLOCKS_DIM * NBLOCKS_DIM)
#define KEY_LENGTH_BYTES 40
#define HISTOGRAM_LENGTH_BYTES (sizeof(int) * (HISTOGRAM_LEN) * (NBLOCKS))
#define ACCEPT_THRESHOLD 120000.0
#define to_mat(x) (unsigned char (*)[IMG_WIDTH])(x)
#define PAVEL_APTR
#define ROT_INVARIANT
#define READ_REQ_FROM_FILES

sgx_aes_ctr_128bit_key_t aes_key = { 0 };
#define CTR_SIZE 16*255
uint8_t ctr_dec[CTR_SIZE] = { 0 };
uint8_t ctr_enc[CTR_SIZE] = { 0 };

struct entry_t {
	unsigned char key[40];
	unsigned char* value;
};

struct hash_t {
	struct entry_t* id;
	size_t nentries;
	size_t total_size;
	size_t nbuckets;
};

struct req_t {
	unsigned char name[256 + 40];
#ifdef READ_REQ_FROM_FILES
	char fname[256 + 40];
#else
	unsigned char image[IMG_WIDTH][IMG_WIDTH];
#endif
};

Queue* g_request_queue;
Queue* g_response_queue;

void hash_init(struct hash_t* h);
void hash_destroy(struct hash_t* h);
void hash_get(struct hash_t* h, unsigned char* key, unsigned char** val);
void hash_put(struct hash_t* h, unsigned char* key, unsigned char* val, int override);

int pattern2bin[256];
struct hash_t hash;

void * my_malloc(size_t size) {
	void *ptr = malloc(size);
	if (!ptr) {
		//perror("malloc");
		//exit(1);
		debug("malloc error!\n");
		abort();
	}
	memset(ptr, 0, size);
	return ptr;
}

void * my_realloc(void *ptr, size_t old_size, size_t new_size) {
	//	printf("realloc!\n");
	void *new_ptr = my_malloc(new_size);
	memset(new_ptr, 0, new_size);
	memcpy(new_ptr, ptr, old_size);
	free(ptr);
	return new_ptr;
}


long hash_function(unsigned char *str, int nbuckets) {
	unsigned long hash = 5381;
	int c;

	while ((c = *str++)) {
		hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
	}

	return hash % nbuckets;
}

void hash_init(struct hash_t* h)
{
	h->nentries = 0;
	h->nbuckets = 1024 * DUP_NUM;
	h->total_size = h->nbuckets * 40 * sizeof(unsigned char);
	h->id = (struct entry_t*)my_malloc(sizeof(struct entry_t) * h->nbuckets);
	for (unsigned int i = 0; i<h->nbuckets; i++) {
		h->id[i].key[0] = '\0';
		h->id[i].value = NULL;
	}
}

void hash_destroy(struct hash_t* h)
{
	for (unsigned int i = 0; i<h->nbuckets; i++) {
#ifndef PAVEL_APTR
		if (h->id[i].value) free(h->id[i].value);
#else
		if (h->id[i].value) memsys5Free(h->id[i].value);
#endif
	}
	free(h->id);
}


void hash_put(struct hash_t* h, unsigned char* key, unsigned char* val, int override)
{
	unsigned char* old_val = NULL;
	hash_get(h, key, &old_val);

	if (old_val && !override) return;

	if (old_val) h->nentries--; // will get updated to 0 in end of func

	int hash = hash_function(key, h->nbuckets);
	int idx = hash;

	while ((h->id[idx].value != NULL) && strcmp((char*)h->id[idx].key, (char*)key))
	{
		idx = (idx + 1) % h->nbuckets;
	}


	struct entry_t* entry = &h->id[idx];
	strncpy((char*)entry->key, (char*)key, 40);
#ifndef PAVEL_APTR
	entry->value = (unsigned char*)my_malloc(sizeof(int)*HISTOGRAM_LEN*NBLOCKS);
	memcpy(entry->value, val, sizeof(int)*HISTOGRAM_LEN*NBLOCKS);
#else
	entry->value = (unsigned char*)memsys5Malloc(sizeof(int) * HISTOGRAM_LEN*NBLOCKS);
	Aptr<char> ptr = Aptr<char>(entry->value, sizeof(int) * HISTOGRAM_LEN*NBLOCKS, 0);
	memmove_aptr_reg(&ptr, val, sizeof(int)*HISTOGRAM_LEN*NBLOCKS);
#endif

	h->total_size += sizeof(int)*HISTOGRAM_LEN*NBLOCKS;
	h->nentries++;
}

void hash_get(struct hash_t* h, unsigned char* key, unsigned char** val)
{
	int hash = hash_function(key, h->nbuckets);
	int idx = hash;
	struct entry_t* entry;

	do {
		entry = &h->id[idx];
		if ((entry->value != NULL) && (!strcmp((char*)key, (char*)h->id[idx].key)))
		{
			*val = entry->value;
			return;
		}
		idx = (idx + 1) % h->nbuckets;
	} while ((idx != hash) && (entry->value != NULL));
	*val = NULL;
}

unsigned char get_pattern(unsigned char img[IMG_WIDTH][IMG_WIDTH], int r, int c) {
	unsigned char v = img[r][c];
	unsigned char pattern = ((img[r - 1][c - 1] > v) << 7) |
		((img[r - 1][c] > v) << 6) |
		((img[r - 1][c + 1] > v) << 5) |
		((img[r][c + 1] > v) << 4) |
		((img[r + 1][c + 1] > v) << 3) |
		((img[r + 1][c] > v) << 2) |
		((img[r + 1][c - 1] > v) << 1) |
		((img[r][c - 1] > v) << 0);
	return pattern;
}

int is_pattern_uniform(unsigned char pattern) {
	unsigned char last = pattern & 1;
	unsigned char cur = pattern;
	int transitions = 0;
	int i;
	for (i = 0; i < 8; i++) {
		if ((cur & 1) != last) {
			transitions++;
		}
		last = cur & 1;
		cur >>= 1;
	}
	if ((pattern & 1) != ((pattern >> 7) & 1)) /* circular transition */
		transitions++;
	return transitions < 3;
}

unsigned char rotate(unsigned char pattern, int rotation) {
	unsigned char cur = pattern;
	int i;
	unsigned char lsb;
	for (i = 0; i < rotation; i++) {
		lsb = cur & 1;
		cur >>= 1;
		cur |= (lsb << 7);
	}
	return cur;
}


int create_pattern2bin(int *pattern2bin) {
	int i;
	int next;
	next = 0;
	for (i = 0; i < 256; i++)
		pattern2bin[i] = 0xff;

	for (i = 0; i < 256; i++) {
		if (pattern2bin[i] != 0xff)
			continue;

		if (is_pattern_uniform(i)) {
			pattern2bin[i] = next;
#ifdef ROT_INVARIANT
			int j;
			for (j = 0; j < 8; j++) {
				pattern2bin[rotate(i, j)] = next;
			}
#endif
			next++;
		}
		else {
			pattern2bin[i] = HISTOGRAM_LEN - 1;
		}
	}
	/*if (next != HISTOGRAM_LEN)
	printf("expected %d bins. got %d\n", HISTOGRAM_LEN, next);*/
	return 0;
}

float histogram_distance(int h1[HISTOGRAM_LEN * NBLOCKS], int h2[HISTOGRAM_LEN * NBLOCKS]) {
	float chisqr = 0;
	int diff;
	int sum;
	int i;
#pragma omp simd
	for (i = 0; i < HISTOGRAM_LEN * NBLOCKS; i++) {
		diff = h1[i] - h2[i];
		sum = h1[i] + h2[i];
		if (sum != 0) chisqr += (float)(diff * diff) / sum;
	}
	return chisqr;
}

void calc_histogram(int *histogram, unsigned char img[IMG_WIDTH][IMG_WIDTH], int *pattern2bin) {
	memset(histogram, 0, sizeof(int) * HISTOGRAM_LEN * NBLOCKS);

	int i, j;
	for (i = 1; i < IMG_WIDTH - 1; i++) {
		for (j = 1; j < IMG_WIDTH - 1; j++) {
			int pattern = get_pattern(img, i, j);
			int histogram_i = i / BLOCK_WIDTH;
			int histogram_j = j / BLOCK_WIDTH;
			int start_offset = histogram_i * NBLOCKS_DIM * HISTOGRAM_LEN + histogram_j * HISTOGRAM_LEN;
			histogram[start_offset + pattern2bin[pattern]]++;
		}
	}

}

float do_one(struct hash_t *hashmap, int *pattern2bin, int* req_histogram, unsigned char *key) {
	int *saved_histogram;
	static int empty_hist[HISTOGRAM_LEN * NBLOCKS] = { 0 };
#ifdef PAVEL_APTR
	static int hist_from_hash[HISTOGRAM_LEN * NBLOCKS];
#endif
	float distance;
	//calc_histogram(histogram, img, pattern2bin);
	hash_get(hashmap, key, (unsigned char **)&saved_histogram);

	if (saved_histogram) {
#ifdef PAVEL_APTR
		Aptr<int> ptr = Aptr<int>(saved_histogram, sizeof(int) * HISTOGRAM_LEN * NBLOCKS, 0);
		memmove_reg_aptr(hist_from_hash, &ptr, sizeof(int) * HISTOGRAM_LEN * NBLOCKS);
		distance = histogram_distance(req_histogram, hist_from_hash);
#else
		//memmove(hist_from_hash, saved_histogram, sizeof(int) * HISTOGRAM_LEN * NBLOCKS);

		distance = histogram_distance(req_histogram, saved_histogram);
#endif
	}
	else
	{
		debug("ERROR: Invalid person ID recieved\n");
		distance = histogram_distance(req_histogram, empty_hist);
	}

	return distance;
}

int populate_database(struct hash_t *hashmap, int *pattern2bin) {
	int cnt = 0;
	static int histogram[HISTOGRAM_LEN * NBLOCKS];

	char *img = (char*)my_malloc(IMG_WIDTH * IMG_WIDTH);
	
	int retVal = 0;
	ocall_FindFirstFile(&retVal);
	if (retVal == -1) {
		debug("error opening dir\n");
		abort();
	}

	size_t already_added_size = 1024 * DUP_NUM;
	char *already_added = (char*)my_malloc(already_added_size);
	//struct dirent *dent; ocall_readdir((void**)&dent, d);
	unsigned int max = 0;
	int max_hash = 0;
	
	//while (dent)
	do {
		unsigned char *key;
		ocall_GetKey(&key);
		//key = (unsigned char*)strtok(dent->d_name, "_");
		unsigned int key_int = (unsigned int)atoi((char *)key); 
		if (key_int > max) {
			max = key_int;
		}

		if (hash_function(key, 1024) > max_hash) {
			max_hash = hash_function(key, 1024);
		}

		if (key_int >= already_added_size) {
			already_added = (char*)my_realloc(already_added, already_added_size, already_added_size * 2);
			already_added_size *= 2;
		}

		if (already_added[key_int]) {
			ocall_FindNextFile(&retVal);
			if (retVal != 0) {
				continue;
			}
			else {
				break;
			}
		}

		already_added[key_int] = 1;
		ocall_readfile(&retVal, img, IMG_WIDTH * IMG_WIDTH);
		
		/*
		ocall_open(&fd, fname, O_RDONLY);
		if (fd < 0) {
			debug("%d::%s\n", __LINE__, fname);
			abort();
		}
		n = 0;
		while (n < IMG_WIDTH * IMG_WIDTH) {
			ocall_read(&bytes_read, fd, img + n, IMG_WIDTH * IMG_WIDTH - n);
			if (bytes_read > 0) {
				n += (int)bytes_read;
			}
			else {
				//perror("image read");
				debug("%d::%s\n", __LINE__, fname);
				abort();
				//exit(1);
			}
		}
		ocall_close(&ret, fd);
		*/

		calc_histogram(histogram, to_mat(img), pattern2bin);
		//        hashmap_put(hashmap, key, (unsigned char *)histogram, sizeof(int) * HISTOGRAM_LEN * NBLOCKS, 0);

		for (int x = 0; x<DUP_NUM; x++) {
			char tmp[40];
			snprintf(tmp, 40, "%s%d", key, x);
			hash_put(hashmap, /*key*/ (unsigned char*)tmp, (unsigned char *)histogram, 0);
			cnt++;
#ifdef MAX_DB_IMAGES
			if (cnt == MAX_DB_IMAGES)
				break;
#endif
		}
		
		ocall_FindNextFile(&retVal);
	} while (retVal != 0);

	int total_db_size_bytes = hashmap->total_size;
	total_db_size_bytes = (int)hashmap->nentries * sizeof(int) * HISTOGRAM_LEN*NBLOCKS;
	debug("total DB size %d [B]  %f.1 [MB]\n", total_db_size_bytes, (float)total_db_size_bytes / (1024 * 1024));

	debug("number of entries %d \n", (int)hashmap->nentries);

	free(img);
	free(already_added);
	return 0;
}

int ecall_train_server(void* _request_queue, void* _response_queue)
//void face_verification_prepare_server(void* _request_queue, void* _response_queue)
{
	g_request_queue = (Queue*)_request_queue;
	g_response_queue = (Queue*)_response_queue;

	create_pattern2bin(pattern2bin);

	hash_init(&hash);

	debug("populating database...\n");
	populate_database(&hash, pattern2bin);
	debug("done populating database\n");

	return 0;
}

int ecall_open_server()
//int workload_face_verification(void* dummy)
{
	unsigned char key[1024];
	//char img[IMG_WIDTH * IMG_WIDTH]; memset(img, 0, IMG_WIDTH*IMG_WIDTH);
	int i = 0;
	//int *histogram = (int*)my_malloc(sizeof(int) * HISTOGRAM_LEN * NBLOCKS);

	unsigned int data_size = (unsigned int)(KEY_LENGTH_BYTES + HISTOGRAM_LENGTH_BYTES);
	unsigned char* decrypted_data_buffer = (unsigned char*)malloc(data_size);

	debug("starting worker...\n");
	unsigned char* data = NULL;
	while (1) {

		request* req = g_request_queue->dequeue();
		if (req == NULL)
		{
			__asm
			{
				pause
			}
			continue;
		}

		s_server_data* server_data = (s_server_data*)req->buffer;
		unsigned char* data = (unsigned char*)server_data->_buffer;
		int client_id = *(int*)(data);
		//decrypted_data_buffer = (unsigned char*)data+sizeof(int);

		sgx_status_t y = sgx_aes_ctr_decrypt(&aes_key,
			data + sizeof(int),
			data_size,
			&ctr_dec[client_id * 16],
			8,
			decrypted_data_buffer);
		if (y != 0)
		{
			debug("failed decrypt\n"); abort();
		}

		memcpy(key, decrypted_data_buffer, KEY_LENGTH_BYTES);
		char* histogram = (char*)decrypted_data_buffer + KEY_LENGTH_BYTES;
		float distance = do_one(&hash, pattern2bin, (int*)histogram, key);
		int answer = 0;
		if (distance < ACCEPT_THRESHOLD) answer = 1;

		int encrypted_answer;
		y = sgx_aes_ctr_encrypt(&aes_key,
			(unsigned char*)&answer,
			sizeof(int),
			&ctr_enc[client_id * 16],
			8,
			(unsigned char*)&encrypted_answer);
		if (y != 0)
		{
			debug("failed encrypt\n"); abort();
		}

		request* response = server_data->_response;
		response->is_done = encrypted_answer;
		g_response_queue->enqueue(response);
	}

	return 0;
}