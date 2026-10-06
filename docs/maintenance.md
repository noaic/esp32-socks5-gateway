# Maintenance and recovery

Set the management password through the AP-local first-use page. Configuration reads do not return Wi-Fi or management passwords. Ordinary reset or application flashing preserves NVS. Saved network/password changes require confirmation within 180 seconds after restarting; otherwise they roll back. After a subnet change, a phone may retain its old DHCP lease until reconnecting.

## Networks and status

Choose a different AP third octet when the upstream network overlaps `192.168.X.0/24`. This project does not implement NAT.

UART runs at 115200 baud. `status` is a current state/resource snapshot; `events` is a bounded history of significant events; `log` controls live diagnostic output. The everyday log level is WARN. Use INFO temporarily and return to WARN afterward. Full text logs are not stored in Flash.

The LED's green Internet state only describes the recent DNS/HTTP check target. An accepted maintenance task is not proof of restored Internet connectivity. Web operation results match task IDs; restart completion matches a changed boot identifier. A missing response does not cause an automatic repeat POST. The page stops waiting after 90 seconds and offers the new address when the subnet changes.

## Recovery commands

Send `reset-admin`, read the displayed effect and code, then send `confirm` followed by that code within 30 seconds. It keeps the network currently used by this boot, logically clears the password in both active/backup records, cancels a trial and restarts. Set a new password on the first-use page.

`reset-network` uses the same confirmation mechanism. It keeps this boot's management password and restores compiled network defaults: STA/AP credentials, SOCKS, DNS, AP name, max two clients, and `192.168.4.0/24`. Compiled defaults may contain credentials you entered in menuconfig; recovery does not mean every Wi-Fi password becomes empty. LED/check preferences remain stored.

Codes are invalid after cancellation, error, expiration or use. Busy/rejected/failed submissions are not successful resets; inspect the returned message and status before trying again. Logical reset does not securely erase old Flash pages.

RST/EN only restarts; BOOT retains its normal boot function. Avoid `erase-flash` for routine recovery.

Flashing, monitor and capture must not simultaneously own the same UART. `tools/uart_capture.py --no-reset --interactive --timestamps` combines log capture and command input through one connection. Adapter/kernel behavior may still affect reset lines; the flag prevents the tool's own active reset.

## lwIP override

`python3 tools/prepare_lwip.py restore` reverses the project-local patch, leaving global IDF untouched. Transparent mode needs the patch; run `apply` and `check` before building it again. Unexpected local edits or a different pinned revision cause the tool to refuse overwriting files.

## Tested boundary

Real-board `reset-admin` successfully allowed password setup and browsing. `reset-network` subsequently kept that password, restored the default management address and restored Internet access. These tests cover ordinary commit/restart, not interrupted Flash writes or hardware storage failure.
