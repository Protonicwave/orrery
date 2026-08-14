"""The compute service.

Six things, in the order a run passes through them:

- `contract` is what crosses to the client, and the only statement of it.
- `configuration` reads the `.orrery` format, as the C++ reader reads it.
- `validation` decides whether a submitted configuration is a run this service
  will take, by the rules `sim/configuration.cpp` applies plus the ceilings in
  `limits`.
- `queue` holds the jobs, in PostgreSQL.
- `worker` claims one, runs the native simulator, and puts the result in object
  storage through `storage`.
- `api` is the HTTP and WebSocket surface over all of it.

What is not here is any physics. The service runs the binary this repository
builds and does nothing to its output beyond moving it, which is the property
that makes a run submitted from a browser the same run as one taken from a
command line.
"""

__all__ = ["__version__"]

#: Kept in step with the C++ by the release process, and reported by the API so
#: that a client can say which simulator produced a trajectory. The authority is
#: CMakeLists.txt; the worker reports what its binary answers to `--version`,
#: which is the figure that actually ran, and this is only the fallback for a
#: service that has not yet started one.
__version__ = "1.0.0"
