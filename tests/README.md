# Tests and benchmarks

The test executable is opt-in so release builds do not compile it:

```sh
cmake -S . -B build/tests -DCMAKE_BUILD_TYPE=Release -DBPP_BUILD_TESTS=ON
cmake --build build/tests --target bpp_tests -j "$(nproc)"
./build/tests/bpp_tests            # unit tests
./build/tests/bpp_tests --bench    # micro-benchmarks
ctest --test-dir build/tests --output-on-failure
```

`tests/fixtures` contains **held-out** data written by
`tests/fixtures/generate_fixtures.py` using only Python's standard library, not
by the C++ code under test. The fixtures cover:

- NBT kitchen-sink and large chunk-like payloads, plus truncated and corrupt
  inputs.
- Raw/zlib/gzip deflate streams and a corrupted stream.
- Golden serialized chunk and sub-region data.
- A complete `r.0.0.mcr` region file with one chunk.
- UTF-8 to UTF-16LE text pairs.
- A properties file with comments, whitespace and embedded `=` characters.

Re-generating the fixtures is deterministic:

```sh
python3 tests/fixtures/generate_fixtures.py
```

The benchmarks report ns/op and MiB/s for NBT parsing/writing, deflate
compression/decompression, chunk serialization, chunk generation, region
encoding/decoding, UCS-2 conversion and network string encoding. They adapt
their iteration count to run for roughly a quarter second per case, so results
are comparable between runs on the same machine.
