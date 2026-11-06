# Store / TENT / MemFabric transfer tracing

Set `MC_MF_TRACE=1` before starting the Store client / vLLM worker processes.
The switch is read once in each process. Traces use INFO logs with the
`[MF_TRACE]` prefix and do not require `TENT_METRICS_ENABLED` or global verbose
logging. Leave `GLOG_v` at its normal value: existing verbose logs can print
on every polling iteration.

All durations are monotonic wall-clock microseconds. Enable tracing for a
small diagnostic run, since clocks, counters, and log output add overhead.

## Correlation

`store_get` creates a process-local `get_id` for each
`RealClient::batch_get_into_multi_buffers_internal` invocation. Nested Store
logs inherit it on the calling thread. `store_te_submit` maps that ID to a
`tent_batch_id`. TENT and MemFabric logs use `tent_batch_id`, including work
dispatched on another thread. `memfabric_copy` maps it to `native_batch_id`.
Every trace includes `pid`. Correlate IDs within one client process; IDs can
be reused across restarts.

## Stages

| `stage` | Fields to inspect |
| --- | --- |
| `store_get` | `master_query_us`, `select_and_slices_us`, `dedup_and_plan_us`, `batch_get_us`, `fanout_us`, key counts and successful bytes |
| `client_batch_get` | `prepare_and_submit_us`, `future_wait_us`, `checksum_us`, `wait_and_postprocess_us`, per-key and grouped submit counts, hot cache hits |
| `store_submit_batch` | Replica count, request count, `local_memcpy`, aggregate `open_segment_us`, preparation and submission times |
| `store_submit_slices` | Per-key slice count, `open_segment_us`, request preparation time |
| `store_te_submit` | `get_id`, `tent_batch_id`, request count, bytes, allocation, conversion and TENT submission times |
| `tent_submit` | `tent_batch_id`, `input_requests`, merged `physical_requests`, `retain_and_lock_us`, `prepare_merge_route_us`, `queued`, `dispatch_or_enqueue_us` |
| `transfer_wait` | `tent_batch_id`, public task count, polling rounds, actual calls to TENT `getTransferStatus`, aggregate status call time, time until the future result |
| `memfabric_copy` | `tent_batch_id`, `native_batch_id`, peer segment ID, protocol, direction, logical request count, native slice count, bytes, API duration and return code |
| `memfabric_batch` | Per-sub-batch totals for transport submissions, native copy/status calls, pending queries, errors, API durations, lock wait and observed completion |

Scope logs are emitted on return, including early returns; fields for stages
not reached are absent. `transfer_wait` and `memfabric_batch` summaries are
emitted when their state is destroyed, which can occur after completion.
`transfer_wait.total_us` includes state lifetime and cleanup; `until_result_us`
stops when the result is set. `tent_status_calls` counts only TENT calls.

## Native API counts

For each `tent_batch_id`, inspect `memfabric_batch`:

- `transport_submit_calls`: invocations of MemFabric `submitTransferTasks`.
- `smem_batch_copy_calls`: actual attempts to call `smem_trans_batch_copy`,
  including failed submissions. Within one transport submission there is one
  call per `(destination URL, READ/WRITE)` group.
- `native_slices`: total address-array entries passed to those copy calls.
  These can exceed request count because ranges are split at both endpoints'
  2 MiB registration boundaries. Slice count is not API call count.
- `smem_status_query_calls`: actual `smem_trans_batch_status_query` calls.
  `transport_status_calls` also includes calls served from completed task state.
  `pending_queries` counts native queries that still report pending. These
  fields are accumulated without printing per-query logs.
- `copy_api_us` measures native submission, including any synchronous
  connection setup inside MemFabric. `prepare_us` covers other time in
  successful transport submissions, including grouping, splitting and trace
  output. `resolve_dest_us` measures destination metadata resolution within
  that preparation; `submit_lock_wait_us` is reported separately.
- `observed_completion_us` runs from the first transport submission until
  all accepted native batches are observed terminal. It includes polling
  delay and does not measure DMA time alone. `-1` means completion was not
  observed. Inspect error counters and `pending_native_batches` as well.
  `query_errors` counts native query API failures; `failed_native_batches`
  counts terminal native batches whose transfer failed.

`bytes` and `native_slices` count attempted native submissions, including
failures. `success_bytes` in `store_get` counts successful logical results,
including locally duplicated keys. Runtime queue dispatch or retries can
produce several native calls under the same TENT batch ID; sum any sub-batch
summaries with that ID in the same process.

These counters measure the Mooncake-to-MemFabric C API boundary. Counting
MemFabric's internal HyBM / HCOM calls requires instrumentation in MemFabric.

## Validation on a Linux build host

Configure Mooncake with `USE_TENT=ON`, `USE_MEMFABRIC=ON` and
`BUILD_UNIT_TESTS=ON`, then build `tent_memfabric_transport_test`. Its ACL and
MemFabric mocks do not require an NPU. Run both modes in separate processes:

```bash
env -u MC_MF_TRACE ./build/mooncake-transfer-engine/tent/tests/tent_memfabric_transport_test
MC_MF_TRACE=1 ./build/mooncake-transfer-engine/tent/tests/tent_memfabric_transport_test
```

`TraceCountsNativeGroupsSplitsAndPendingQueries` checks that three requests
produce four native slices, two copy API calls and four native status queries,
while repeated polling of an already completed task avoids another query.
