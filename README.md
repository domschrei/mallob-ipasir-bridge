# Mallob IPASIR Bridge
An IPASIR interface to connect applications to Mallob's incremental on-demand SAT solving

## Building

Execute `make`. This produces the library `libipasirmallob.a` which provides all IPASIR functions as defined in `src/ipasir.h`.
By default, the interface will use `.api/jobs.0/` as a location of Mallob's JSON API directory (relative to the path from where the application is executed!). This can be changed at build time by supplying different values for `MALLOB_BASE_DIRECTORY` and/or `MALLOB_API_INDEX` as follows:
```
make MALLOB_BASE_DIRECTORY='"path/to/my/mallob"' MALLOB_API_INDEX='"5"'
```
This would let the interface use `path/to/my/mallob/.api/jobs.5/` as the API directory.

Now you can link IPASIR applications with the library file `libipasirmallob.a` to obtain an application which uses Mallob as a backend SAT solver.
For more information on linking IPASIR applications with IPASIR solvers, see [the IPASIR Github repository](https://github.com/biotomas/ipasir).

## Usage

Mallob must run in the background in order to use this interface.
For each call to `solve()`, this IPASIR bridge creates a JSON request file and puts it into the `new/` subdirectory of the API directory and awaits an answer in the `done/` subdirectory.

For correct functionality, it is essential that your application properly calls `ipasir_release` for each IPASIR instance it created.
Otherwise, stale job nodes that are reserved for the dead job may accumulate and eventually clog the entire system. 

## ToDos

* The termination method is not functional yet because Mallob does not yet support arbitrary interruption of a certain incremental job "from the outside".
* No clauses are reported even if a callback is supplied: Mallob does not export clauses to the outside (yet).
