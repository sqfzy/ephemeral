# eph-net Benchmark History

## 2026-04-03 — HttpClient::Config URL parsing/serialization baseline

Source: `/bench` after adding `from_url()` and `to_url()`
Commit: (current dev HEAD)
CPU: 16x ARM64 @ 2000 MHz, L3 36864 KiB

| Benchmark | Time (ns) | CPU (ns) | Iterations |
|-----------|-----------|----------|------------|
| BM_BuildHttpRequest_Get | 57.2 | 57.2 | 12,219,013 |
| BM_BuildHttpRequest_Post | 159 | 159 | 4,399,183 |
| BM_ParseHttpResponse_Json | 82.2 | 82.2 | 8,519,143 |
| BM_ParseHttpResponse_LargeBody | 88.0 | 88.0 | 7,955,052 |
| BM_FindHeader | 81.9 | 81.9 | 8,548,831 |
| BM_FindHeader_LastHeader | 71.0 | 71.0 | 9,865,016 |
| BM_FindHeaderOpt | 78.0 | 78.0 | 8,975,648 |
| BM_FindHeader_Miss | 23.5 | 23.5 | 29,731,130 |
| BM_HttpResponse_ToJson | 290 | 290 | 2,417,032 |
| BM_HttpClientConfig_Validate | 7.15 | 7.15 | 97,898,398 |
| BM_HttpClientConfig_FromUrl_Https | 32.5 | 32.5 | 21,589,853 |
| BM_HttpClientConfig_FromUrl_HttpsWithPortAndPath | 47.8 | 47.8 | 14,650,451 |
| BM_HttpClientConfig_FromUrl_Http | 31.2 | 31.2 | 22,437,623 |
| BM_HttpClientConfig_FromUrl_Ipv6 | 27.9 | 27.9 | 25,123,438 |
| BM_HttpClientConfig_ToUrl_DefaultPort | 72.5 | 72.5 | 9,665,760 |
| BM_HttpClientConfig_ToUrl_NonDefaultPort | 106 | 106 | 6,602,287 |
| BM_HttpClientConfig_ToUrl_Ipv6 | 106 | 106 | 6,598,767 |
