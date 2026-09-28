### Latency

Simulated completion time in us; in parentheses, improvement over the baseline column (positive = faster).

| size/pair | baseline+sync | all+sync | p2p | p2p+sync |
|---:|---:|---:|---:|---:|
| 4KB | 27.0 | 24.3 | 21.6 | 21.6 |
| 32KB | 117.7 | 95.8 | 146.2 | 146.2 |
| 256KB | 873.8 | 678.1 | 1,176.0 | 1,176.0 |
| 2MB | 7,066.2 | 5,676.6 | 9,963.6 | 9,963.6 |
| 16MB | - | - | 80,690.8 | 79,947.2 |

### PFC pause / resume frames

Count over the whole run. A single number means pause and resume agreed; `pause/resume` shows them separately when they did not.

| size/pair | baseline+sync | all+sync | p2p | p2p+sync |
|---:|---:|---:|---:|---:|
| 4KB | 587 | 0 | 1,475 | 1,475 |
| 32KB | 14,098 | 0 | 18,565 | 18,565 |
| 256KB | 146,837 | 0 | 157,621 | 157,621 |
| 2MB | 1,268,774 | 90,348 | 1,328,880 | 1,328,880 |
| 16MB | - | - | 10,740,312 | 10,736,498 |

### Peak queue depth (KB)

Deepest egress queue reached on any switch port, at any instant, anywhere in the network.

| size/pair | baseline+sync | all+sync | p2p | p2p+sync |
|---:|---:|---:|---:|---:|
| 4KB | 128.1 | 83.6 | 201.6 | 201.6 |
| 32KB | 443.8 | 112.1 | 634.6 | 634.6 |
| 256KB | 1,232.0 | 112.1 | 1,804.4 | 1,804.4 |
| 2MB | 1,725.6 | 780.6 | 3,276.9 | 3,276.9 |
| 16MB | - | - | 3,683.4 | 3,343.3 |

### Bytes shaped by the XML rate (%, paced / unshapeable messages)

Left: share of transmitted bytes whose gap the schedule's `rate` actually set. Right: share of rate-carrying messages that fit in one MTU, which have no inter-packet gap to stretch. A 0 on the left means the run is identical to one with `rate` off.

| size/pair | baseline+sync | all+sync | p2p | p2p+sync |
|---:|---:|---:|---:|---:|
| 4KB | - | - | - | - |
| 32KB | - | - | - | - |
| 256KB | - | - | - | - |
| 2MB | - | - | - | - |
| 16MB | - | - | - | - |

### GPU fabric NIC bandwidth (Gbps, mean / peak per NIC)

Mean is per fabric NIC over the window in which any NIC was transmitting; peak is the busiest single sample. Line rate is 400.

| size/pair | baseline+sync | all+sync | p2p | p2p+sync |
|---:|---:|---:|---:|---:|
| 4KB | - | - | - | - |
| 32KB | - | - | - | - |
| 256KB | - | - | - | - |
| 2MB | - | - | - | - |
| 16MB | - | - | - | - |
