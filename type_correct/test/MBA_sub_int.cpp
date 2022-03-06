// RUN: clang -cc1 -load %shlibdir/libTypeCorrect%shlibext -plugin TypeCorrect
// %s 2>&1 | FileCheck %s --match-full-lines

// CHECK: int short_f(int a, int b) {
// CHECK-NEXT: int c = a + b;

int short_f(int a, int b) {
  short c = a + b;
  return c;
}
