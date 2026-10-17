# Maintenance and recovery

Set the management password through the AP-local first-use page. Configuration reads do not return Wi-Fi or management passwords. Ordinary reset or application-only flashing normally preserves NVS; writing the merged precompiled `factory.bin` clears it because the image covers the NVS address range. Saved network/password changes require confirmation within 180 seconds after restarting; otherwise they roll back. After a subnet change, a phone may retain its old DHCP lease until reconnecting.

## Networks and status

Choose a different AP third octet when the upstream network overlaps `192.168.X.0/24`. This project does not implement NAT.

UART runs at 115200 baud. `status` is a current state/resource snapshot; `events` is a bounded history of significant events; `log` controls live diagnostic output. The everyday log level is WARN. Use INFO temporarily and return to WARN afterward. Full text logs are not stored in Flash.

The firmware displays `gateway> ` and echoes accepted ASCII characters. Backspace/Delete removes the last character; Enter submits once, including CRLF. Do not type the prompt itself. Disable terminal local echo to avoid duplicate characters; use `log warn`/`log off` if logs interrupt the input line. Input is bounded to 95 bytes. State-changing/recovery commands have a one-second rate limit; `help`, `status`, `events`, argument-free `log`, and `cancel` can run immediately. Arrow-key/history editing is unavailable.

The LED's green Internet state only describes the recent DNS/HTTP check target. An accepted maintenance task is not proof of restored Internet connectivity. Web operation results match task IDs; restart completion matches a changed boot identifier. A missing response does not cause an automatic repeat POST. Network saves and queued operations stop waiting after 90 seconds and offer the new address when the subnet changes. Lost immediate-setting/action responses do not lock subsequent controls or network confirmation: status polling observes the target state, without automatically resending the request or proving which request produced it.

Status refresh preserves unsaved Internet-check choices, and events follow the page language. HTTP requests have a 5-second network receive/send deadline and 1024-byte body limit; rejected/incomplete bodies close the connection rather than entering the SDK's leftover-body drain. Rollback still runs in the HTTP task, so network waits are bounded rather than promising exact real-time recovery.

## Recovery commands

Send `reset-admin`, read the displayed effect and code, then send `confirm` followed by that code within 30 seconds. It keeps the network currently used by this boot, logically clears the password in both active/backup records, cancels a trial and restarts. Set a new password on the first-use page.

`reset-network` uses the same confirmation mechanism. It keeps this boot's management password and restores compiled network defaults: STA/AP credentials, SOCKS, DNS, AP name, max two clients, and `192.168.4.0/24`. Compiled defaults may contain credentials you entered in menuconfig; recovery does not mean every Wi-Fi password becomes empty. LED/check preferences remain stored.

Codes are invalid after cancellation, error, expiration or use. Busy/rejected/failed submissions are not successful resets; inspect the returned message and status before trying again. Logical reset does not securely erase old Flash pages.

RST/EN only restarts; BOOT retains its normal boot function. Avoid `erase-flash` for routine recovery.

Flashing, monitor and capture must not simultaneously own the same UART. On Linux/WSL2, `python tools/uart_capture.py --port /dev/ttyACM0 --no-reset --interactive --timestamps --seconds 0` combines capture and line-based command input. Timestamped capture displays prompts before a newline. `--interactive` stdin is POSIX-only; Windows users should use `idf.py -p COM5 monitor` for commands, or capture logs with `--port COM5` without `--interactive`. Adapter/kernel behavior may still affect reset lines; the flag prevents the tool's own active reset. See [tool usage](../tools/README.md).

## lwIP override

`python tools/prepare_lwip.py restore` reverses the verified project-local `ip4.c` patch, leaving global IDF untouched; Linux can also use `python3`. This offline operation does not check the whole component. Transparent mode needs the patch; run `apply` and `check` before building it again. These commands check both pinned revisions and tracked SDK component contents, accepting text LF/CRLF differences but rejecting actual edits, missing tracked files, unexpected files in the local override and symlinks. SDK-local untracked caches are ignored and not copied. Back up and review unexpected source edits instead of forcing an overwrite.

## Tested boundary

Early hardware/NVS initialization failure can prevent the serial worker from starting; its recovery commands cannot fix every boot failure. Inspect the boot log and verify the required N16R8 hardware first. The existing startup code erases NVS when initialization returns `ESP_ERR_NVS_NO_FREE_PAGES` or `ESP_ERR_NVS_NEW_VERSION_FOUND`, then retries; this exception can clear settings despite an ordinary application flash preserving the NVS partition.

Real-board `reset-admin` successfully allowed password setup and browsing. `reset-network` subsequently kept that password, restored the default management address and restored Internet access. These tests cover ordinary commit/restart, not interrupted Flash writes or hardware storage failure.
