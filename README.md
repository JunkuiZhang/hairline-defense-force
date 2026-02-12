```bash
sudo apt install ninja-build
```

```bash
cmake --build build --target unit_tests
./bin/unit_tests

cmake --build build --target main
./bin/main < examples/demo_input.jsonl
```