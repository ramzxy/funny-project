# Module 3 — Week 2: Reliable Data Transfer

Two projects for the DRDT (Designing Reliable Data Transfer) challenge.

## rdt_cpp — Challenge Client

The actual client that connects to the university challenge server.

### Compile

```bash
cd rdt_cpp
make
```

This produces the `drdtchallenge` binary. Use `make debug` for a debug build with `-g3`.

To clean build artifacts:

```bash
make clean
```

### Run

```bash
./drdtchallenge [file_number]
```

- `file_number` is optional (1–6), defaults to 1
- File sizes: 1=248B, 2=2085B, 3=6267B, 4=21067B, 5=53228B, 6=141270B

### Server

- **Address:** `challenges.dacs.utwente.nl`
- **Port:** `8002`
- **Auth token:** set in `my_protocol/Program.cpp` (`groupToken`)

### How it works

1. Run the client — it connects to the challenge server
2. Press Enter to start as **sender**, or wait to be started as **receiver** by the other group member
3. The protocol implementation in `my_protocol/MyProtocol.cpp` handles the data transfer
4. Received files are saved as `rdtcOutput<N>.<timestamp>.png`

## TCP-prot — Protocol Simulation

A standalone TCP-like protocol simulation for local testing (no server needed).

### Compile

```bash
cd TCP-prot
g++ -std=c++17 -Iinclude src/*.cpp -o tcp_sim
```

### Run

```bash
./tcp_sim
```

Runs a local simulation of two endpoints exchanging packets with SACK-based acknowledgment.
