// RUN: circt-opt --verify-diagnostics --split-input-file %s

// expected-error @+1 {{ambiguous packing; wrap `unsized` in `packed<...>` or `unpacked<...>` to disambiguate}}
func.func @Foo(%arg0: !moore.unsized<bit>) { return }

// -----
// expected-error @+1 {{ambiguous packing; wrap `range` in `packed<...>` or `unpacked<...>` to disambiguate}}
func.func @Foo(%arg0: !moore.range<bit, 3:0>) { return }

// -----
// expected-error @+1 {{ambiguous packing; wrap `struct` in `packed<...>` or `unpacked<...>` to disambiguate}}
func.func @Foo(%arg0: !moore.struct<{}, loc(unknown)>) { return }
