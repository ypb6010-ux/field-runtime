<!--
SPDX-FileCopyrightText: 2026 ypb6010-ux
SPDX-License-Identifier: MPL-2.0
-->

# Contributing to FieldRuntime

Thanks for your interest in contributing. This document explains the
licensing terms for contributions and the basic workflow.

## Licensing of contributions

FieldRuntime is released under the **Mozilla Public License 2.0
(MPL-2.0)**. Unless explicitly stated otherwise, every contribution you
submit is provided under the MPL-2.0.

This project is maintained under an **open-core / dual-licensing** model:
a community edition under MPL-2.0, and a separate commercial license for
parties who need to modify MPL-covered files without publishing those
modifications (see [`COMMERCIAL-LICENSE.md`](COMMERCIAL-LICENSE.md)).

To make dual licensing possible, **all contributors must sign the
Contributor License Agreement (CLA)** before their contribution can be
merged. See [`CLA.md`](CLA.md). The CLA does not transfer your copyright —
you retain ownership of your work — but it grants the maintainer the
rights needed to license the project under both MPL-2.0 and a commercial
license.

> If you cannot or do not wish to sign the CLA, you can still open issues,
> propose designs, and review code. Only code/asset contributions that are
> merged into the tree require a signed CLA.

## SPDX headers

Every source file must carry an SPDX header at the very top:

```cpp
// SPDX-FileCopyrightText: <year> <your name or handle>
// SPDX-License-Identifier: MPL-2.0
```

New files you author should use your own `SPDX-FileCopyrightText` line.
Do not remove or alter existing copyright/license headers.

## Dependency policy

The **core runtime** keeps its dependencies to permissive licenses
(MIT / BSD / Apache-2.0 / MPL-2.0). Do not add GPL/LGPL/AGPL dependencies
to the core build. GPL/LGPL components are only acceptable behind an
**optional** build switch and confined to an isolated module (as QxOrm is
for the persistence module). Any new third-party dependency must be added
to [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md) in the same PR.

## Workflow

### Long-lived branches

- `main` is the Qt Core runtime and QML integration mainline.
- `core-base-split` owns the Qt-free `FieldRuntimeBase`, gateway, and Web Console.
- Keep the two branches functionally independent. Port shared runtime fixes as
  focused commits with branch-specific tests; do not merge one long-lived branch
  wholesale into the other.

1. Open an issue describing the change for anything non-trivial.
2. Fork and create a topic branch.
3. Keep the build green: `CORE_BUILD_TESTS=ON` and run the test suite.
4. Add an SPDX header to every new file.
5. Update docs / `THIRD-PARTY-NOTICES.md` when relevant.
6. Open a PR; confirm in the PR that you have signed the CLA.

## Reporting security issues

Do **not** open public issues for security vulnerabilities. Follow the
process in [`SECURITY.md`](SECURITY.md).
