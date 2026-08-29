# Scratch DB

Writing a database entirely from scratch using only C++ and its standard libraries and posix.


## Build, Run and Test

Build and Run
```bash
cmake -S . -B build && cmake --build build && ./build/db_cli
```

Build and Test
```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

## Features

- It offers full SQL functionality
- In addition it has options to turn table to RAM mode (Reddis like)
- Handles Concurrency, Durability and Consistency ([TODO] how in the future)

## Architecture

The databse can be divided into 2 parts:

- Storage Manager
- SQL Query Processor 

