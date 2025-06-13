#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <stdio.h>
#include <stdlib.h>

#include <atomic>
#include <thread>

#define RX_RING_SIZE 512
#define TX_RING_SIZE 512
#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32
#define DEVICE_PORT 0

static struct rte_mempool *mbuf_pool = nullptr;

struct lcore_stats {
  std::atomic_uint64_t packet_count = 0;
} __rte_cache_aligned;

struct lcore_stats stats[RTE_MAX_LCORE];

int lcore_worker(__rte_unused void *arg) {
  unsigned lcore_id = rte_lcore_id();
  printf("Starting packet poller on lcore %u\n", lcore_id);

  struct rte_mbuf *bufs[BURST_SIZE];
  uint16_t nb_rx;

  for (;;) {
    uint64_t local_packet_count = 0;

    for (int i = 0; i < 64; ++i) {
      nb_rx = rte_eth_rx_burst(0, 0, bufs, BURST_SIZE);
      local_packet_count += nb_rx;

      for (int i = 0; i < nb_rx; i++) {
        struct rte_mbuf *m = bufs[i];
        rte_pktmbuf_free(m);
      }
    }

    stats[lcore_id].packet_count.fetch_add(local_packet_count, std::memory_order_relaxed);
  }

  return 0;
}

void controller() {
  uint64_t last_num_packets = 0;
  uint64_t last_dropped_packets = 0;
  uint64_t last_errors = 0;

  while (1) {
    struct rte_eth_stats port_stats;
    rte_eth_stats_get(DEVICE_PORT, &port_stats);
    uint64_t current_dropped_packets = port_stats.imissed;
    uint64_t current_errors = port_stats.ierrors;

    uint64_t current_num_packets = 0;
    for (int i = 0; i < RTE_MAX_LCORE; ++i) {
      current_num_packets += stats[i].packet_count.load(std::memory_order_relaxed);
    }

    printf("Current PPS: %lu\n", current_num_packets - last_num_packets);
    printf("Dropped packets: %lu\n", current_dropped_packets - last_dropped_packets);
    printf("Errors: %lu\n", current_errors - last_errors);

    last_num_packets = current_num_packets;
    last_dropped_packets = current_dropped_packets;
    last_errors = current_errors;

    rte_delay_ms(1000);
  }
}

int init_device_and_start_pollers(uint16_t port, struct rte_mempool *mbufpool) {
  struct rte_eth_conf port_conf_default;
  memset(&port_conf_default, 0, sizeof(struct rte_eth_conf));

  uint16_t nb_queues;

  uint16_t nb_rxd = RX_RING_SIZE;
  uint16_t nb_txd = TX_RING_SIZE;

  struct rte_eth_dev_info dev_info;
  struct rte_eth_txconf txconf;

  if (!rte_eth_dev_is_valid_port(port)) {
    return -1;
  }

  int retval = rte_eth_dev_info_get(port, &dev_info);
  if (retval != 0) {
    fprintf(stderr, "Error during getting device (port %u) info: %s\n", port, strerror(-retval));
    return retval;
  }

  if (dev_info.max_rx_queues != dev_info.max_tx_queues) {
    fprintf(stderr, "Queues are not combined Rx/Tx queues (port %u)\n", port);
    return -1;
  }

  nb_queues = std::min(dev_info.max_rx_queues, (uint16_t)(rte_lcore_count() - 1));
  printf("Using %u out of a maximum of %u NIC queues\n", dev_info.max_rx_queues, nb_queues);

  if (nb_queues == 0) {
    fprintf(stderr, "No queues available to configure\n");
    return -1;
  }

  retval = rte_eth_dev_configure(port, nb_queues, nb_queues, &port_conf_default);
  if (retval < 0) {
    fprintf(stderr, "Error configuring device: %s\n", strerror(-retval));
    return retval;
  }

  for (uint16_t q = 0; q < nb_queues; ++q) {
    retval = rte_eth_rx_queue_setup(port, 0, nb_rxd, rte_eth_dev_socket_id(port), NULL, mbufpool);
    if (retval < 0) {
      fprintf(stderr, "Error setting up Rx queue %u: %s\n", q, strerror(-retval));
      return retval;
    }

    retval = rte_eth_tx_queue_setup(port, 0, nb_txd, rte_eth_dev_socket_id(port), &txconf);
    if (retval < 0) {
      fprintf(stderr, "Error setting up Tx queue %u: %s\n", q, strerror(-retval));
      return retval;
    }
  }

  printf("Configured NIC with %u Rx/Tx queues\n", nb_queues);

  retval = rte_eth_dev_start(port);
  if (retval < 0) {
    fprintf(stderr, "Error starting device: %s\n", strerror(-retval));
    return retval;
  }

  struct rte_ether_addr addr;
  retval = rte_eth_macaddr_get(port, &addr);
  if (retval < 0) {
    fprintf(stderr, "Error retrieving device MAC address: %s\n", strerror(-retval));
    return retval;
  }

  printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8
         "\n",
         port, RTE_ETHER_ADDR_BYTES(&addr));

  for (unsigned int lcore_id = rte_get_next_lcore(-1, true, false); lcore_id < 64;
       lcore_id = rte_get_next_lcore(lcore_id, true, false)) {
    rte_eal_remote_launch(lcore_worker, NULL, lcore_id);
  }

  return 0;
}

int main(int argc, char *argv[]) {
  uint32_t nb_ports;

  if (rte_eal_init(argc, argv) < 0) {
    rte_exit(EXIT_FAILURE, "EAL initialization failed");
  }

  nb_ports = rte_eth_dev_count_avail();
  if (nb_ports != 1) {
    rte_exit(EXIT_FAILURE, "Error: Program assumes a single DPDK interface\n");
  }

  mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports, MBUF_CACHE_SIZE, 0,
                                      RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  if (!mbuf_pool) {
    rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");
  }

  if (init_device_and_start_pollers(DEVICE_PORT, mbuf_pool) != 0) {
    rte_exit(EXIT_FAILURE, "Cannot init port %" PRIu16 "\n", DEVICE_PORT);
  }

  std::thread controller_thread(controller);
  controller_thread.join();

  return 0;
}
