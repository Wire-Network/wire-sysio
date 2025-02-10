---
content_title: Kiod Troubleshooting
---

## How to solve the error "Failed to lock access to wallet directory; is another `kiod` running"?

Since `clio` may auto-launch an instance of `kiod`, it is possible to end up with multiple instances of `kiod` running. That can cause unexpected behavior or the error message above.

To fix this issue, you can terminate all running `kiod` instances and restart `kiod`. The following command will find and terminate all instances of `kiod` running on the system:

```sh
pkill kiod
```
