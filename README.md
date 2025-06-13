# DPDK Packet Loss

Test program for tuning kernel for DPDK on Linux. Program prints out
packet loss and errors. A well-configured instance should print out
zeroes for both even when pushing packets close to the maximum capacity
of the NIC. This typically requires isolating CPUs for DPDK in
the kernel as well as turning off power saving states.

The program configures the NIC with `min(cpus-1, rx_queues)` number of
Rx queues.
