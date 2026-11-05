# Ring

A **ring buffer** of bytes, with *no* allocation after it is made. See the
[design notes](docs/design.md) or <https://example.com/ring>.

## Use

1. Make a ring of a fixed capacity.
2. Write bytes with `ringWrite`, and read them back one at a time.
   - A write past the capacity is cut short.
   - A read of an empty ring returns `RING_EMPTY`.

> **Note:** the ring is not safe for two threads at once.

```c
Ring ring = {0};
ringWrite(&ring, "hello", 5);
```

```python
from ring import Ring
ring = Ring(capacity=64)
```

```sh
make DEBUG=1 test
```

| Operation | Cost | Allocates |
|-----------|-----:|:---------:|
| write     | O(n) | no        |
| read      | O(1) | no        |

---

<details>
<summary>History</summary>

Version 1.2 takes `const void*`.
</details>

- [x] Fixed capacity
- [ ] Growing capacity
