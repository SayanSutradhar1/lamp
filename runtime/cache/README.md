# runtime/cache

Deterministic operation response caching with sharded LRU + TTL storage and `stale_while_revalidate` support.

## Key Components

- `cache_key.hpp` — Canonical cache key builder (`CacheKeyBuilder`)
- `local_operation_cache.hpp` — Thread-safe sharded LRU + TTL in-memory cache (`LocalOperationCache`)
- `layered_operation_cache.hpp` — Composable multi-tier cache (`LayeredOperationCache`)
