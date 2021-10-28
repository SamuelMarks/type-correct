type-correct
============

[![CC0](https://img.shields.io/badge/license-CC0-%23373737)](LICENSE.md)
[![x86-Darwin](https://github.com/SamuelMarks/type-correct/actions/workflows/x86-darwin.yml/badge.svg)](https://github.com/SamuelMarks/type-correct/actions/workflows/x86-darwin.yml)
[![x86-Ubuntu-llvm-from-sources](https://github.com/SamuelMarks/type-correct/actions/workflows/x86-ubuntu-llvm-from-sources.yml/badge.svg)](https://github.com/SamuelMarks/type-correct/actions/workflows/x86-ubuntu-llvm-from-sources.yml)
[![x86-Darwin-llvm-from-sources](https://github.com/SamuelMarks/type-correct/actions/workflows/x86-darwin-llvm-from-sources.yml/badge.svg)](https://github.com/SamuelMarks/type-correct/actions/workflows/x86-darwin-llvm-from-sources.yml)
[![x86-Ubuntu](https://github.com/SamuelMarks/type-correct/actions/workflows/x86-ubuntu.yml/badge.svg)](https://github.com/SamuelMarks/type-correct/actions/workflows/x86-ubuntu.yml)

[LLVM](https://llvm.org) [LibClang](https://clang.llvm.org/doxygen/group__CINDEX.html) / [LibTooling](https://clang.llvm.org/docs/LibTooling.html) solution to 'fix' types, rewriting inconsistent use of types to make them consistent.

## Automatic conversions
```c
#include <string.h>

int main(void) {

    /* FROM */
    const int n = strlen("FOO");

    /* TO */
    const size_t n = strlen("FOO");

    /* FROM */
    for(int i=0; i<strlen("BAR"); i++) {}

    /* TO */
    for(size_t i=0; i<strlen("BAR"); i++) {}

    /* FROM */
    int f(long b) { return b; }
    static const int c = f(5);

    /* TO */
    long f(long b) { return b; }
    static const long c = f(5);
}
```

## Justification

Often when building third-party libraries I get a bunch of warnings "comparison between signed and unsigned types is UB".

Not every such occurrence has a trivial solution. But—in my experience—most do. Usually switching just one var from `int` to `size_t` also requires tracing all use of that var and changing all those types to `size_t` also.

From:
```c
unsigned long f() {return 0;}
const size_t v = f();

int main() {
    std::vector<float> vec;
    for(int i=0; i<vec.size(); i++) {}
}
```

To:
```c
unsigned long f() {return 0;}
const unsigned long v = f();

int main() {
    std::vector<float> vec;
    for(size_t i=0; i<vec.size(); i++) {}
}
```

PS: I'm aware that [`size_type`](https://github.com/llvm/llvm-project/blob/d081d75dc8fc4b5173d6b15ffcf077d2e0d4143f/libcxx/include/vector#L321) isn't necessarily `size_t`—and that `decltype(vec)::size_type` would be more correct—but using it here anyway. Just to reiterate: C++ is an afterthought, my main target is C.

## Integer promotion

```c
#include <limits.h>

short sh=SHRT_MAX;
int i=INT_MAX;
long l=LONG_MAX;
long long ll=LLONG_MAX; /* C99 */
```

Technically, these are all [defined and expected](https://en.cppreference.com/w/c/language/conversion) [on clang as a[`ImplicitCastExpr`](https://clang.llvm.org/doxygen/classclang_1_1ImplicitCastExpr.html)]:

```c
ll = l;
l = i;
i = sh;
```

(but the other direction, '[narrowing](https://releases.llvm.org/13.0.0/tools/clang/tools/extra/docs/clang-tidy/checks/cppcoreguidelines-narrowing-conversions.html)', is implementation defined)

However, IMHO, people doing `int sl = strlen(s);` actually want `size_t`. This opinionated view is the assumption made for _type_correct_.

## Build instructions

Install compiler suite, CMake, and LLVM (from `brew`, `apt`, or source), then run:

    mkdir build && cd build
    cmake .. \
          -DCMAKE_BUILD_TYPE='Debug' \
          -DCT_Clang_INSTALL_DIR='/usr/local/opt/llvm'

(replace `/usr/local/opt/llvm` with your LLVM install dir)

## Thanks

Boilerplate from  https://github.com/banach-space/clang-tutor

---

### License

The person who associated a work with this deed has **dedicated** the work to the public domain by waiving all of his or her rights to the work worldwide under copyright law, including all related and neighboring rights, to the extent allowed by law.

You can copy, modify, distribute and perform the work, even for commercial purposes, all without asking permission. See [Other Information](#Other%20Information) below.

#### Other Information

  - In no way are the patent or trademark rights of any person affected by CC0, nor are the rights that other persons may have in the work or in how the work is used, such as publicity or privacy rights. 
  - Unless expressly stated otherwise, the person who associated a work with this deed makes no warranties about the work, and disclaims liability for all uses of the work, to the fullest extent permitted by applicable law. 
  - When using or citing the work, you should not imply endorsement by the author or the affirmer.
