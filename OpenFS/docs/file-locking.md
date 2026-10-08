# OpenFS file locking

OpenFS provides advisory byte-range locks associated with an open file description.

- SHARED permits other shared locks.
- EXCLUSIVE conflicts with locks owned by another open file description.
- length 0 means the range extends to the end of the address space.
- BLOCK waits until a conflicting lock disappears.
- dup shares the same open file description and therefore the same lock owner.
- Locks are released when the final reference to that open file description is closed.
- Locks are advisory; ordinary reads and writes are not implicitly blocked by them.

The lock registry is runtime-only and is never serialized to disk.
