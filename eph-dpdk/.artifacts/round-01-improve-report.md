# Round 1: Improve Report

## Changes
1. **Constexpr `internet_checksum`**: Added `const uint8_t*` overload that is fully constexpr-evaluable (compile-time checksum verification via static_assert). Kept `const void*` overload as inline wrapper for backward compatibility with DPDK structs.

2. **Replaced C snprintf with std::format_to**: `format_ipv4()` and `format_mac()` now use `std::format_to_n` instead of `snprintf` for type-safety and consistency with the codebase's C++23 style.

3. **Deduplicated `random_ephemeral_port()`**: Moved IANA ephemeral port constants (`kEphemeralPortMin`, `kEphemeralPortRange`) to `net_header.hpp` as shared constants. Updated `dns.hpp::detail` and `connector.hpp` to reference the shared constants instead of defining their own.

4. **Added `[[nodiscard]]` to `try_parse_dns_packet`**: Missing attribute on a function that returns an optional result.

## Tests Added (5 new)
- `ChecksumIsConstexpr`: static_assert with RFC 1071 example
- `ConstexprChecksumOddLength`: static_assert for odd-byte input
- `ConstexprChecksumEmpty`: static_assert for null/zero input
- `EphemeralPortConstants`: Validates IANA range and power-of-2 constraint
- `ChecksumConsistencyAcrossSizes`: Verifies checksum self-validation property for sizes 0-32

## Benchmark Results (checksum, unchanged throughput)
```
BM_Checksum/64    6.75 ns   ~8.8 GiB/s
BM_Checksum/128  12.4  ns   ~9.6 GiB/s
BM_Checksum/256  25.1  ns   ~9.5 GiB/s
BM_Checksum/512  46.8  ns  ~10.2 GiB/s
BM_Checksum/1024 96.3  ns   ~9.9 GiB/s
```
No regression from the byte-reconstruction approach.
