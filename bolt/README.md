# BOLT

BOLT is a post-link optimizer developed to speed up large applications.
It achieves the improvements by optimizing application's code layout based on
execution profile gathered by sampling profiler, such as Linux `perf` tool.
An overview of the ideas implemented in BOLT along with a discussion of its
potential and current results is available in
[CGO'19 paper](https://research.fb.com/publications/bolt-a-practical-binary-optimizer-for-data-centers-and-beyond/).

## Input Binary Requirements

BOLT operates on X86-64 and AArch64 ELF binaries. At the minimum, the binaries
should have an unstripped symbol table, and, to get maximum performance gains,
they should be linked with relocations (`--emit-relocs` or `-q` linker flag).



### Manual Build

BOLT heavily uses LLVM libraries, and by design, it is built as one of LLVM
tools. The build process is not much different from a regular LLVM build.
The following instructions are assuming that you are running under Linux.

Start with cloning LLVM and BOLT repos:

```
> git clone https://github.com/facebookincubator/BOLT llvm-bolt
> mkdir build
> cd build
> cmake -G Ninja ../llvm-bolt/llvm -DLLVM_TARGETS_TO_BUILD="X86;AArch64" -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_ASSERTIONS=ON -DLLVM_ENABLE_PROJECTS="clang;lld;bolt"
> ninja
```

`llvm-bolt` will be available under `bin/`. Add this directory to your path to
ensure the rest of the commands in this tutorial work.

Note that we use a specific snapshot of LLVM monorepo as we currently
rely on a set of patches that are not yet upstreamed.



## Usage

For a complete practical guide of using BOLT see [Optimizing Clang with BOLT](./bolt/docs/OptimizingClang.md).

### Step 0

In order to allow BOLT to re-arrange functions (in addition to re-arranging
code within functions) in your program, it needs a little help from the linker.
Add `--emit-relocs` to the final link step of your application. You can verify
the presence of relocations by checking for `.rela.text` section in the binary.
BOLT will also report if it detects relocations while processing the binary.

### Step 1: Collect Profile

This step is different for different kinds of executables. If you can invoke
your program to run on a representative input from a command line, then check
**For Applications** section below. If your program typically runs as a
server/service, then skip to **For Services** section.

The version of `perf` command used for the following steps has to support
`-F brstack` option. We recommend using `perf` version 4.5 or later.

#### For Applications

This assumes you can run your program from a command line with a typical input.
In this case, simply prepend the command line invocation with `perf`:
```
$ perf record -e cycles:u -j any,u -o perf.data -- <executable> <args> ...
```

#### For Services

Once you get the service deployed and warmed-up, it is time to collect perf
data with LBR (branch information). The exact perf command to use will depend
on the service. E.g., to collect the data for all processes running on the
server for the next 3 minutes use:
```
$ perf record -e cycles:u -j any,u -a -o perf.data -- sleep 180
```

Depending on the application, you may need more samples to be included with
your profile. It's hard to tell upfront what would be a sweet spot for your
application. We recommend the profile to cover 1B instructions as reported
by BOLT `-dyno-stats` option. If you need to increase the number of samples
in the profile, you can either run the `sleep` command for longer and use
`-F<N>` option with `perf` to increase sampling frequency.

Note that for profile collection we recommend using cycle events and not
`BR_INST_RETIRED.*`. Empirically we found it to produce better results.

If the collection of a profile with branches is not available, e.g., when you run on
a VM or on hardware that does not support it, then you can use only sample
events, such as cycles. In this case, the quality of the profile information
would not be as good, and performance gains with BOLT are expected to be lower.

#### With instrumentation

If perf record is not available to you, you may collect profile by first
instrumenting the binary with BOLT and then running it.
```
llvm-bolt <executable> -instrument -o <instrumented-executable>
```

After you run instrumented-executable with the desired workload, its BOLT
profile should be ready for you in `/tmp/prof.fdata` and you can skip
**Step 2**.

Run BOLT with the `-help` option and check the category "BOLT instrumentation
options" for a quick reference on instrumentation knobs.

