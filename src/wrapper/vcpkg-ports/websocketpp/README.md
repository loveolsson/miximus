# WebSocket++ Linux package parity

Uses the exact source archives for Linux package `0.8.2+git20250909-2`, whose headers report `0.8.3-dev`:

- `websocketpp_0.8.2+git20250909.orig.tar.gz`
- `websocketpp_0.8.2+git20250909-2.debian.tar.xz`

Both archives come from the Ubuntu archive and are pinned by SHA-512 in `portfile.cmake`.
Their SHA-256 values were checked against the distribution's `.dsc` source descriptor:
`b11f7f9181e7d3cbf9a82002f8f350b6e77153faa94a43c36e334bc3db214cab` and
`330ab950fbda8918bcaa23bed645d9d77cc62a214fe8f0214974ebe67fbf5fc3`, respectively.

The recipe applies the distribution's complete, unchanged `1190.patch`. The pinned
`debian/patches/series` contains only that patch. No locally rebased Asio or C++20
patch is used. Both upstream and distribution licensing are installed with the headers.

Boost packages use the separate vcpkg `2026.01.16` registry pin for Boost 1.90.0.
This matches the Linux Boost release; it does not reproduce Ubuntu's `1.90.0-6ubuntu1`
packaging patches or compiler/ABI on Windows.

The header-install recipe derives from vcpkg's websocketpp port; its MIT license is
retained in `vcpkg-LICENSE.txt`.
