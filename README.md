# Multithreaded LZ77 File Compressor

A simple **multithreaded file compression and decompression tool** using the **LZ77 algorithm** in C++.  
This project is designed to work on **Windows** and demonstrates how to handle **multithreading, file I/O, and basic data compression techniques**.

---

## Features
- **Compression**: Compress any file into a custom `.mtc` format.
- **Decompression**: Restore the original file from its compressed form.
- **Multithreading**: Utilizes multiple threads to speed up compression for large files.
- **Customizable**: Can be adapted for different block sizes and algorithms.
- **Cross-platform Codebase**: Although targeted for Windows, the code can be adapted for Linux with minimal changes.

---

## Prerequisites
- **Compiler**: MinGW-w64 or MSVC (Microsoft Visual C++)
- **CMake** (Optional, if you want a build system)
- **Linux OS**

---

## Build Instructions

### Using MinGW (g++)
1. Open **Command Prompt** or **PowerShell**.
2. Navigate to the project folder containing the `.cpp` files.
3. Compile:
   ```bash
   g++ -std=c++17 -pthread -o compressor.exe main.cpp lz77.cpp
``

---
### Compression (Syntax)
```bash
compressor.exe <mode> <input_file> <output_file>
```
----

### Decompression (Syntax)

```bash
compressor.exe d test.mtc test_restored.cpp
```

----
