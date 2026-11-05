"""Retries a flaky call with exponential backoff, as a decorator and a context of attempts."""

from __future__ import annotations

import asyncio
import random
from dataclasses import dataclass, field
from functools import wraps
from typing import Awaitable, Callable, TypeVar

T = TypeVar("T")
MAX_DELAY = 30.0


@dataclass(frozen=True)
class Backoff:
    attempts: int = 5
    base: float = 0.1
    jitter: bool = True
    retry_on: tuple[type[BaseException], ...] = field(default=(ConnectionError, TimeoutError))

    def delay(self, attempt: int) -> float:
        delay = min(MAX_DELAY, self.base * 2**attempt)
        return random.uniform(0, delay) if self.jitter else delay


def retry(backoff: Backoff = Backoff()) -> Callable[[Callable[..., Awaitable[T]]], Callable[..., Awaitable[T]]]:
    def decorate(fn):
        @wraps(fn)
        async def wrapper(*args, **kwargs):
            for attempt in range(backoff.attempts):
                try:
                    return await fn(*args, **kwargs)
                except backoff.retry_on as error:
                    if attempt + 1 == backoff.attempts:
                        raise
                    wait = backoff.delay(attempt)
                    print(f"attempt {attempt + 1} failed ({error!r}); retrying in {wait:.2f}s")
                    await asyncio.sleep(wait)
            raise AssertionError("unreachable")

        return wrapper

    return decorate


@retry(Backoff(attempts=3, jitter=False))
async def fetch(url: str) -> bytes:
    if random.random() < 0.5:
        raise ConnectionError(f"reset by {url}")
    return b"ok\n"


if __name__ == "__main__":
    print(asyncio.run(fetch("https://example.com")), 0x1F, 1_000, 2.5e-3, None, True)
