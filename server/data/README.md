# Data shipped with the server

## common-passwords.txt

The passwords a Global server's accounts refuse at signup, a password change and a recovery
(`src/password_accounts.h`, `CommonPasswordsFile`; docs/HOSTED_SERVER.md, "Global servers:
accounts"). Read as plain data: one a line, compared in lower case; lines shorter than 10
characters are skipped (too short anyway).

| | |
| --- | --- |
| Source | [SecLists](https://github.com/danielmiessler/SecLists), `Passwords/Common-Credentials/xato-net-10-million-passwords-100000.txt` (the "10 million passwords, top 100 000" list) |
| Commit | `35d74839e66f44d116e694369d86a5bf07aaed6f` (2026-11-03) |
| SHA-256 | `1472aafa2561df5e3293aee252aee3ca660c12b399a283cf808bb01b39be388b` |
| Size | 781 879 bytes, 100 000 lines, ASCII |
| Licence | MIT, Copyright (c) 2018 Daniel Miessler (`LICENSES/MIT.txt`) |

To update it, take the file from a pinned commit (never `master`), check its licence is still
MIT, and record the new commit and SHA-256 here.

SecLists' licence:

    MIT License

    Copyright (c) 2018 Daniel Miessler

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
