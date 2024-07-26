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

#define PKT_SIZE_BYTES 16

/* Struct used to describe a single DMA channel */
struct channel {
	struct channel_buffer *buf_ptr;	/* Pointer to channel buffer, which is the buffer in kernel space memory that holds data for DMA transferring/receiving */
	int fd;							/* Channel device file descriptor */
	pthread_t tid;					/* Channel thread id */
};

/* Struct used to describe a single node of the dynamically allocated packet queue */
struct pkt_queue_node {
	uint32_t pkt;
	struct pkt_queue_node *next;
};

/* Struct used to describe dynamically allocated queue of neuro FPGA packets */
struct pkt_queue {
	struct pkt_queue_node *front;
	struct pkt_queue_node *back;
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
static struct pkt_queue tx_pkt_queue;
static struct pkt_queue rx_pkt_queue;

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
int tx_pkt_queue_push(uint32_t pkt) {
	struct pkt_queue_node *new_node;

	if(stop) {
		return -1;
	}

	if(pthread_mutex_lock(&tx_pkt_queue_lock) != 0) {
		return -1;
	}

	new_node = (struct pkt_queue_node *) malloc(sizeof(struct pkt_queue_node));
	if(new_node == NULL) {
		pthread_mutex_unlock(&tx_pkt_queue_lock);
		return -1;
	}

	new_node->pkt = pkt;

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
int tx_pkt_queue_pop(uint32_t *pkt) {
	struct pkt_queue_node *node_to_delete;

	if(pthread_mutex_lock(&tx_pkt_queue_lock) != 0) {
		return -1;
	}

	if(is_tx_pkt_queue_empty() != 0) {
		pthread_mutex_unlock(&tx_pkt_queue_lock);
		return -1;
	}

	node_to_delete = tx_pkt_queue.front;
	*pkt = node_to_delete->pkt;

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
int rx_pkt_queue_push(uint32_t pkt) {
	struct pkt_queue_node *new_node;

	if(stop) {
		return -1;
	}

	if(pthread_mutex_lock(&rx_pkt_queue_lock) != 0) {
		return -1;
	}

	new_node = (struct pkt_queue_node *) malloc(sizeof(struct pkt_queue_node));
	if(new_node == NULL) {
		pthread_mutex_unlock(&rx_pkt_queue_lock);
		return -1;
	}

	new_node->pkt = pkt;

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
int rx_pkt_queue_pop(uint32_t *pkt) {
	struct pkt_queue_node *node_to_delete;

	if(pthread_mutex_lock(&rx_pkt_queue_lock) != 0) {
		return -1;
	}

	if(is_rx_pkt_queue_empty() != 0) {
		pthread_mutex_unlock(&rx_pkt_queue_lock);
		return -1;
	}

	node_to_delete = rx_pkt_queue.front;
	*pkt = node_to_delete->pkt;

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
	uint32_t pkt;
	uint8_t is_buffer_active[TX_BUFFER_COUNT] = {0};
	
	/* Initialize every tx buffer by writing the DMA transfer size (equal to the neuro FPGA packet size in bytes) to its buffer table */
	for(buffer_id = 0; buffer_id < TX_BUFFER_COUNT; buffer_id += 1) {
		channel_ptr->buf_ptr[buffer_id].length = PKT_SIZE_BYTES;
	}

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

			/* If there are any packets on the queue, copy the packet to the current DMA buffer and start a DMA transfer */
			if(is_tx_pkt_queue_empty() == 0) {
				tx_pkt_queue_pop(&pkt);
				channel_ptr->buf_ptr[buffer_id].buffer[0] = pkt;
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
	unsigned int i;
	int buffer_id;
	uint32_t pkt;
	
	/* Initialize every rx buffer by writing the DMA transfer size (equal to the neuro FPGA packet size in bytes) to its buffer table and starting a DMA transfer for the buffer*/
	for(buffer_id = 0; buffer_id < RX_BUFFER_COUNT; buffer_id += 1) {
		channel_ptr->buf_ptr[buffer_id].length = PKT_SIZE_BYTES;
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
				for(i = 0; i < PKT_SIZE_BYTES / sizeof(unsigned int); i++) {
					pkt = channel_ptr->buf_ptr[buffer_id].buffer[i];
					rx_pkt_queue_push(pkt);
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
			pkt = channel_ptr->buf_ptr[buffer_id].buffer[0];
			rx_pkt_queue_push(pkt);
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
	uint32_t pkt;

	while(is_tx_pkt_queue_empty() == 0) {
		tx_pkt_queue_pop(&pkt);
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
	uint32_t pkt;

	while(is_rx_pkt_queue_empty() == 0) {
		rx_pkt_queue_pop(&pkt);
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
	uint32_t pkt;
	uint64_t start_time;
	uint64_t end_time;

	const unsigned int NUM_TRANSFERS = 1;

	printf("Neuro FPGA DMA Test\n\n");

	if(setup_neuro_fpga_dma() != 0) {
		return EXIT_FAILURE;
	}

	printf("Adding packets to tx queue\n");

	start_time = get_posix_clock_time_usec();

	// for(i = 0; i < NUM_TRANSFERS; i++) {
	// 	tx_pkt_queue_push(i);
	// }

	tx_pkt_queue_push(1);

	printf("Popping packets from rx queue\n");
	for(i = 0; i < NUM_TRANSFERS; i++) {
		while(is_rx_pkt_queue_empty() == 1) {}
		rx_pkt_queue_pop(&pkt);
		printf("rx pkt=%08x\n", pkt);
		if(pkt != i) {
			printf("ERROR: popped rx DMA packet %d does not equal epected DMA packet %d\n", pkt, i);
		}
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