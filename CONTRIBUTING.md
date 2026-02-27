# Contributing to LLM Audit GUI

Welcome! Thank you for considering contributing to the LLM Audit GUI project.

## Development Setup

1. **Prerequisites**
   - CMake 3.21+
   - A C++20 compatible compiler (GCC, Clang, or MSVC)
   - Ninja build system (recommended)
   - Internet connection (for FetchContent dependencies like Raylib and JSON)
   - `curl` development packages (if not on Windows)

2. **Building from Source**
   To build the project natively:
   ```bash
   cmake --preset native-release
   cmake --build --preset build-native-release
   ```
   The executable will be located at `build/native-release/llm_audit_gui`.

3. **Submitting Changes**
   - Create a new branch for your feature or bugfix.
   - Keep your commits small and descriptive.
   - Ensure the project builds successfully on your platform.
   - Submit a Pull Request describing your changes.

Thanks for your help in improving the LLM Audit GUI!
