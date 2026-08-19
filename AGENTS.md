# AGENTS.md

## General principles

- Do not preserve backward compatibility unless it is an explicit requirement. Remove obsolete APIs, code paths, compatibility shims, and deprecated abstractions instead of extending them.
- Choose the simplest C++ implementation that fully meets the current requirements. Avoid speculative abstractions, excessive templates, unnecessary indirection, and premature generalization.
- Grow the system in small, working layers. Start with the smallest end-to-end implementation, then add capabilities without breaking existing behavior.
- Keep modules focused and concerns clearly separated. Prefer small classes and functions with explicit responsibilities over large inheritance hierarchies.
- Make architectural decisions for the long term. Do not accept temporary designs that are intended to be replaced later unless the temporary nature is explicitly documented and required.
- Always use 'build' folder, if needed just remove it and re-build.

## C++ design

- Prefer composition over inheritance. Use inheritance only when an actual substitutable relationship exists.
- Use runtime polymorphism only when multiple interchangeable runtime implementations are genuinely required.
- Use modern C++ features when they improve correctness, safety, or readability. Do not use advanced language features merely because they are available.
- Prefer RAII and deterministic lifetime management. Never manage resources through scattered manual cleanup.
- Express ownership clearly:
  - Prefer values for small, self-contained objects.
  - Prefer references for required non-owning access.
  - Use pointers for optional or nullable non-owning access.
  - Use `std::unique_ptr` for exclusive dynamic ownership.
  - Use `std::shared_ptr` only when shared ownership is unavoidable and intentional.
  - Do not use owning raw pointers.
- Prefer standard-library facilities over custom implementations. Do not reimplement containers, algorithms, smart pointers, synchronization primitives, or common utilities without a clear reason.
- Lean on dependencies already present in the project before adding new libraries. Check their documentation, types, supported features, and existing usage first.
- Keep public interfaces small and explicit. Hide implementation details when doing so meaningfully reduces coupling.
- Prefer compile-time guarantees when they simplify the design, but avoid template-heavy APIs that make code difficult to understand, debug, or compile.
- Minimize mutable shared state. Make data `const` by default and restrict mutation to the smallest practical scope.
- Avoid global mutable state.
- Isolate platform-specific code behind narrow interfaces.
- Write portable, standards-compliant C++ unless platform-specific behavior is an explicit requirement.

## Coding style

- Follow the existing project style when it is consistent and reasonable. Do not introduce a competing style into an established codebase.
- Use `clang-format` when the project provides a configuration. Do not manually fight the formatter.
- Prefer clear, descriptive names over short or clever names.
- Use names that describe intent rather than implementation details.
- Use:
  - `PascalCase` for types, classes, structs, enums, and concepts.
  - `camelCase` for functions, methods, and local variables.
  - `snake_case` only when required by an existing project convention or external API.
  - `kPascalCase` for compile-time constants when consistent with the project.
- Name boolean values and functions so they read naturally, such as `isReady`, `hasValue`, `canRetry`, and `shouldRefresh`.
- Avoid vague names such as `data`, `info`, `manager`, `helper`, `util`, `temp`, or `obj` unless their meaning is obvious from a very small scope.
- Keep functions short and focused on one responsibility.
- Prefer early returns over deeply nested conditionals.
- Avoid nesting beyond a few levels. Extract logic when nesting obscures the main path.
- Keep the normal execution path visually clear.
- Prefer straightforward control flow over clever expressions.
- Do not compress multiple logical operations into one line merely to reduce line count.
- Use braces for all control-flow statements, including single-line bodies.
- Declare variables as close as possible to their first use.
- Initialize variables at declaration.
- Prefer direct initialization and avoid unnecessary temporary objects.
- Mark variables and member functions `const` whenever appropriate.
- Use `constexpr` when a value or function is genuinely compile-time evaluable.
- Use `[[nodiscard]]` for results that should not be silently ignored.
- Use `override` and `final` explicitly where applicable.
- Prefer scoped enums with `enum class`.
- Avoid macros except for include guards, platform integration, conditional compilation, or cases where the language provides no reasonable alternative.
- Prefer `nullptr` over `NULL` or `0`.
- Prefer range-based loops and standard algorithms when they improve clarity.
- Do not force standard algorithms when a normal loop is easier to understand.
- Avoid implicit narrowing conversions. Use explicit checked conversions where necessary.
- Avoid C-style casts. Use the appropriate C++ cast and make the reason evident.
- Do not rely on operator precedence when parentheses would improve readability.
- Keep source files organized consistently:
  1. Corresponding header
  2. C++ standard-library headers
  3. Third-party headers
  4. Project headers
- Keep includes minimal and explicit. Do not depend on transitive includes.
- Do not place `using namespace` directives in headers.
- Avoid broad `using namespace` directives in source files. Prefer targeted aliases when they genuinely improve readability.

## Comments and documentation

- Comment code when the intent, constraint, invariant, workaround, or reasoning is not obvious from the code itself.
- Comments should explain **why**, not repeat **what** the code already says.
- Do not write comments that merely translate code into English.

Bad:

```cpp
// Increment the retry count.
++retryCount;
```