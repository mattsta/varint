# Varint Library Example Suite

This directory contains comprehensive examples demonstrating all varint encodings and their combinations.

## Directory Structure

```
examples/
├── standalone/          # Individual module examples
│   ├── example_tagged.c
│   ├── example_external.c
│   ├── example_split.c
│   ├── example_chained.c
│   ├── example_packed.c
│   ├── example_dimension.c
│   └── example_bitstream.c
│
├── integration/         # Combination examples
│   ├── database_system.c
│   ├── network_protocol.c
│   ├── column_store.c
│   ├── game_engine.c
│   ├── sensor_network.c
│   └── ml_features.c
│
├── reference/          # Complete reference implementations
│   ├── kv_store.c
│   ├── timeseries_db.c
│   └── graph_database.c
│
└── advanced/           # Production-quality real-world systems
    ├── blockchain_ledger.c
    ├── dns_server.c
    ├── game_replay_system.c
    ├── bytecode_vm.c
    ├── inverted_index.c
    ├── financial_orderbook.c
    ├── log_aggregation.c
    ├── geospatial_routing.c
    └── trie_pattern_matcher.c
```

## Standalone Examples

Each standalone example demonstrates a single varint type with:
- Basic encode/decode operations
- Boundary value testing
- Common use patterns
- Error handling

| Example | Module | Description |
|---------|--------|-------------|
| `example_tagged.c` | varintTagged | Sortable database keys |
| `example_external.c` | varintExternal | Zero-overhead encoding |
| `example_split.c` | varintSplit | Three-level encoding |
| `example_chained.c` | varintChained | Legacy Protocol Buffers format |
| `example_packed.c` | varintPacked | Fixed-width bit-packed arrays |
| `example_dimension.c` | varintDimension | Matrix storage |
| `example_bitstream.c` | varintBitstream | Bit-level operations |

## Integration Examples

Real-world scenarios combining multiple varint types:

### database_system.c
**Combines**: varintTagged (keys) + varintExternal (values) + varintPacked (indexes)
- B-tree implementation with sortable varint keys
- Column store with external metadata
- Packed integer indexes

### network_protocol.c
**Combines**: varintBitstream (headers) + varintChained (Protocol Buffers compatibility)
- Custom protocol with bit-packed headers
- Protocol Buffers wire format encoding
- Message framing

### column_store.c
**Combines**: varintExternal (columns) + varintDimension (metadata)
- Columnar data storage
- Schema-driven encoding
- Efficient compression

### game_engine.c
**Combines**: varintPacked (coordinates) + varintBitstream (flags)
- Entity position storage (12-bit coordinates)
- Compact state flags
- Network synchronization

### sensor_network.c
**Combines**: varintExternal (timestamps) + varintPacked (readings)
- Time-series data storage
- Multi-sensor reading arrays
- Delta encoding

### ml_features.c
**Combines**: varintDimension (sparse matrices) + varintExternal (values)
- Sparse feature matrices
- Variable-width feature IDs
- Efficient storage for ML datasets

## Reference Implementations

Complete, production-ready implementations:

### kv_store.c
**Full key-value store** with:
- varintTagged keys (sortable, memcmp-friendly)
- varintExternal values (space-efficient)
- varintPacked indexes (B-tree node arrays)
- Serialization/deserialization
- Persistence layer

### timeseries_db.c
**Time-series database** with:
- varintExternal timestamps (40-bit unix time)
- varintPacked sensor readings (14-bit values)
- Compression and downsampling
- Range queries

### graph_database.c
**Graph storage system** with:
- varintDimension adjacency matrices (bit matrices)
- varintTagged node IDs (sortable)
- varintExternal edge weights
- Graph traversal algorithms

## Advanced Examples

Production-quality real-world systems with comprehensive benchmarks. See [advanced/README.md](advanced/README.md) for full details.

**Highlights:**
- **blockchain_ledger.c** - Cryptocurrency transactions (10x compression)
- **dns_server.c** - DNS packet encoding (1M+ queries/sec)
- **game_replay_system.c** - Delta compression (100:1 ratio)
- **bytecode_vm.c** - VM instruction encoding (50-70% smaller)
- **inverted_index.c** - Search engine posting lists (20-30x compression)
- **financial_orderbook.c** - HFT order processing (sub-microsecond)
- **log_aggregation.c** - Log collection (100:1 compression)
- **geospatial_routing.c** - GPS coordinate compression (20-40x)
- **trie_pattern_matcher.c** - AMQP routing (2391x faster, 0.7 bytes/pattern)

## Building Examples

```bash
# From repository root
mkdir -p build
cd build
cmake ..
make examples

# Run individual examples
./build/examples/example_tagged
./build/examples/database_system
./build/examples/kv_store
```

## Example Template

Each example follows this pattern:

```c
#include "../src/varint*.h"
#include <stdio.h>
#include <assert.h>

// 1. Data structure definition
typedef struct { ... } MyStructure;

// 2. Core operations
void myEncode(...) { ... }
void myDecode(...) { ... }

// 3. Usage demonstration
void demonstrate() { ... }

// 4. Testing with assertions
void test() { ... }

// 5. Main with output
int main() {
    demonstrate();
    test();
    printf("All tests passed!\n");
    return 0;
}
```

## Learning Path

### Beginners
1. Start with `example_tagged.c` - simplest self-describing format
2. Try `example_external.c` - understand external metadata
3. Explore `example_packed.c` - fixed-width arrays

### Intermediate
1. Study `database_system.c` - see how types combine
2. Examine `network_protocol.c` - bit-level efficiency
3. Review `column_store.c` - schema-driven design

### Advanced
1. Implement `kv_store.c` modifications
2. Extend `timeseries_db.c` with new features
3. Optimize `graph_database.c` for your use case

### Expert
1. Study production systems in `advanced/` directory
2. Start with `bytecode_vm.c` for fundamental patterns
3. Progress to `trie_pattern_matcher.c` for data structures
4. Master `blockchain_ledger.c` or `financial_orderbook.c` for complete systems
5. See `advanced/README.md` for detailed learning path

## Testing

All examples include:
- ✅ Assertions for correctness
- ✅ Boundary value tests
- ✅ Round-trip encode/decode verification
- ✅ Memory leak checks (use valgrind)

Run with valgrind:
```bash
valgrind --leak-check=full ./build/examples/kv_store
```

## Performance Benchmarking

Compare varint types for your workload:

```c
// See src/varintCompare.c for comprehensive benchmarks
// Adapt for your specific use case
```

## Contributing Examples

To add a new example:
1. Choose appropriate directory (standalone/integration/reference)
2. Follow the example template
3. Include comprehensive comments
4. Add test cases with assertions
5. Update this README
6. Submit pull request

## Support

- **Documentation**: See `docs/` directory
- **API Reference**: See `docs/modules/*.md`
- **Decision Guide**: See `docs/CHOOSING_VARINTS.md`
- **Issues**: GitHub issues

## License

Examples are released under the same license as the library (Apache-2.0).
