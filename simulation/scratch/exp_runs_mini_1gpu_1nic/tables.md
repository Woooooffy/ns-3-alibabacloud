### Latency

Simulated completion time in us; in parentheses, improvement over the baseline column (positive = faster).

| size/pair | noRate |
|---:|---:|
| 256MB | 87,328.2 |
| 512MB | 174,662.3 |

### PFC pause / resume frames

Count over the whole run. A single number means pause and resume agreed; `pause/resume` shows them separately when they did not.

| size/pair | noRate |
|---:|---:|
| 256MB | 0 |
| 512MB | 0 |

### Peak queue depth (KB)

Deepest egress queue reached on any switch port, at any instant, anywhere in the network.

| size/pair | noRate |
|---:|---:|
| 256MB | 332.2 |
| 512MB | 332.2 |

### Bytes shaped by the XML rate (%, paced / unshapeable messages)

Left: share of transmitted bytes whose gap the schedule's `rate` actually set. Right: share of rate-carrying messages that fit in one MTU, which have no inter-packet gap to stretch. A 0 on the left means the run is identical to one with `rate` off.

| size/pair | noRate |
|---:|---:|
| 256MB | 0.0 |
| 512MB | 0.0 |

### GPU fabric NIC bandwidth (Gbps, mean / peak per NIC)

Mean is per fabric NIC over the window in which any NIC was transmitting; peak is the busiest single sample. Line rate is 100.

| size/pair | noRate |
|---:|---:|
| 256MB | 74.9 / 100 |
| 512MB | 74.8 / 100 |
