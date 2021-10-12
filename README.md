type-correct
============

[![CC0](https://img.shields.io/badge/license-CC0-%23373737)](LICENSE.md)

[LLVM](https://llvm.org) [LibClang](https://clang.llvm.org/doxygen/group__CINDEX.html) / [LibTooling](https://clang.llvm.org/docs/LibTooling.html) solution to 'fix' types, rewriting inconsistent use of types to make them consistent.

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
    for(int i=0; i<vec.size(); i++) {}
}
```

PS: I'm aware that `size_type` isn't necessarily `size_t`… but using it here anyway. Just to reiterate: C++ is an afterthought, my main target is C.

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
