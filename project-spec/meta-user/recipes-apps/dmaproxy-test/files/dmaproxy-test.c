#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <dmaproxy/dmaproxy.h>

#define INP_TDATA_WIDTH_BYTES 1
#define OUT_TDATA_WIDTH_BYTES 1

/* Struct used to describe a single DMA channel */
struct channel {
	struct channel_buffer *buf_ptr;	/* Pointer to channel buffer, which is the buffer in kernel space memory that holds data for DMA transferring/receiving */
	int fd;							/* Channel device file descriptor */
	pthread_t tid;					/* Channel thread id */
};

/* Struct used to describe a single node of a dynamically allocated packet queue of sent neuro FPGA packets */
struct tx_pkt_queue_node {
	uint8_t pkt[INP_TDATA_WIDTH_BYTES];
	struct tx_pkt_queue_node *next;
};

/* Struct used to describe a single node of a dynamically allocated packet queue of received neuro FPGA packets */
struct rx_pkt_queue_node {
	uint8_t pkt[OUT_TDATA_WIDTH_BYTES];
	struct rx_pkt_queue_node *next;
};


/* Struct used to describe dynamically allocated queue of sent neuro FPGA packets */
struct tx_pkt_queue {
	struct tx_pkt_queue_node *front;
	struct tx_pkt_queue_node *back;
};

/* Struct used to describe dynamically allocated queue of received neuro FPGA packets */
struct rx_pkt_queue {
	struct rx_pkt_queue_node *front;
	struct rx_pkt_queue_node *back;
};

/* Global constants */
const char TX_CHANNEL_NAME[] = "/dev/dmaproxy_tx";
const char RX_CHANNEL_NAME[] = "/dev/dmaproxy_rx";

/* Global non-constants */
static volatile int stop = 0;
static pthread_mutex_t tx_pkt_queue_lock;
static pthread_mutex_t rx_pkt_queue_lock;
static pthread_mutexattr_t tx_pkt_queue_lock_attr;
static pthread_mutexattr_t rx_pkt_queue_lock_attr;
static pthread_attr_t tattr_tx;
static pthread_attr_t tattr_rx;
static struct channel tx_channel;
static struct channel rx_channel;
static struct tx_pkt_queue tx_pkt_queue;
static struct rx_pkt_queue rx_pkt_queue;

/*******************************************************************************************************************/
/* Get the clock time in usecs to allow performance testing
 */
static uint64_t get_posix_clock_time_usec ()
{
    struct timespec ts;

    if (clock_gettime (CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t) (ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
    else
        return 0;
}

/*******************************************************************************************************************/
/*
 * Returns 1 if queue of packets to send to neuro FPGA is empty, 0 if not empty, and -1 on error.
 */
int is_tx_pkt_queue_empty() {
	int ret;

	if(pthread_mutex_lock(&tx_pkt_queue_lock) != 0) {
		return -1;
	}

	ret = (tx_pkt_queue.front == NULL);

	if(pthread_mutex_unlock(&tx_pkt_queue_lock) != 0) {
		return -1;
	}

	return ret;
}

/*******************************************************************************************************************/
/*
 * Push a packet onto the back of the queue of packets to send to neuro FPGA. Returns 0 on success and -1 on failure.
 */
int tx_pkt_queue_push(uint8_t *pkt) {
	struct tx_pkt_queue_node *new_node;
	unsigned int i;

	if(stop) {
		return -1;
	}

	if(pthread_mutex_lock(&tx_pkt_queue_lock) != 0) {
		return -1;
	}

	new_node = (struct tx_pkt_queue_node *) malloc(sizeof(struct tx_pkt_queue_node));
	if(new_node == NULL) {
		pthread_mutex_unlock(&tx_pkt_queue_lock);
		return -1;
	}

	for(i = 0; i < INP_TDATA_WIDTH_BYTES; i++) {
		new_node->pkt[i] = pkt[i];
	}

	if(is_tx_pkt_queue_empty() == 0) {
		tx_pkt_queue.back->next = new_node;
	} else {
		tx_pkt_queue.front = new_node;
	}

	tx_pkt_queue.back = new_node;

	if(pthread_mutex_unlock(&tx_pkt_queue_lock) != 0) {
		return -1;
	}

	return 0;
}

/*******************************************************************************************************************/
/*
 * Pop a packet from the front of the queue of packets to send to neuro FPGA. Returns 0 on success and -1 on failure.
 */
int tx_pkt_queue_pop(uint8_t *pkt) {
	struct tx_pkt_queue_node *node_to_delete;
	unsigned int i;

	if(pthread_mutex_lock(&tx_pkt_queue_lock) != 0) {
		return -1;
	}

	if(is_tx_pkt_queue_empty() != 0) {
		pthread_mutex_unlock(&tx_pkt_queue_lock);
		return -1;
	}

	node_to_delete = tx_pkt_queue.front;
	for(i = 0; i < INP_TDATA_WIDTH_BYTES; i++) {
		pkt[i] = node_to_delete->pkt[i];
	}

	if(tx_pkt_queue.front != tx_pkt_queue.back) {
		tx_pkt_queue.front = node_to_delete->next;
	} else {
		tx_pkt_queue.front = NULL;
		tx_pkt_queue.back = NULL;
	}
	
	free(node_to_delete);

	if(pthread_mutex_unlock(&tx_pkt_queue_lock) != 0) {
		return -1;
	}

	return 0;
}

/*******************************************************************************************************************/
/*
 * Returns 1 if queue of packets to receive from neuro FPGA is empty, 0 if not empty, and -1 on error.
 */
int is_rx_pkt_queue_empty() {
	int ret;

	if(pthread_mutex_lock(&rx_pkt_queue_lock) != 0) {
		return -1;
	}

	ret = (rx_pkt_queue.front == NULL);

	if(pthread_mutex_unlock(&rx_pkt_queue_lock) != 0) {
		return -1;
	}

	return ret;
}

/*******************************************************************************************************************/
/*
 * Push a packet onto the back of the queue of packets to receive from neuro FPGA. Returns 0 on success and -1 on failure.
 */
int rx_pkt_queue_push(uint8_t *pkt) {
	struct rx_pkt_queue_node *new_node;
	unsigned int i;

	if(stop) {
		return -1;
	}

	if(pthread_mutex_lock(&rx_pkt_queue_lock) != 0) {
		return -1;
	}

	new_node = (struct rx_pkt_queue_node *) malloc(sizeof(struct rx_pkt_queue_node));
	if(new_node == NULL) {
		pthread_mutex_unlock(&rx_pkt_queue_lock);
		return -1;
	}

	for(i = 0; i < OUT_TDATA_WIDTH_BYTES; i++) {
		new_node->pkt[i] = pkt[i];
	}

	if(is_rx_pkt_queue_empty() == 0) {
		rx_pkt_queue.back->next = new_node;
	} else {
		rx_pkt_queue.front = new_node;
	}

	rx_pkt_queue.back = new_node;

	if(pthread_mutex_unlock(&rx_pkt_queue_lock) != 0) {
		return -1;
	}

	return 0;
}

/*******************************************************************************************************************/
/*
 * Pop a packet from the front of the queue of packets to receive from neuro FPGA. Returns 0 on success and -1 on failure.
 */
int rx_pkt_queue_pop(uint8_t *pkt) {
	struct rx_pkt_queue_node *node_to_delete;
	unsigned int i;

	if(pthread_mutex_lock(&rx_pkt_queue_lock) != 0) {
		return -1;
	}

	if(is_rx_pkt_queue_empty() != 0) {
		pthread_mutex_unlock(&rx_pkt_queue_lock);
		return -1;
	}

	node_to_delete = rx_pkt_queue.front;
	for(i = 0; i < OUT_TDATA_WIDTH_BYTES; i++) {
		pkt[i] = node_to_delete->pkt[i];
	}

	if(rx_pkt_queue.front != rx_pkt_queue.back) {
		rx_pkt_queue.front = node_to_delete->next;
	} else {
		rx_pkt_queue.front = NULL;
		rx_pkt_queue.back = NULL;
	}
	
	free(node_to_delete);

	if(pthread_mutex_unlock(&rx_pkt_queue_lock) != 0) {
		return -1;
	}

	return 0;
}

/*******************************************************************************************************************/
/*
 * Thread for DMA tx channel.
 */
void *tx_thread(struct channel *channel_ptr) {
	int buffer_id;
	unsigned int buffer_ind;
	unsigned int pkt_cnt;
	unsigned int bytes_in_buff_elmnt;
	unsigned int h;
	unsigned int i;
	uint8_t pkt[INP_TDATA_WIDTH_BYTES];
	uint8_t is_buffer_active[TX_BUFFER_COUNT] = {0};

	/* Do DMA tx channel transfers of neuro FPGA packets until the user says to stop */
	while(stop == 0) {
		for(buffer_id = 0; buffer_id < TX_BUFFER_COUNT; buffer_id += 1) {

			/* If the current buffer has a DMA transfer active, finish it first (blocks thread) */
			if(is_buffer_active[buffer_id]) {
				ioctl(channel_ptr->fd, FINISH_XFER, &buffer_id);
				if(channel_ptr->buf_ptr[buffer_id].status != PROXY_NO_ERROR) {
					printf("DMA neuro FPGA tx transfer error at buffer_id=%d\n", buffer_id);
				}
				is_buffer_active[buffer_id] = 0;
			}

			/* Reset buffer data to 0 */
			for(i = 0; i < BUFFER_SIZE / sizeof(unsigned int); i++) {
				channel_ptr->buf_ptr[buffer_id].buffer[i] = 0;
			}

			/* If there are any packets on the queue, copy the packet to the current DMA buffer and start a DMA transfer */
			buffer_ind = 0;
			pkt_cnt = 0;
			bytes_in_buff_elmnt = 0;
			while(is_tx_pkt_queue_empty() == 0 && (pkt_cnt+1) * INP_TDATA_WIDTH_BYTES <= BUFFER_SIZE) {
				tx_pkt_queue_pop(pkt);

				/*
				INP_TDATA_WIDTH_BYTES is number of bytes in pkt; unlike UART neuro FPGA, INP_TDATA_WIDTH_BYTES must be one of the following due to AXI DMA IP AXIS tdata width constraints: 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024 

				Case (INP_TDATA_WIDTH_BYTES == 4):
					This means each pkt fits perfectly into one element of the buffer array.
					
					for(i = 0; i < 4; i++) {
						buffer[buffer_ind] = pkt[i] << i*8;
					}
					buffer_ind++;


				Case (INP_TDATA_WIDTH_BYTES < 4):
					This means more than one pkt must be placed into one element of the buffer array. The limits on AXIS tdata width ensure that an integer number of pkts can be placed into one element of the buffer array.
					
					for(i = 0; i < INP_TDATA_WIDTH_BYTES; i++) {
						buffer[buffer_ind] = pkt[i] << bytes_in_buff_elmnt*8;
					}
					bytes_in_buff_elmnt += INP_TDATA_WIDTH_BYTES;
					if(bytes_in_buff_elmnt >= 4) {
						buffer_ind++;
						bytes_in_buff_elmnt = 0;
					}


				Case (INP_TDATA_WIDTH_BYTES > 4):
					This means less than one pkt must be placed into one element of the buffer array. The limits on AXIS tdata width ensure that an integer number of buffer array elements can perfectly hold one pkt.

					for(h = 0; h < INP_TDATA_WIDTH_BYTES/4; h++) {
						for(i = 0; i < 4; i++) {
							buffer[buffer_ind] = pkt[h*4+i] << i*8;
						}
						buffer_ind++;
					}

				*/

				if(INP_TDATA_WIDTH_BYTES <= sizeof(unsigned int)) {

					/* Put one or more data packets into a single buffer element */
					for(i = 0; i < INP_TDATA_WIDTH_BYTES; i++) {
						printf("buffer_ind=%d, i=%d, bytes_in_buff_elmnt=%d\n", buffer_ind, i, bytes_in_buff_elmnt);
						channel_ptr->buf_ptr[buffer_id].buffer[buffer_ind] |= pkt[i] << (bytes_in_buff_elmnt*8);
					}
					bytes_in_buff_elmnt += INP_TDATA_WIDTH_BYTES;
					if(bytes_in_buff_elmnt >= sizeof(unsigned int)) {
						buffer_ind++;
						bytes_in_buff_elmnt = 0;
					}

				} else {
					
					/* Use multiple buffer elements for a single data packet */
					for(h = 0; h < INP_TDATA_WIDTH_BYTES/sizeof(unsigned int); h++) {
						for(i = 0; i < sizeof(unsigned int); i++) {
							printf("buffer_ind=%d, h=%d, i=%d, pkt index = %d\n", buffer_ind, h, i, h * sizeof(unsigned int) + i);
							channel_ptr->buf_ptr[buffer_id].buffer[buffer_ind] |= pkt[h * sizeof(unsigned int) + i] << (i*8);
						}
						buffer_ind++;
					}
				}

				pkt_cnt++;
			}

			if(pkt_cnt > 0) {
				channel_ptr->buf_ptr[buffer_id].length = pkt_cnt * INP_TDATA_WIDTH_BYTES;
				ioctl(channel_ptr->fd, START_XFER, &buffer_id);
				is_buffer_active[buffer_id] = 1;
			}
		}
	}

	/* Finish remaining DMA tx channel transfers */
	for(buffer_id = 0; buffer_id < TX_BUFFER_COUNT; buffer_id += 1) {
		if(is_buffer_active[buffer_id]) {
			ioctl(channel_ptr->fd, FINISH_XFER, &buffer_id);
			if(channel_ptr->buf_ptr[buffer_id].status != PROXY_NO_ERROR) {
				printf("DMA neuro FPGA tx transfer error at buffer_id=%d\n", buffer_id);
			}
		}
	}
}

/*******************************************************************************************************************/
/*
 * Thread for DMA rx channel.
 */
void *rx_thread(struct channel *channel_ptr) {
	int buffer_id;
	unsigned int pkt_byte;
	unsigned int i;
	unsigned int j;
	unsigned int k;
	uint8_t pkt[OUT_TDATA_WIDTH_BYTES];
	
	/* Initialize every rx buffer by writing the DMA transfer size (equal to the neuro FPGA packet size in bytes) to its buffer table and starting a DMA transfer for the buffer*/
	for(buffer_id = 0; buffer_id < RX_BUFFER_COUNT; buffer_id += 1) {
		channel_ptr->buf_ptr[buffer_id].length = BUFFER_SIZE;
		ioctl(channel_ptr->fd, START_XFER, &buffer_id);
	}

	while(stop == 0) {
		for(buffer_id = 0; buffer_id < RX_BUFFER_COUNT; buffer_id++) {

			/* Finish DMA transfer for the current buffer (blocking) */
			ioctl(channel_ptr->fd, FINISH_XFER, &buffer_id);
			if(channel_ptr->buf_ptr[buffer_id].status == PROXY_ERROR) {
				printf("DMA neuro FPGA rx transfer error at buffer_id=%d\n", buffer_id);
			}

			/* If the finished DMA transfer completed without timeout or error, add the received packet to the rx queue */
			if(channel_ptr->buf_ptr[buffer_id].status == PROXY_NO_ERROR) {
				pkt_byte = 0;
				for(i = 0; i < BUFFER_SIZE / sizeof(unsigned int); i++) {

					/*
					Case (OUT_TDATA_WIDTH_BYTES == 4):
						One pkt in one buffer entry

						for(j = 0; j < 4; j++) {
							pkt[j] = (buffer[i] >> (j*8)) & 0xFF;
						}
						rx_pkt_queue_push(pkt);


					Case (OUT_TDATA_WIDTH_BYTES < 4):
						Multiple pkts in one buffer entry

						for(j = 0; j < 4 / OUT_TDATA_WIDTH_BYTES; j++) {
							for(k = 0; k < OUT_TDATA_WIDTH_BYTES; k++) {
								pkt[k] = (buffer[i] >> (j*OUT_TDATA_WIDTH_BYTES*8 + k*8)) & 0xFF;
							}
							rx_pkt_queue_push(pkt);
						}

					
					Case (OUT_TDATA_WIDTH_BYTES > 4):
						One pkt in multiple buffer entries

						for(j = 0; j < 4; j++) {
							pkt[pkt_byte] = (buffer[i] >> (j*8)) & 0xFF;
							pkt_byte++;
						}
						if(pkt_byte >= OUT_TDATA_WIDTH_BYTES) {
							rx_pkt_queue_push(pkt);
							pkt_byte = 0;
						}
					*/

					printf("buffer[i]=%08x\n", channel_ptr->buf_ptr[buffer_id].buffer[i]);

					if(OUT_TDATA_WIDTH_BYTES <= sizeof(unsigned int)) {

						/* Use one buffer element for one or more data packets */
						for(j = 0; j < sizeof(unsigned int) / OUT_TDATA_WIDTH_BYTES; j++) {
							for(k = 0; k < OUT_TDATA_WIDTH_BYTES; k++) {
								printf("i=%d, j=%d, k=%d, right shift amount = %d\n", i, j, k, j*OUT_TDATA_WIDTH_BYTES*8 + k*8);
								pkt[k] = (channel_ptr->buf_ptr[buffer_id].buffer[i] >> (j*OUT_TDATA_WIDTH_BYTES*8 + k*8)) & 0xFF;
							}
							rx_pkt_queue_push(pkt);
						}

					} else {

						/* Use multiple buffer elements for one data packet */
						for(j = 0; j < sizeof(unsigned int); j++) {
							printf("pkt_byte=%d, i=%d, j=%d, right shift amount = %d\n", pkt_byte, i, j, j*8);
							pkt[pkt_byte] = (channel_ptr->buf_ptr[buffer_id].buffer[i] >> (j*8)) & 0xFF;
							pkt_byte++;
						}
						if(pkt_byte >= OUT_TDATA_WIDTH_BYTES) {
							rx_pkt_queue_push(pkt);
							pkt_byte = 0;
						}
					}
				}
			}

			if(channel_ptr->buf_ptr[buffer_id].status == PROXY_TIMEOUT) {
				printf("DMA neuro FPGA rx transfer timeout at buffer_id=%d\n", buffer_id);
			}

			/* Start new DMA transfer for this buffer */
			ioctl(channel_ptr->fd, START_XFER, &buffer_id);
		}
	}

	/* Finish remaining DMA rx channel transfers */
	for(buffer_id = 0; buffer_id < RX_BUFFER_COUNT; buffer_id++) {

		/* Finish DMA transfer for the current buffer (blocking) */
		ioctl(channel_ptr->fd, FINISH_XFER, &buffer_id);
		if(channel_ptr->buf_ptr[buffer_id].status == PROXY_ERROR) {
			printf("DMA neuro FPGA rx transfer error at buffer_id=%d\n", buffer_id);
		}

		/* If the finished DMA transfer completed without timeout or error, add the received packet to the rx queue */
		if(channel_ptr->buf_ptr[buffer_id].status == PROXY_NO_ERROR) {
			pkt_byte = 0;
			for(i = 0; i < BUFFER_SIZE / sizeof(unsigned int); i++) {

				if(OUT_TDATA_WIDTH_BYTES <= sizeof(unsigned int)) {

					/* Use one buffer element for one or more data packets */
					for(j = 0; j < sizeof(unsigned int) / OUT_TDATA_WIDTH_BYTES; j++) {
						for(k = 0; k < OUT_TDATA_WIDTH_BYTES; k++) {
							printf("i=%d, j=%d, k=%d, right shift amount = %d\n", i, j, k, j*OUT_TDATA_WIDTH_BYTES*8 + k*8);
							pkt[k] = (channel_ptr->buf_ptr[buffer_id].buffer[i] >> (j*OUT_TDATA_WIDTH_BYTES*8 + k*8)) & 0xFF;
						}
						rx_pkt_queue_push(pkt);
					}

				} else {

					/* Use multiple buffer elements for one data packet */
					for(j = 0; j < sizeof(unsigned int); j++) {
						printf("pkt_byte=%d, i=%d, j=%d, right shift amount = %d\n", pkt_byte, i, j, j*8);
						pkt[pkt_byte] = (channel_ptr->buf_ptr[buffer_id].buffer[i] >> (j*8)) & 0xFF;
						pkt_byte++;
					}
					if(pkt_byte >= OUT_TDATA_WIDTH_BYTES) {
						rx_pkt_queue_push(pkt);
						pkt_byte = 0;
					}
				}
			}
		}

		if(channel_ptr->buf_ptr[buffer_id].status == PROXY_TIMEOUT) {
			printf("DMA neuro FPGA rx transfer timeout at buffer_id=%d\n", buffer_id);
		}
	}
}

/*******************************************************************************************************************/
/*
 * Setup one thread for DMA tx channel and one thread for DMA rx channel. Tx channel thread has lower priority than
 * rx channel thread to relieve backpressure on rx side of the interface. Returns 0 on success and -1 on failure.
 */
int setup_threads() {
	struct sched_param sched_param_tx;

	/* Make tx channel's thread lower priority (via a high priority int value) than rx channel's thread */
	if(pthread_attr_init(&tattr_tx) != 0) {
		printf("Failed to initialize DMA tx channel's pthread attributes\n");
		return -1;
	}
	if(pthread_attr_setschedpolicy(&tattr_tx, SCHED_RR)) {
		printf("Failed to set DMA tx channel's pthread scheduling policy\n");
		return -1;
	}
	if(pthread_attr_getschedparam(&tattr_tx, &sched_param_tx) != 0) {
		printf("Failed to get DMA tx channel's pthread scheduling attributes\n");
		return -1;
	}
	(sched_param_tx.sched_priority)++;
	if(pthread_attr_setschedparam(&tattr_tx, &sched_param_tx) != 0) {
		printf("Failed to set DMA tx channel's pthread scheduling attributes\n");
		return -1;
	}
	if(pthread_attr_init(&tattr_rx) != 0) {
		printf("Failed to initialize DMA rx channel's pthread attributes\n");
		return -1;
	}
	if(pthread_attr_setschedpolicy(&tattr_rx, SCHED_RR)) {
		printf("Failed to set DMA rx channel's pthread scheduling policy\n");
		return -1;
	}

	/* Create one thread for rx channel and one for tx channel */
	if(pthread_create(&rx_channel.tid, &tattr_rx, rx_thread, (void *)&rx_channel) != 0) {
		printf("Failed to create thread for DMA rx channel\n");
		return -1;
	}
	if(pthread_create(&tx_channel.tid, &tattr_tx, tx_thread, (void *)&tx_channel) != 0) {
		printf("Failed to create thread for DMA tx channel\n");
		return -1;
	}

	return 0;
}

/*******************************************************************************************************************/
/*
 * Setup one DMA tx channel and one rx channel; for each, open its corresponding device file and map necessary
 * kernel driver memory into user space. Returns 0 on success and -1 on failure.
 */
int setup_channels() {
	/* Open DMA tx channel file descriptor */
	tx_channel.fd = open(TX_CHANNEL_NAME, O_RDWR);
	if(tx_channel.fd < 1) {
		printf("Unable to open neuro FPGA DMA tx channel device file: %s\n", TX_CHANNEL_NAME);
		return -1;
	}

	/* Map DMA tx channel kernel driver memory into user space */
	tx_channel.buf_ptr = (struct channel_buffer *)mmap(NULL, sizeof(struct channel_buffer) * TX_BUFFER_COUNT, PROT_READ | PROT_WRITE, MAP_SHARED, tx_channel.fd, 0);
	if(tx_channel.buf_ptr == MAP_FAILED) {
		printf("Failed to mmap DMA tx channel kernel driver memory into user space\n");
		return -1;
	}

	/* Open DMA rx channel file descriptor */
	rx_channel.fd = open(RX_CHANNEL_NAME, O_RDWR);
	if(rx_channel.fd < 1) {
		printf("Unable to open neuro FPGA DMA rx channel device file: %s\n", RX_CHANNEL_NAME);
		return -1;
	}

	/* Map DMA rx channel kernel driver memory into user space */
	rx_channel.buf_ptr = (struct channel_buffer *)mmap(NULL, sizeof(struct channel_buffer) * RX_BUFFER_COUNT, PROT_READ | PROT_WRITE, MAP_SHARED, rx_channel.fd, 0);
	if(rx_channel.buf_ptr == MAP_FAILED) {
		printf("Failed to mmap DMA rx channel kernel driver memory into user space\n");
		return -1;
	}

	return 0;
}

/*******************************************************************************************************************/
/*
 * Setup queue of packets to be sent to neuro FPGA. Returns 0 on success and -1 on failure.
 */
int setup_tx_pkt_queue() {
	/* Init queue members */
	tx_pkt_queue.front = NULL;
	tx_pkt_queue.back = NULL;

	/* Init queue mutex and set its type to recursive */
	if(pthread_mutexattr_init(&tx_pkt_queue_lock_attr) != 0) {
		printf("Falied to initialize mutex attributes for queue of packets to be sent to neuro FPGA\n");
		return -1;
	}
	if(pthread_mutexattr_settype(&tx_pkt_queue_lock_attr, PTHREAD_MUTEX_RECURSIVE_NP) != 0) {
		printf("Failed to set type of mutex for queue of packets to be sent to neuro FPGA\n");
		return -1;
	}
	if(pthread_mutex_init(&tx_pkt_queue_lock, &tx_pkt_queue_lock_attr) != 0) {
		printf("Failed to initialize mutex for queue of packets to be sent to neuro FPGA\n");
		return -1;
	}

	return 0;
}

/*******************************************************************************************************************/
/*
 * Setup queue of packets to receive from neuro FPGA. Returns 0 on success and -1 on failure.
 */
int setup_rx_pkt_queue() {
	/* Init queue members */
	rx_pkt_queue.front = NULL;
	rx_pkt_queue.back = NULL;

	/* Init queue mutex and set its type to recursive */
	if(pthread_mutexattr_init(&rx_pkt_queue_lock_attr) != 0) {
		printf("Falied to initialize mutex attributes for queue of packets to be received from neuro FPGA\n");
		return -1;
	}
	if(pthread_mutexattr_settype(&rx_pkt_queue_lock_attr, PTHREAD_MUTEX_RECURSIVE_NP) != 0) {
		printf("Failed to set type of mutex for queue of packets to be received from neuro FPGA\n");
		return -1;
	}
	if(pthread_mutex_init(&rx_pkt_queue_lock, &rx_pkt_queue_lock_attr) != 0) {
		printf("Failed to initialize mutex for queue of packets to be received from neuro FPGA\n");
		return -1;
	}

	return 0;
}

/*******************************************************************************************************************/
/*
 * Setup neuro fpga DMA device and data structures. Returns 0 on success and -1 on failure.
 */
int setup_neuro_fpga_dma() {
	printf("Setting up queues to hold packets of data to be sent to and received from neuro FPGA\n");

	if(setup_tx_pkt_queue() != 0) {
		return -1;
	}

	if(setup_rx_pkt_queue() != 0) {
		return -1;
	}

	printf("Setting up neuro FPGA DMA rx and tx channels\n");

	if(setup_channels() != 0) {
		return -1;
	}

	printf("Setting up neuro FPGA DMA rx and tx channels' threads\n");

	if(setup_threads() != 0) {
		return -1;
	}

	return 0;
}

/*******************************************************************************************************************/
/*
 * Clean up threads for DMA rx and tx channels via join. Returns 0 on success and -1 on failure.
 */
int cleanup_threads() {
	if(pthread_join(tx_channel.tid, NULL)) {
		printf("Failed to join thread for DMA tx channel\n");
		return -1;
	}
	if(pthread_join(rx_channel.tid, NULL)) {
		printf("Failed to join thread for DMA rx channel\n");
		return -1;
	}

	if(pthread_attr_destroy(&tattr_tx) != 0) {
		printf("Failed to destroy DMA tx channel pthread attributes\n");
		return -1;
	}
	if(pthread_attr_destroy(&tattr_rx) != 0) {
		printf("Failed to destroy DMA rx channel pthread attributes\n");
		return -1;
	}

	return 0;
}

/*******************************************************************************************************************/
/*
 * Clean up DMA rx and tx channels. Returns 0 on success and -1 on failure.
 */
int cleanup_channels() {
	/* Unmap DMA tx channel kernel driver memory into user space */
	if(munmap(tx_channel.buf_ptr, sizeof(struct channel_buffer)) != 0) {
		printf("Failed to unmap DMA tx channel kernel driver memory from user space\n");
		return -1;
	}

	/* Close DMA tx channel file descriptor */
	if(close(tx_channel.fd) != 0) {
		printf("Unable to close neuro FPGA DMA tx channel device file: %s\n", TX_CHANNEL_NAME);
		return -1;
	}

	/* Unmap DMA rx channel kernel driver memory into user space */
	if(munmap(rx_channel.buf_ptr, sizeof(struct channel_buffer)) != 0) {
		printf("Failed to unmap DMA rx channel kernel driver memory from user space\n");
		return -1;
	}

	/* Close DMA rx channel file descriptor */
	if(close(rx_channel.fd) != 0) {
		printf("Unable to close neuro FPGA DMA rx channel device file: %s\n", RX_CHANNEL_NAME);
		return -1;
	}

	return 0;
}

/*******************************************************************************************************************/
/*
 * Clean up queue of packets to send to neuro FPGA by freeing its memory and destroying both its mutex and mutex
 * attributes. Returns 0 on success and -1 on failure.
 */
int cleanup_tx_pkt_queue() {
	uint8_t pkt[INP_TDATA_WIDTH_BYTES];

	while(is_tx_pkt_queue_empty() == 0) {
		tx_pkt_queue_pop(pkt);
	}

	if(pthread_mutex_destroy(&tx_pkt_queue_lock) != 0) {
		printf("Failed to destroy mutex for queue of packets to be sent to neuro FPGA\n");
		return -1;
	}
	if(pthread_mutexattr_destroy(&tx_pkt_queue_lock_attr) != 0) {
		printf("Failed to destroy mutex attributes for queue of packets to be sent to neuro FPGA\n");
		return -1;
	}

	return 0;
}

/*******************************************************************************************************************/
/*
 * Clean up queue of packets to receive from neuro FPGA by freeing its memory and destroying both its mutex and mutex
 * attributes. Returns 0 on success and -1 on failure.
 */
int cleanup_rx_pkt_queue() {
	uint8_t pkt[OUT_TDATA_WIDTH_BYTES];

	while(is_rx_pkt_queue_empty() == 0) {
		rx_pkt_queue_pop(pkt);
	}

	if(pthread_mutex_destroy(&rx_pkt_queue_lock) != 0) {
		printf("Failed to destroy mutex for queue of packets to be received from neuro FPGA\n");
		return -1;
	}
	if(pthread_mutexattr_destroy(&rx_pkt_queue_lock_attr) != 0) {
		printf("Failed to destroy mutex attributes for queue of packets to be received from neuro FPGA\n");
		return -1;
	}

	return 0;
}

/*******************************************************************************************************************/
/*
 * Clean up neuro fpga DMA device and data structures. Returns 0 on success and -1 on failure.
 */
int cleanup_neuro_fpga_dma() {
	stop = 1;

	printf("Cleaning up neuro FPGA DMA rx and tx channels' threads\n");

	if(cleanup_threads() != 0) {
		return -1;
	}

	printf("Cleaning up neuro FPGA DMA rx and tx channels\n");

	if(cleanup_channels() != 0) {
		return -1;
	}
	
	printf("Cleaning up queues to hold packets of data to be sent to and received from neuro FPGA\n");

	if(cleanup_tx_pkt_queue() != 0) {
		return -1;
	}

	if(cleanup_rx_pkt_queue() != 0) {
		return -1;
	}

	return 0;
}

int main(int argc, char *argv[]) {
	unsigned int i;
	unsigned int j;
	uint8_t tx_pkt[INP_TDATA_WIDTH_BYTES];
	uint8_t rx_pkt[OUT_TDATA_WIDTH_BYTES];
	uint64_t start_time;
	uint64_t end_time;

	const unsigned int NUM_TRANSFERS = 5;

	printf("Neuro FPGA DMA Test\n\n");

	if(setup_neuro_fpga_dma() != 0) {
		return EXIT_FAILURE;
	}

	printf("Adding packets to tx queue\n");

	start_time = get_posix_clock_time_usec();

	tx_pkt[0] = 0x08;
	tx_pkt_queue_push(tx_pkt);

	tx_pkt[0] = 0x20;
	tx_pkt_queue_push(tx_pkt);

	tx_pkt[0] = 0x28;
	tx_pkt_queue_push(tx_pkt);

	// tx_pkt[0] = 0x00;
	// tx_pkt[1] = 0x00;
	// tx_pkt[2] = 0x00;
	// tx_pkt[3] = 0x08;
	// tx_pkt_queue_push(tx_pkt);

	// tx_pkt[0] = 0x00;
	// tx_pkt[1] = 0x00;
	// tx_pkt[2] = 0x00;
	// tx_pkt[3] = 0x08;
	// tx_pkt_queue_push(tx_pkt);

	// tx_pkt[0] = 0x00;
	// tx_pkt[1] = 0x00;
	// tx_pkt[2] = 0x00;
	// tx_pkt[3] = 0x20;
	// tx_pkt_queue_push(tx_pkt);

	// sleep(2);

	// tx_pkt[0] = 0x00;
	// tx_pkt[1] = 0x00;
	// tx_pkt[2] = 0x00;
	// tx_pkt[3] = 0x08;
	// tx_pkt_queue_push(tx_pkt);

	// tx_pkt[0] = 0x00;
	// tx_pkt[1] = 0x00;
	// tx_pkt[2] = 0x00;
	// tx_pkt[3] = 0x20;
	// tx_pkt_queue_push(tx_pkt);

	// tx_pkt[0] = 0x00;
	// tx_pkt[1] = 0x00;
	// tx_pkt[2] = 0x00;
	// tx_pkt[3] = 0x28;
	// tx_pkt_queue_push(tx_pkt);

	printf("Popping packets from rx queue\n");
	for(i = 0; i < NUM_TRANSFERS; i++) {
		while(is_rx_pkt_queue_empty() == 1) {}
		rx_pkt_queue_pop(rx_pkt);
		printf("rx pkt = 0x");
		for(j = OUT_TDATA_WIDTH_BYTES; j > 0; j--) {
			printf("%02x", rx_pkt[j-1]);
		}
		printf("\n");
	}

	end_time = get_posix_clock_time_usec();

	printf("Total time in usec for %d %d-byte transfers = %llu\n", (int)NUM_TRANSFERS, BUFFER_SIZE, end_time-start_time);

	while(1) {}

	if(cleanup_neuro_fpga_dma() != 0) {
		return EXIT_FAILURE;
	}	

	printf("Successfully finished Neuro FPGA DMA Test\n");

	return EXIT_SUCCESS;
}