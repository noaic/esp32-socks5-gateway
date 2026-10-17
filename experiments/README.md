# Inactive experiments

These modules are retained for host experiments and are **not included in the
firmware's CMake source list**. Passing their tests does not establish that the
firmware implements the experimental behavior.

| Files | Purpose | Firmware behavior |
| --- | --- | --- |
| `packet_buffer.c/.h` | Reuse an exclusively owned input pbuf instead of cloning it | The current input path keeps its existing ownership/copy behavior |
| `udp_delivery.c/.h` | Bounded retries after a locally rejected UDP output | The current callback-queue failure path drops the reply; it does not enable this retry pool |

The regular host suite includes the explicitly named `ExperimentalBufferTests`
and `test_experimental_output_retry_reaches_client_once_without_upstream_resend`
to keep these experiments reviewable. Other UDP relay tests leave the retry
adapter disabled. The original UDP experiment and its regression are described
in [the historical record](../docs/history/udp10.md).

For the separate deterministic UDP pool harness, run on a host with GCC:

```bash
gcc -std=c11 -Wall -Wextra -Werror -I main -I experiments tests/udp_delivery_harness.c experiments/udp_delivery.c -o /tmp/esp-proxy-udp-delivery-test
/tmp/esp-proxy-udp-delivery-test
```

Moving these files does not change the forwarding implementation or reduce the
current firmware size. Re-enabling an experiment requires its own code review
and hardware validation; this directory is not an alternate supported build.
