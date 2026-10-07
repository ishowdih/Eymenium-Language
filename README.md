# Eymenium Language

**Eymenium** is a lightweight, easy-to-learn programming language written in **C++**.

It is designed to have a simple and readable syntax while still providing features such as functions, loops, classes, modules, exception handling, and more.

## Features

- Simple and readable syntax
- Written in C++
- Fast runtime
- Functions and return values
- Conditional statements
- `while` and `for` loops
- Lists and list comprehensions
- Classes and object-oriented programming
- Exception handling
- Modules
- Built-in functions
- Cross-platform development with CMake

## Example

```eymenium
exp greet(name)
    out "Hello, " + name

greet("World")
```

A simple loop:

```eymenium
loop i within range(5)
    out i
```

Conditional statements:

```eymenium
chk age >= 18
    out "Adult"
orchk age >= 13
    out "Teenager"
nah
    out "Child"
```

## Why Eymenium?

Eymenium aims to combine the simplicity of high-level languages with the performance of a C++-based runtime.

The syntax is designed to be easy to understand, especially for people who are new to programming.

Instead of making the language unnecessarily complicated, Eymenium focuses on readable code and straightforward syntax.

## Documentation

The official documentation and learning materials are available here:

**https://eymenium.pages.dev/learn**

The documentation covers the language syntax, functions, loops, classes, lists, modules, exceptions, and other features.

## Project Structure

```text
Eymenium/
├── src/
├── include/
├── examples/
├── tests/
├── CMakeLists.txt
├── README.md
└── LICENSE
```

## Building

Eymenium uses **CMake** as its build system.

Clone the repository:

```bash
git clone <REPOSITORY_URL>
cd Eymenium
```

Configure the project:

```bash
cmake -S . -B build
```

Build it:

```bash
cmake --build build --config Release
```

The exact build instructions may vary depending on the operating system and compiler.

## Language Syntax

Some Eymenium keywords:

| Eymenium | Purpose |
|---|---|
| `exp` | Function |
| `chk` | If |
| `orchk` | Else if |
| `nah` | Else |
| `spin` | While loop |
| `loop` | For loop |
| `within` | In |
| `back` | Return |
| `blueprint` | Class |

## Performance

Eymenium is implemented in C++, allowing it to perform significantly faster than some equivalent interpreted Python workloads in certain benchmarks.

Performance depends heavily on the program, algorithm, hardware, and implementation details, so benchmarks should always be compared under the same conditions.

## Status

Eymenium is an actively developed project.

Features and syntax may change as the language evolves.

## License

See the `LICENSE` file for the license used by this project.

## Links

- **Website:** https://eymenium.pages.dev/
- **Documentation:** https://eymenium.pages.dev/learn
- **Source Code:** `<GITHUB_REPOSITORY_URL>`

---

Made with C++ and an unreasonable amount of curiosity.
